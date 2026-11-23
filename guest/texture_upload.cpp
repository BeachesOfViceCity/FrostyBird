#define NOMINMAX
#include "texture_upload.hpp"
#include "play_timing.hpp"
#include "depth_alias.hpp"
#include "tile_plan.hpp"
#include "xg_layout.hpp"
#include "gpu_detile.hpp"
#include "texture_mapping.hpp"
#include <algorithm>
#include <bit>
#include <atomic>
#include <array>
#include <cstring>
#include <vector>
#include <memory>

namespace {
// xg_xs from EmbeddedXvd, as used by Frosty's own layout calculation.
// This interface has AddRef/Release at slots 0/1, not an IUnknown prefix.
using Computer=XgComputer;
enum class Backing { invalid, sparse, resident };
Backing backing(const BYTE* base,UINT64 bytes) {
    auto pos=reinterpret_cast<UINT_PTR>(base);
    if (!base || !bytes || bytes>(2ull<<30) || bytes>UINTPTR_MAX-pos) return Backing::invalid;
    auto end=pos+bytes;
    bool sparse=false;
    while(pos<end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(pos),&mbi,sizeof(mbi))) {
            sparse=true;
            pos=(pos|0xffffull)+1;
            continue;
        }
        if (mbi.Type==MEM_IMAGE) return Backing::invalid;
        sparse |= mbi.State!=MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS|PAGE_GUARD));
        pos=reinterpret_cast<UINT_PTR>(mbi.BaseAddress)+mbi.RegionSize;
    }
    return sparse ? Backing::sparse : Backing::resident;
}
bool block_format(DXGI_FORMAT f) {
    return (f>=DXGI_FORMAT_BC1_TYPELESS && f<=DXGI_FORMAT_BC5_SNORM) ||
           (f>=DXGI_FORMAT_BC6H_TYPELESS && f<=DXGI_FORMAT_BC7_UNORM_SRGB);
}
// VirtualQuery describes a mapping; it does not retain it. In particular a
// parallel streaming worker can free/protect ordinary CPU allocation pages
// before memcpy. Detile from checked, owned page windows. A failed snapshot
// leaves the mip pending instead of publishing stale or partial contents.
class SourcePages {
    static constexpr SIZE_T page=65536;
    struct Page{UINT_PTR lo{},hi{};std::unique_ptr<BYTE[]> data;};
    std::array<Page,32> pages;
    UINT_PTR base,end;
public:
    SourcePages(const BYTE* p,UINT64 n):base(reinterpret_cast<UINT_PTR>(p)),end(base+n){}
    bool copy(UINT64 offset,BYTE* dest,SIZE_T count,UINT64& hole){
        auto at=base+offset;
        while(count){
            auto& p=pages[(at/page)%pages.size()];
            if(at<p.lo || at>=p.hi){
                MEMORY_BASIC_INFORMATION m{};hole=at-base;
                if(!VirtualQuery(reinterpret_cast<const void*>(at),&m,sizeof(m)) || m.State!=MEM_COMMIT ||
                   m.Type==MEM_IMAGE || m.Protect&(PAGE_NOACCESS|PAGE_GUARD))return false;
                auto region=reinterpret_cast<UINT_PTR>(m.BaseAddress),boundary=at&~UINT_PTR(page-1);
                auto lo=std::max({base,boundary,region}),hi=std::min({end,boundary+page,region+m.RegionSize});
                if(lo>at || hi<=at)return false;
                if(!p.data)p.data=std::make_unique_for_overwrite<BYTE[]>(page);
                p.hi=0;SIZE_T got{};
                if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(lo),p.data.get(),hi-lo,&got) || got!=hi-lo)return false;
                p.lo=lo;p.hi=hi;
            }
            auto n=std::min<SIZE_T>(count,p.hi-at);std::memcpy(dest,p.data.get()+(at-p.lo),n);
            dest+=n;at+=n;count-=n;
        }
        return true;
    }
};
struct RegionFree {void operator()(BYTE* p)const{if(p)VirtualFree(p,0,MEM_RELEASE);}};
using RegionBytes=std::unique_ptr<BYTE,RegionFree>;
RegionBytes private_bytes(SIZE_T size){return RegionBytes(static_cast<BYTE*>(VirtualAlloc(nullptr,size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)));}
struct UploadReport{
    UINT64 begin{},lookup{},decode{},source{},bytes{},original{};UINT gpu{},bulk{},planned{},direct{},dense{};
    bool sparse{};
    UINT64 tick()const{return begin?play_timing::now():0;}
    ~UploadReport(){
        if(!begin)return;
        static const double to_ms=[](){LARGE_INTEGER f{};QueryPerformanceFrequency(&f);return 1000./f.QuadPart;}();
        auto ms=(tick()-begin)*to_ms;if(ms<60.)return;
        FILETIME ft{};GetSystemTimePreciseAsFileTime(&ft);
        auto us=(((UINT64(ft.dwHighDateTime)<<32)|ft.dwLowDateTime)-116444736000000000ull)/10;
        std::fprintf(stderr,"frosty/upload-latency: %.2f ms, source/footprint %llu/%llu KiB sparse=%d, gpu/bulk/plan/direct/dense %u/%u/%u/%u/%u, lookup %.2f snapshot %.2f detile %.2f ms end_us=%llu\n",
            ms,original/1024,bytes/1024,sparse,gpu,bulk,planned,direct,dense,lookup*to_ms,source*to_ms,decode*to_ms,us);
    }
};
D3D12_HEAP_PROPERTIES props(D3D12_HEAP_TYPE t) {
    D3D12_HEAP_PROPERTIES p{};
    p.Type=t;
    p.CreationNodeMask=p.VisibleNodeMask=1;
    return p;
}
// Lifetime is tied to the host image through SetPrivateDataInterface. No
// owning reference back to that image. Recheck sparse mips when an SRV is
// created (the SDK changes view ranges as it streams mips in).
constexpr GUID placement_key{0x683e5bb6,0xb31f,0x41ba,{0x94,0x42,0x3e,0x5e,0x10,0xbe,0x5f,0xa0}};
struct Placed final : IUnknown {
    std::atomic<ULONG> refs{1};
    Microsoft::WRL::ComPtr<Computer> computer;
    const BYTE* address{};
    D3D12_RESOURCE_DESC desc{},xs{};
    D3D12_RESOURCE_STATES state{};
    std::vector<bool> uploaded;
    std::vector<bool> confirmed;
    // An absent byte is a witness that the entire subresource cannot yet
    // be uploaded. Recheck that actual mapping before doing any layout,
    // allocation or detiling on a repeated bind. It is not a time-based
    // throttle: newly mapped mips are eligible on the very next bind.
    std::vector<UINT64> hole;
    std::mutex image_lock;
    std::shared_ptr<TextureAccess> access=std::make_shared<TextureAccess>();
    HRESULT WINAPI QueryInterface(REFIID id,void** out) override {
        if (!out) return E_POINTER;
        *out=nullptr;
        if (id!=__uuidof(IUnknown)) return E_NOINTERFACE;
        *out=this; AddRef(); return S_OK;
    }
    ULONG WINAPI AddRef() override {return ++refs;}
    ULONG WINAPI Release() override {auto n=--refs;if(!n)delete this;return n;}
};
}

HRESULT XboxTextures::load(ID3D12Device* dev,const BYTE* address,const D3D12_RESOURCE_DESC& xs,
                          D3D12_RESOURCE_STATES state,const D3D12_CLEAR_VALUE* clear,REFIID iid,void** out) try {
    if (!out) return E_POINTER;
    *out=nullptr;
    if (xs.Dimension<D3D12_RESOURCE_DIMENSION_TEXTURE1D || xs.Dimension>D3D12_RESOURCE_DIMENSION_TEXTURE3D ||
        !xs.MipLevels || !xs.DepthOrArraySize || xs.MipLevels>15 || xs.SampleDesc.Count!=1)
        return E_NOTIMPL;
    auto desc=desktop_desc(xs);
    auto def=props(D3D12_HEAP_TYPE_DEFAULT);
    if (desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) {
        // Render-output surfaces are GPU-produced. Keep the VkImage in the
        // host's tiling; do not reinterpret nonportable color/depth metadata.
        auto hr=dev->CreateCommittedResource(&def,D3D12_HEAP_FLAG_NONE,&desc,state,clear,iid,out);
        if(SUCCEEDED(hr) && *out && iid==__uuidof(ID3D12Resource))tag_depth_alias(static_cast<ID3D12Resource*>(*out),xs);
        return hr;
    }
    D3D12_FEATURE_DATA_FORMAT_INFO info{xs.Format,0};
    if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO,&info,sizeof(info))) || info.PlaneCount!=1)
        return E_NOTIMPL;
    Ptr<Computer> computer;
    auto hr=xg_computer(xs,computer.GetAddressOf());
    if (FAILED(hr)) return hr;
    auto source_size=computer->v->size(computer.Get());
    if (backing(address,source_size)==Backing::invalid) return E_INVALIDARG;
    UINT layers=desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize;
    UINT n=layers*desc.MipLevels;
    Ptr<ID3D12Resource> texture;
    if (FAILED(hr=dev->CreateCommittedResource(&def,D3D12_HEAP_FLAG_NONE,&desc,state,clear,IID_PPV_ARGS(&texture)))) return hr;
    Ptr<Placed> metadata;
    metadata.Attach(new Placed);
    metadata->computer=std::move(computer);
    metadata->address=address;
    metadata->desc=desc;
    metadata->xs=xs;
    metadata->state=state;
    metadata->uploaded.resize(n);
    metadata->hole.assign(n,UINT64_MAX);
    if(!(desc.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
        metadata->confirmed.resize(n);
    if (FAILED(hr=texture->SetPrivateDataInterface(placement_key,metadata.Get())) || FAILED(hr=refresh(dev,texture.Get()))) return hr;
    return texture->QueryInterface(iid,out);
} catch(const std::bad_alloc&) {return E_OUTOFMEMORY;}

std::shared_ptr<TextureUse> XboxTextures::shader_view(ID3D12Resource* image,const D3D12_SHADER_RESOURCE_VIEW_DESC* d){
    Ptr<Placed> p;UINT n=sizeof(Placed*);
    if(!image || FAILED(image->GetPrivateData(placement_key,&n,p.GetAddressOf())) || !p || p->access->settled.load())return {};
    auto use=std::make_shared<TextureUse>();use->access=p->access;
    if(p->confirmed.empty()){use->ready=true;return use;}
    use->pending=image;
    UINT levels=p->desc.MipLevels,slices=p->desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE3D?1:p->desc.DepthOrArraySize;
    use->mip_count=levels;use->layers=slices;
    if(d)switch(d->ViewDimension){
        case D3D12_SRV_DIMENSION_TEXTURE1D:use->first_mip=d->Texture1D.MostDetailedMip;use->mip_count=d->Texture1D.MipLevels;break;
        case D3D12_SRV_DIMENSION_TEXTURE2D:use->first_mip=d->Texture2D.MostDetailedMip;use->mip_count=d->Texture2D.MipLevels;break;
        case D3D12_SRV_DIMENSION_TEXTURE3D:use->first_mip=d->Texture3D.MostDetailedMip;use->mip_count=d->Texture3D.MipLevels;break;
        case D3D12_SRV_DIMENSION_TEXTURECUBE:use->first_mip=d->TextureCube.MostDetailedMip;use->mip_count=d->TextureCube.MipLevels;use->layers=6;break;
        case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:use->first_mip=d->Texture1DArray.MostDetailedMip;use->mip_count=d->Texture1DArray.MipLevels;use->first_layer=d->Texture1DArray.FirstArraySlice;use->layers=d->Texture1DArray.ArraySize;break;
        case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:use->first_mip=d->Texture2DArray.MostDetailedMip;use->mip_count=d->Texture2DArray.MipLevels;use->first_layer=d->Texture2DArray.FirstArraySlice;use->layers=d->Texture2DArray.ArraySize;break;
        case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:use->first_mip=d->TextureCubeArray.MostDetailedMip;use->mip_count=d->TextureCubeArray.MipLevels;use->first_layer=d->TextureCubeArray.First2DArrayFace;
            use->layers=d->TextureCubeArray.NumCubes>UINT_MAX/6?UINT_MAX:d->TextureCubeArray.NumCubes*6;break;
        default:return {};
    }
    return use;
}
std::shared_ptr<TextureAccess> XboxTextures::access(ID3D12Resource* image){
    if(!image)return {};
    auto d=image->GetDesc();
    if(d.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER || d.Flags&(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))return {};
    Ptr<Placed> p;UINT n=sizeof(Placed*);
    if(FAILED(image->GetPrivateData(placement_key,&n,p.GetAddressOf())) || !p || p->access->settled.load())return {};
    return p->access;
}
HRESULT XboxTextures::reap_locked(){
    auto done=fence_?fence_->GetCompletedValue():0;
    if(done==UINT64_MAX)return DXGI_ERROR_DEVICE_REMOVED;
    while(!flight_.empty() && flight_.front().value<=done){
        auto& b=flight_.front();
        for(auto& a:b.images)if(a->immutable && a->epoch<=done){a->settled=true;a->readers.clear();}
        in_flight_-=b.bytes;flight_.pop_front();
    }
    return S_OK;
}
HRESULT XboxTextures::begin_locked(ID3D12Device* dev){
    HRESULT hr;
    D3D12_COMMAND_QUEUE_DESC qd{};
    if(!queue_ && FAILED(hr=dev->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue_))))return hr;
    if(!fence_ && FAILED(hr=dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence_))))return hr;
    if(FAILED(hr=reap_locked()))return hr;
    if(!pending_.list){
        if(FAILED(hr=dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&pending_.alloc))) ||
           FAILED(hr=dev->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,pending_.alloc.Get(),nullptr,IID_PPV_ARGS(&pending_.list))))return hr;
    }
    return S_OK;
}
HRESULT XboxTextures::flush_locked(){
    if(!pending_.list)return S_OK;
    HRESULT hr=pending_.list->Close();if(FAILED(hr))return hr;
    ID3D12CommandList* lists[]{pending_.list.Get()};queue_->ExecuteCommandLists(1,lists);
    if(FAILED(hr=queue_->Signal(fence_.Get(),++serial_)))return hr;
    pending_.value=serial_;in_flight_+=pending_.bytes;flight_.push_back(std::move(pending_));pending_={};
    return reap_locked();
}
XboxTextures::~XboxTextures(){
    // An unsubmitted preamble has no consumers. Submitted allocations must
    // survive until the transfer queue signals, including on normal shutdown.
    if(fence_ && serial_ && fence_->GetCompletedValue()<serial_)fence_->SetEventOnCompletion(serial_,nullptr);
}
HRESULT XboxTextures::submit(ID3D12Device* dev,ID3D12CommandQueue* q,UploadConsumer& consumer,
                            std::span<const TextureReads* const> usage,UINT n,ID3D12CommandList* const* lists){
    if(std::ranges::none_of(usage,[](auto* u){return !u->images.empty();})){q->ExecuteCommandLists(n,lists);return S_OK;}
    std::lock_guard g(lock_);HRESULT hr;
    if(FAILED(hr=reap_locked()))return hr;
    UINT64 epoch=0;bool mutable_images=false;
    for(auto* u:usage)for(auto& a:u->images){
        epoch=std::max(epoch,a->epoch);mutable_images|=!a->immutable;
    }
    if(epoch>serial_ && FAILED(hr=flush_locked()))return hr;
    if(epoch>consumer.seen){
        if(fence_->GetCompletedValue()<epoch && FAILED(hr=q->Wait(fence_.Get(),epoch)))return hr;
        consumer.seen=epoch;
    }
    if(mutable_images && !consumer.fence && FAILED(hr=dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&consumer.fence))))return hr;
    q->ExecuteCommandLists(n,lists);
    if(mutable_images){
        if(FAILED(hr=q->Signal(consumer.fence.Get(),++consumer.serial)))return hr;
        for(auto* u:usage)for(auto& a:u->images)if(!a->immutable)a->readers[consumer.fence.Get()]={consumer.fence,consumer.serial};
    }
    return S_OK;
}
HRESULT XboxTextures::refresh(ID3D12Device* dev,ID3D12Resource* texture,TextureUse* use) try {
    Ptr<Placed> meta;
    UINT length=sizeof(Placed*);
    if (!texture || FAILED(texture->GetPrivateData(placement_key,&length,meta.GetAddressOf())) || !meta) return S_OK;
    // Different textures can be detiled concurrently by streaming workers.
    std::lock_guard guard(meta->image_lock);
    if(use && use->ready.load())return S_OK;
    const auto& desc=meta->desc;
    auto address=meta->address;
    auto& computer=meta->computer;
    auto state=meta->state;
    UINT n=UINT(meta->uploaded.size());
    UINT layers=n/desc.MipLevels;
    auto needed=[&](UINT i){
        if(!use)return !meta->uploaded[i];
        auto mip=i%desc.MipLevels,layer=i/desc.MipLevels;
        return mip>=use->first_mip && mip-use->first_mip<use->mip_count &&
               layer>=use->first_layer && layer-use->first_layer<use->layers && !meta->confirmed[i];
    };
    bool missing=false;
    for(UINT i=0;i<n;++i)if(needed(i)){missing=true;break;}
    if(!missing){if(use)use->ready=true;return S_OK;}
    std::vector<UINT> candidates;
    MEMORY_BASIC_INFORMATION last{};UINT_PTR region_lo{},region_hi{};
    for(UINT i=0;i<n;++i)if(needed(i)){
        auto h=meta->hole[i];
        if(h!=UINT64_MAX){
            auto at=reinterpret_cast<UINT_PTR>(address)+h;
            if(at<region_lo || at>=region_hi){
                if(!VirtualQuery(reinterpret_cast<const void*>(at),&last,sizeof(last)))continue;
                region_lo=reinterpret_cast<UINT_PTR>(last.BaseAddress);region_hi=region_lo+last.RegionSize;
            }
            if(last.Type==MEM_IMAGE)return E_INVALIDARG;
            if(last.State!=MEM_COMMIT || (last.Protect&(PAGE_NOACCESS|PAGE_GUARD)))continue;
        }
        candidates.push_back(i);
    }
    if(candidates.empty())return S_OK;
    play_timing::Scope time(play_timing::uploads);
    UploadReport report;report.begin=time.start;
    auto source_size=computer->v->size(computer.Get());
    auto resident=backing(address,source_size);if(resident==Backing::invalid)return E_INVALIDARG;
    report.original=source_size;report.sparse=resident==Backing::sparse;
    HRESULT hr;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layout(n);
    std::vector<UINT> rows(n);
    std::vector<UINT64> widths(n);
    UINT64 size{},gpu_bytes{};const UINT block=block_format(desc.Format)?4:1;
    std::vector<GpuTileWork> gpu(n);
    static const bool force_offsets=GetEnvironmentVariableA("FROSTY_FORCE_OFFSET_DETILE",nullptr,0)!=0;
    static const bool test_gpu=GetEnvironmentVariableA("FROSTY_TEST_GPU_DETILE",nullptr,0)!=0;
    for(UINT i:candidates){
        // A streamed view can request a single small mip of a large image.
        // Do not allocate the whole texture's footprint on each refresh.
        UINT64 bytes{};dev->GetCopyableFootprints(&desc,i,1,0,&layout[i],&rows[i],&widths[i],&bytes);
        if(!bytes)return E_INVALIDARG;
        bytes=std::max(bytes,UINT64(layout[i].Footprint.RowPitch)*rows[i]*layout[i].Footprint.Depth);
        auto mip=i%desc.MipLevels;auto cols=(std::max<UINT64>(1,desc.Width>>mip)+block-1)/block;
        if(!cols || !widths[i] || widths[i]%cols)return E_NOTIMPL;
        auto elem=widths[i]/cols;auto depth=layout[i].Footprint.Depth;
        if(!force_offsets && (bytes>=512u<<10 || test_gpu) && std::has_single_bit(elem) && elem<=16 && bytes<=128ull<<20){
            auto begin=report.tick();auto first=computer->v->offset(computer.Get(),0,mip,0,0,layers==1?0:i/desc.MipLevels,0,nullptr);
            auto key=tile_key(meta->xs,source_size,i,cols,rows[i],depth,UINT(elem),first);
            auto m=detiler_.view(dev,key,computer.Get(),test_gpu);report.lookup+=report.tick()-begin;
            if(m){
                begin=report.tick();
                if((resident==Backing::resident || backing(address+m->low,m->high-m->low)==Backing::resident) &&
                   detiler_.stage(dev,m,address,UINT(cols),rows[i]*depth,layout[i].Footprint.RowPitch,UINT(elem),gpu[i])){
                    ++report.gpu;gpu_bytes+=gpu[i].bytes;report.source+=report.tick()-begin;layout[i].Offset=0;continue;
                }
                report.source+=report.tick()-begin;
            }
        }
        size=(size+D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT-1)&~UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT-1);
        if(size>(2ull<<30) || bytes>(2ull<<30)-size)return E_INVALIDARG;
        layout[i].Offset=size;size+=bytes;
    }
    D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER,0,size,1,1,1,DXGI_FORMAT_UNKNOWN,
        {1,0},D3D12_TEXTURE_LAYOUT_ROW_MAJOR,D3D12_RESOURCE_FLAG_NONE};
    auto upload=props(D3D12_HEAP_TYPE_UPLOAD);
    Ptr<ID3D12Resource> staging;
    BYTE* mapped{};
    D3D12_RANGE no_reads{};
    if(size){
        if(FAILED(hr=dev->CreateCommittedResource(&upload,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,IID_PPV_ARGS(&staging))))return hr;
        if(FAILED(hr=staging->Map(0,&no_reads,reinterpret_cast<void**>(&mapped))))return hr;
    }
    report.bytes=size+gpu_bytes;
    RegionBytes tiled,linear;SIZE_T linear_size=0;
    // The Scarlett CopyFromSubresource ABI takes an extra flags word.
    // Give the packaged bulk detiler an aligned, owned and fully readable
    // snapshot. Sparse/partial residency still uses checked page windows.
    if(size && !force_offsets && resident==Backing::resident && source_size<=128ull<<20 && source_size<=2*size+65536 &&
       computer->v->alignment(computer.Get())<=65536){
        auto start=report.tick();tiled=private_bytes(source_size);
        if(tiled){MappingLease lease(address,source_size);SIZE_T got{};
            if(!ReadProcessMemory(GetCurrentProcess(),address,tiled.get(),source_size,&got) || got!=source_size)tiled.reset();}
        report.source+=report.tick()-start;
    }
    std::vector<UINT> ready;
    for (UINT i:candidates) {
        if(gpu[i].linear){ready.push_back(i);meta->hole[i]=UINT64_MAX;continue;}
        UINT mip=i%desc.MipLevels;
        auto& fp=layout[i].Footprint;
        UINT64 texels=std::max<UINT64>(1,desc.Width>>mip);
        UINT64 columns=(texels+block-1)/block;
        if (!widths[i] || widths[i]%columns) return E_NOTIMPL;
        auto elem=widths[i]/columns;
        if (elem>16) return E_NOTIMPL;
        auto pitched=UINT64(fp.RowPitch)*rows[i]*fp.Depth;
        if(tiled && pitched<=128ull<<20){
            auto start=report.tick();
            if(linear_size<pitched){linear=private_bytes(pitched);linear_size=linear?pitched:0;}
            if(linear && SUCCEEDED(computer->v->copy_from(computer.Get(),linear.get(),0,i,0,tiled.get(),fp.RowPitch,
                    desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE3D?fp.RowPitch*rows[i]:0))){
                // GPU-visible upload memory can be write-combined. Perform
                // scattered tile writes in ordinary RAM, then stream once.
                std::memcpy(mapped+layout[i].Offset,linear.get(),pitched);
                ++report.bulk;report.decode+=report.tick()-start;
                meta->hole[i]=UINT64_MAX;ready.push_back(i);continue;
            }
            report.decode+=report.tick()-start;
        }
        auto offset_of=[&](UINT64 x,UINT y,UINT z){return computer->v->offset(computer.Get(),0,mip,x,y,layers==1?z:i/desc.MipLevels,0,nullptr);};
        auto first=offset_of(0,0,0);if(first>source_size || elem>source_size-first)return E_INVALIDARG;
        MEMORY_BASIC_INFORMATION region{};
        if(!VirtualQuery(address+first,&region,sizeof(region)) || region.State!=MEM_COMMIT || (region.Protect&(PAGE_NOACCESS|PAGE_GUARD))){
            meta->hole[i]=first;continue;}
        if(region.Type==MEM_IMAGE)return E_INVALIDARG;
        auto& xs=meta->xs;std::shared_ptr<const TilePlan> plan;
        UINT64 nr=UINT64(rows[i])*fp.Depth,cells=nr*columns;
        auto plan_start=report.tick();
        if(cells>=512 && cells<=16ull<<20 && source_size<=UINT_MAX){
            TileKey key{xs.Dimension,xs.Alignment,xs.Width,xs.Height,xs.DepthOrArraySize,xs.MipLevels,xs.Format,xs.SampleDesc.Count,
                xs.SampleDesc.Quality,xs.Layout,xs.Flags,source_size,i,columns,rows[i],fp.Depth,elem,first};
            // Only the layout calculation is cached. It takes no texture
            // mapping lease and can neither inspect nor retain resource data.
            plan=exact_tile_plan(key,source_size,UINT(widths[i]),UINT(nr),[&]()->std::shared_ptr<TilePlan>{
                auto p=std::make_shared<TilePlan>();p->ends.reserve(nr);
                for(UINT z=0;z<fp.Depth;++z)for(UINT y=0;y<rows[i];++y){
                    for(UINT64 x=0;x<columns;++x){auto t=offset_of(x,y,z);if(t>source_size || elem>source_size-t)return {};
                        if(x && p->runs.back().src+p->runs.back().n==t)p->runs.back().n+=UINT(elem);
                        else p->runs.push_back({UINT(t),UINT(elem)});
                        p->lo=std::min(p->lo,UINT(t));p->hi=std::max(p->hi,UINT(t+elem));}
                    p->ends.push_back(UINT(p->runs.size()));if(p->size()>32u<<20)return {};
                }return p;
            });
        }
        report.lookup+=report.tick()-plan_start;auto decode_start=report.tick();
        bool valid=true;MappingLease lease(address,source_size);SourcePages source(address,source_size);
        if(plan){
            ++report.planned;
            // A single checked system read of a dense mip is cheaper than
            // crossing into Wine again for every tiled page. On sparse pages
            // take the existing checked-window route and remember a real hole.
            SIZE_T bytes=plan->hi-plan->lo,got{};std::unique_ptr<BYTE[]> dense;
            if(bytes<=64u<<20 && bytes<=2*nr*widths[i]+131072){
                dense.reset(new(std::nothrow) BYTE[bytes]);
                if(dense && ReadProcessMemory(GetCurrentProcess(),address+plan->lo,dense.get(),bytes,&got) && got==bytes){lease.close();++report.dense;}
                else dense.reset();
            }
            UINT nrun=0;for(UINT row=0;valid && row<nr;++row){
                auto dst=mapped+layout[i].Offset+UINT64(row)*fp.RowPitch;
                for(;nrun<plan->ends[row];++nrun){auto [offset,length]=plan->runs[nrun];
                    if(dense)std::memcpy(dst,dense.get()+(offset-plan->lo),length);
                    else if(!source.copy(offset,dst,length,meta->hole[i])){valid=false;break;}
                    dst+=length;}
            }
        }else {++report.direct;for (UINT z=0;valid && z<fp.Depth;++z) for (UINT y=0;valid && y<rows[i];++y) {
            BYTE* dst=mapped+layout[i].Offset+(UINT64(z)*rows[i]+y)*fp.RowPitch;
            for(UINT64 x=0;x<columns;++x) {
                auto offset=offset_of(x,y,z);
                if (offset>source_size || elem>source_size-offset) return E_INVALIDARG;
                if(!source.copy(offset,dst+x*elem,SIZE_T(elem),meta->hole[i])){valid=false;break;}
            }
        }}
        report.decode+=report.tick()-decode_start;
        if (valid){meta->hole[i]=UINT64_MAX;ready.push_back(i);}
    }
    if(staging)staging->Unmap(0,nullptr);
    size+=gpu_bytes;
    if(GetEnvironmentVariableA("FROSTY_TEST_UPLOAD_PATH",nullptr,0))
        std::fprintf(stderr,"frosty/upload-fixture: gpu=%u bulk=%u planned=%u direct=%u\n",report.gpu,report.bulk,report.planned,report.direct);
    if (ready.empty()) return S_OK;
    std::unique_lock scheduler(lock_);
    if(FAILED(hr=reap_locked()))return hr;
    while(!flight_.empty() && in_flight_+size>(256ull<<20)){
        // Backpressure only under staging-memory pressure. Do not hold the
        // scheduler mutex while another application's queue needs to submit.
        auto target=flight_.front().value;auto f=fence_;scheduler.unlock();
        hr=f->SetEventOnCompletion(target,nullptr);scheduler.lock();
        if(FAILED(hr) || FAILED(hr=reap_locked()))return hr;
    }
    auto& access=meta->access;
    if(!access->readers.empty()){
        // A first-use residency refresh can overwrite a mip after another
        // queue copied/read it. Order against those precise consumers, never
        // an unrelated queue's last submission. Start a new transfer batch.
        if(FAILED(hr=flush_locked()) || FAILED(hr=begin_locked(dev)))return hr;
        for(auto& [_,reader]:access->readers){
            auto done=reader.first->GetCompletedValue();if(done==UINT64_MAX)return DXGI_ERROR_DEVICE_REMOVED;
            if(done<reader.second){
                pending_.waits.push_back(reader.first);
                if(FAILED(hr=queue_->Wait(reader.first.Get(),reader.second)))return hr;
            }
        }
        access->readers.clear();
    }
    if(FAILED(hr=begin_locked(dev)))return hr;
    auto& l=pending_.list;
    auto transition=[&](D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
        if(before==after)return;
        BarrierBatch batch(l.Get());for(auto i:ready){D3D12_RESOURCE_BARRIER b{};
            b.Transition={texture,i,before,after};batch.add(b);}batch.flush();
    };
    transition(state,D3D12_RESOURCE_STATE_COPY_DEST);
    for (UINT i:ready) {
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};
        src.pResource=staging.Get();
        if(auto& work=gpu[i];work.linear){
            detiler_.record(l.Get(),work);src.pResource=work.linear.Get();
            pending_.keep.push_back(work.linear);pending_.keep.push_back(work.tiled);pending_.keep.push_back(work.view->map);
        }
        src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=layout[i];
        dst.pResource=texture; dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex=i;
        l->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    }
    transition(D3D12_RESOURCE_STATE_COPY_DEST,state);
    if(staging)pending_.keep.push_back(staging);
    pending_.keep.emplace_back(texture);pending_.images.push_back(access);
    pending_.bytes+=size;access->epoch=serial_+1;
    for(UINT i:ready){meta->uploaded[i]=true;if(use)meta->confirmed[i]=true;}
    if(use) {
        bool complete=true;for(UINT i=0;i<n;++i)if(needed(i)){complete=false;break;}
        use->ready=complete;
    }
    const auto& final=meta->confirmed.empty()?meta->uploaded:meta->confirmed;
    access->immutable=std::find(final.begin(),final.end(),false)==final.end();
    if(pending_.images.size()>=16 || pending_.bytes>=32ull<<20)return flush_locked();
    return S_OK;
} catch(const std::bad_alloc&) {return E_OUTOFMEMORY;}
