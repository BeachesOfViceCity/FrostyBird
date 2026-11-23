#define NOMINMAX
#include "depth_alias.hpp"
#include "play_timing.hpp"
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <mutex>
#include <new>
#include <cstdio>

using Microsoft::WRL::ComPtr;
namespace {
bool direct_plane_supported(ID3D12Device* dev){
    if(GetEnvironmentVariableA("FROSTY_TEST_DEPTH_FOOTPRINT",nullptr,0))return false;
    // ID3D12DXVKInteropDevice::GetDeviceFeatures exposes the enabled
    // VkPhysicalDeviceFeatures2 chain of the actual backend device.
    constexpr GUID iid{0x39da4e09,0xbd1c,0x4198,{0x9f,0xae,0x86,0xbb,0xe3,0xbe,0x41,0xfd}};
    struct Header{UINT type;const Header* next;};
    struct Maintenance8{Header header;UINT enabled;};
    constexpr UINT maintenance8=1000574000; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_8_FEATURES_KHR
    ComPtr<IUnknown> ext;if(FAILED(dev->QueryInterface(iid,reinterpret_cast<void**>(ext.GetAddressOf()))))return false;
    auto table=*reinterpret_cast<void***>(ext.Get());
    auto get=std::bit_cast<HRESULT(WINAPI*)(IUnknown*,const Header**)>(table[6]);const Header* p{};
    if(FAILED(get(ext.Get(),&p)))return false;
    for(unsigned i=0;p && i<512;++i,p=p->next)
        if(p->type==maintenance8)return reinterpret_cast<const Maintenance8*>(p)->enabled!=0;
    return false;
}
constexpr GUID alias_key{0xea8cc451,0xe31e,0x4441,{0xab,0x93,0x72,0xae,0xcf,0xf9,0x53,0xba}};
struct Alias final : IUnknown {
    std::atomic<ULONG> refs{1};
    std::mutex init_lock;
    ComPtr<ID3D12Resource> image,transfer;
    UINT64 transfer_bytes{};bool native_depth{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT depth_fp{},color_fp{};
    HRESULT WINAPI QueryInterface(REFIID id,void** out) override {
        if(!out)return E_POINTER;
        *out=nullptr;if(id!=__uuidof(IUnknown))return E_NOINTERFACE;
        *out=this;AddRef();return S_OK;
    }
    ULONG WINAPI AddRef() override {return ++refs;}
    ULONG WINAPI Release() override {auto n=--refs;if(!n)delete this;return n;}
    HRESULT init(ID3D12Device* dev,ID3D12Resource* depth) {
        std::lock_guard guard(init_lock);
        if(image) return S_OK;
        auto d=depth->GetDesc();
        if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.MipLevels!=1 || d.DepthOrArraySize!=1 ||
           d.SampleDesc.Count!=1 || (d.Format!=DXGI_FORMAT_R32_TYPELESS && d.Format!=DXGI_FORMAT_R32G8X24_TYPELESS &&
           d.Format!=DXGI_FORMAT_D32_FLOAT && d.Format!=DXGI_FORMAT_D32_FLOAT_S8X24_UINT)) return E_NOTIMPL;
        UINT64 dsize{},csize{},dw{},cw{};
        UINT dr{},cr{};
        dev->GetCopyableFootprints(&d,0,1,0,&depth_fp,&dr,&dw,&dsize);
        d.Format=DXGI_FORMAT_R32_TYPELESS;d.Alignment=0;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
        dev->GetCopyableFootprints(&d,0,1,0,&color_fp,&cr,&cw,&csize);
        if(!csize || !dsize || dw!=cw || dr!=cr || !cr || depth_fp.Footprint.Depth!=1 || color_fp.Footprint.Depth!=1)
            return E_NOTIMPL;
        // Planar depth/stencil footprints can require a 512-byte pitch
        // even when the color plane requires only 256. Use the stricter
        // valid stride on both sides of the portable buffer copy.
        auto pitch=std::max(depth_fp.Footprint.RowPitch,color_fp.Footprint.RowPitch);
        depth_fp.Footprint.RowPitch=color_fp.Footprint.RowPitch=pitch;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;hp.CreationNodeMask=hp.VisibleNodeMask=1;
        native_depth=direct_plane_supported(dev);transfer_bytes=UINT64(pitch)*cr;
        return dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&image));
    }
    HRESULT staging(ID3D12Device* dev){
        std::lock_guard guard(init_lock);if(transfer)return S_OK;
        auto desc=D3D12_RESOURCE_DESC{D3D12_RESOURCE_DIMENSION_BUFFER,0,transfer_bytes,1,1,1,DXGI_FORMAT_UNKNOWN,{1,0},
                                      D3D12_TEXTURE_LAYOUT_ROW_MAJOR,D3D12_RESOURCE_FLAG_NONE};
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;hp.CreationNodeMask=hp.VisibleNodeMask=1;
        return dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&transfer));
    }
};
ComPtr<Alias> get(ID3D12Resource* r) {
    ComPtr<Alias> a;
    UINT n=sizeof(Alias*);
    if(r && (r->GetDesc().Flags&D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))r->GetPrivateData(alias_key,&n,a.GetAddressOf());
    return a;
}
void trans(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to,UINT sub) {
    if(from==to)return;
    D3D12_RESOURCE_BARRIER b{};b.Transition={r,sub,from,to};l->ResourceBarrier(1,&b);
}
}
D3D12_RESOURCE_DESC desktop_desc(const D3D12_RESOURCE_DESC& xs) {
    auto d=xs;
    // Desktop texture creation chooses the driver's optimal layout. The CPU
    // data from the XS layout is detiled by the package's address computer.
    d.Flags=static_cast<D3D12_RESOURCE_FLAGS>(UINT(xs.Flags)&0x1f);
    if(d.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER) {
        d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
        d.Alignment=0;
        if(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
            d.Flags=static_cast<D3D12_RESOURCE_FLAGS>(UINT(d.Flags)&~UINT(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
    }
    return d;
}
void tag_depth_alias(ID3D12Resource* r,const D3D12_RESOURCE_DESC& xs) {
    constexpr auto both=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL|D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if(!r || (xs.Flags&both)!=both)return;
    ComPtr<Alias> a;
    a.Attach(new(std::nothrow) Alias);
    if(!a || FAILED(r->SetPrivateDataInterface(alias_key,a.Get())))ExitProcess(ERROR_OUTOFMEMORY);
}
HRESULT depth_uav_view(ID3D12Device* dev,ID3D12Resource* depth,const D3D12_UNORDERED_ACCESS_VIEW_DESC* d,D3D12_CPU_DESCRIPTOR_HANDLE dst) {
    auto a=get(depth);if(!a)return S_FALSE;
    // X8X24 labels the ignored stencil/padding bits, not another color
    // component. The backing image contains only the depth plane (32 bits).
    auto format=d?d->Format:DXGI_FORMAT_UNKNOWN;
    auto base=depth->GetDesc().Format;
    if(format==DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS &&
       (base==DXGI_FORMAT_R32G8X24_TYPELESS || base==DXGI_FORMAT_D32_FLOAT_S8X24_UINT))
        format=DXGI_FORMAT_R32_FLOAT;
    if(!d || d->ViewDimension!=D3D12_UAV_DIMENSION_TEXTURE2D || d->Texture2D.PlaneSlice ||
       d->Texture2D.MipSlice || (format!=DXGI_FORMAT_R32_FLOAT && format!=DXGI_FORMAT_R32_UINT && format!=DXGI_FORMAT_R32_SINT)) {
        char msg[140];std::snprintf(msg,sizeof(msg),"frosty/depth: base=%u UAV=%u dimension=%u plane=%u\n",
            UINT(depth->GetDesc().Format),d?UINT(d->Format):0,d?UINT(d->ViewDimension):0,d?d->Texture2D.PlaneSlice:0);
        OutputDebugStringA(msg);
        return E_NOTIMPL;
    }
    auto hr=a->init(dev,depth);
    if(FAILED(hr)) {auto desc=depth->GetDesc();char msg[220];
        std::snprintf(msg,sizeof(msg),"frosty/depth: backing=%u mips=%u slices=%u samples=%u depthPitch=%u colorPitch=%u baseFormat=%u hr=%#lx\n",
            UINT(desc.Dimension),desc.MipLevels,desc.DepthOrArraySize,desc.SampleDesc.Count,a->depth_fp.Footprint.RowPitch,a->color_fp.Footprint.RowPitch,
            UINT(desc.Format),static_cast<unsigned long>(hr));OutputDebugStringA(msg);}
    if(SUCCEEDED(hr)) {
        auto view=*d;view.Format=format;
        dev->CreateUnorderedAccessView(a->image.Get(),nullptr,&view,dst);
    }
    return hr;
}
HRESULT depth_barrier(ID3D12Device* dev,BarrierBatch& batch,const D3D12_RESOURCE_BARRIER& b) {
    if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_UAV && b.UAV.pResource) {
        auto a=get(b.UAV.pResource);
        if(!a || !a->image)return S_FALSE;
        auto bd=b;bd.UAV.pResource=a->image.Get();batch.add(bd);return S_OK;
    }
    if(b.Type!=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)return S_FALSE;
    auto t=b.Transition;
    bool enter=t.StateAfter==D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if(!enter && t.StateBefore!=D3D12_RESOURCE_STATE_UNORDERED_ACCESS)return S_FALSE;
    auto a=get(t.pResource);if(!a)return S_FALSE;
    if(b.Flags!=D3D12_RESOURCE_BARRIER_FLAG_NONE || (t.Subresource && t.Subresource!=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES))
        return E_NOTIMPL;
    auto hr=a->init(dev,t.pResource);if(FAILED(hr))return hr;
    if(t.StateAfter==t.StateBefore)return S_OK;
    // Aliased depth transitions insert real copies. Do not move ordinary
    // barriers from before those copies to after them.
    batch.flush();auto* l=batch.list;
    bool direct=a->native_depth && l->GetType()==D3D12_COMMAND_LIST_TYPE_DIRECT;
    if(!direct && FAILED(hr=a->staging(dev)))return hr;
    static const bool timing=GetEnvironmentVariableA("FROSTY_FRAME_TIMES",nullptr,0)!=0;
    if(timing)(direct?play_timing::direct_planes:play_timing::staged_planes).fetch_add(1,std::memory_order_relaxed);
    static const bool trace_plane=GetEnvironmentVariableA("FROSTY_TEST_PLANE_PATH",nullptr,0)!=0;
    if(trace_plane)std::fprintf(stderr,"frosty/depth-fixture: %s plane zero\n",direct?"direct":"footprint");
    // Only subresource zero is copied, including on depth/stencil images.
    auto src=enter?t.pResource:a->image.Get(),dst=enter?a->image.Get():t.pResource;
    trans(l,src,t.StateBefore,D3D12_RESOURCE_STATE_COPY_SOURCE,enter?t.Subresource:0);
    trans(l,dst,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST,enter?0:t.Subresource);
    D3D12_TEXTURE_COPY_LOCATION image{},buf{};
    image.pResource=src;image.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    if(direct){
        auto to=image;to.pResource=dst;l->CopyTextureRegion(&to,0,0,0,&image,nullptr);
    }else{
        buf.pResource=a->transfer.Get();buf.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        buf.PlacedFootprint=enter?a->depth_fp:a->color_fp;
        l->CopyTextureRegion(&buf,0,0,0,&image,nullptr);
        trans(l,a->transfer.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE,0);
        image.pResource=dst;buf.PlacedFootprint=enter?a->color_fp:a->depth_fp;
        l->CopyTextureRegion(&image,0,0,0,&buf,nullptr);
        trans(l,a->transfer.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST,0);
    }
    trans(l,dst,D3D12_RESOURCE_STATE_COPY_DEST,t.StateAfter,enter?0:t.Subresource);
    trans(l,src,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON,enter?t.Subresource:0);
    return S_OK;
}
