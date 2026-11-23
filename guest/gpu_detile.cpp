#define NOMINMAX
#include "gpu_detile.hpp"
#include <d3dcompiler.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <set>
#include <thread>
#include <atomic>
#include "depth_alias.hpp"
#include "texture_mapping.hpp"

namespace {
using Microsoft::WRL::ComPtr;
using File=std::unique_ptr<FILE,decltype(&std::fclose)>;
constexpr UINT magic=0x314f4758; // XGO1
struct Header{TileKey key;UINT tag,blocks,patterns,pad;std::array<BYTE,32> sha;};
struct Data{std::vector<UINT> words;UINT low{},high{};};
bool bounds(const TileKey& k){
    return k[11] && k[11]<=2ull<<30 && k[13] && k[13]<=65536 && k[14] && k[14]<=65536 &&
           k[15] && k[15]<=2048 && k[13]*k[14]*k[15]<=32ull<<20 && k[16] && k[16]<=16 && std::has_single_bit(k[16]);
}
UINT blocks(const TileKey& k){return UINT((k[13]*k[14]*k[15]+15)/16);}
bool verify(Data& d,UINT count,UINT patterns,const TileKey& k){
    if(!bounds(k) || count!=blocks(k) || !patterns || patterns>65536 || d.words.size()!=2ull*count+16ull*patterns)return false;
    std::vector<std::pair<int,int>> ranges;auto tail=d.words.data()+2*count;
    for(UINT i=0;i<patterns;++i){
        int lo=0,hi=0;
        for(UINT j=0;j<16;++j){int v=std::bit_cast<int>(tail[16*i+j]);if(v&int(std::min<UINT64>(4,k[16])-1))return false;lo=std::min(lo,v);hi=std::max(hi,v);}
        ranges.emplace_back(lo,hi);
    }
    d.low=UINT_MAX;d.high=0;
    for(UINT b=0;b<count;++b){auto base=d.words[2*b],p=d.words[2*b+1];if((base&(std::min<UINT64>(4,k[16])-1)) || p<8*count || (p-8*count)%64)return false;
        auto id=(p-8*count)/64;if(id>=patterns)return false;
        int64_t lo=int64_t(base)+ranges[id].first,hi=int64_t(base)+ranges[id].second+k[16];
        if(lo<0 || hi>int64_t(k[11]))return false;
        d.low=std::min(d.low,UINT(lo));d.high=std::max(d.high,UINT(hi));}
    // The byte address buffer is read in aligned DWORDs.
    d.low&=~15u;d.high=(d.high+3u)&~3u;return d.high>d.low && d.high<=k[11];
}
bool read_map(const std::wstring& path,const TileKey& k,Data& data){
    if(path.empty())return false;
    File f(_wfopen(path.c_str(),L"rb"),std::fclose);if(!f)return false;
    Header h{};if(std::fread(&h,1,sizeof(h),f.get())!=sizeof(h)||h.key!=k||h.tag!=magic||h.pad||
        h.blocks!=blocks(k)||!h.patterns||h.patterns>65536)return false;
    data.words.resize(2ull*h.blocks+16ull*h.patterns);auto bytes=data.words.size()*4;std::array<BYTE,32> sha{};
    return std::fread(data.words.data(),1,bytes,f.get())==bytes && std::fgetc(f.get())==EOF &&
        tile_sha(data.words.data(),bytes,sha) && sha==h.sha && verify(data,h.blocks,h.patterns,k);
}
bool build_map(const TileKey& k,XgComputer* xg,Data& data,UINT& unique){
    if(!bounds(k))return false;
    auto cols=UINT(k[13]),rows=UINT(k[14]);auto n=k[13]*k[14]*k[15];
    auto nb=blocks(k),mip=UINT(k[12]%k[5]),layer=UINT(k[12]/k[5]);data.words.resize(2*nb);
    using Pattern=std::array<int,16>;std::map<Pattern,UINT> dictionary;std::vector<Pattern> patterns;
    for(UINT b=0;b<nb;++b){UINT base=0;Pattern pat{};
        for(UINT j=0;j<16 && 16ull*b+j<n;++j){auto i=16*b+j,row=i/cols;
            auto t=xg->v->offset(xg,0,mip,i%cols,row%rows,k[0]==D3D12_RESOURCE_DIMENSION_TEXTURE3D?row/rows:layer,0,nullptr);
            if((t&(std::min<UINT64>(4,k[16])-1)) || t>k[11] || k[16]>k[11]-t)return false;
            if(!j)base=UINT(t);
            pat[j]=int(int64_t(t)-int64_t(base));
        }
        auto [it,fresh]=dictionary.try_emplace(pat,UINT(patterns.size()));
        if(fresh){if(patterns.size()>=65536)return false;patterns.push_back(pat);}
        data.words[2*b]=base;data.words[2*b+1]=8*nb+64*it->second;
    }
    unique=UINT(patterns.size());for(auto& p:patterns)for(auto v:p)data.words.push_back(std::bit_cast<UINT>(v));
    return verify(data,nb,unique,k);
}
void save_map(const std::wstring& path,const TileKey& k,const Data& data,UINT np){
    if(path.empty())return;
    Header h{k,magic,blocks(k),np,0,{}};auto bytes=data.words.size()*4;
    if(!tile_sha(data.words.data(),bytes,h.sha))return;
    auto tmp=path+L"."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetCurrentThreadId())+L".tmp";
    File f(_wfopen(tmp.c_str(),L"wb"),std::fclose);if(!f)return;
    bool ok=std::fwrite(&h,1,sizeof(h),f.get())==sizeof(h) && std::fwrite(data.words.data(),1,bytes,f.get())==bytes && !std::fflush(f.get());
    f.reset();if(!ok || !MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING))DeleteFileW(tmp.c_str());
}
bool get(const TileKey& k,XgComputer* xg,bool build,Data& data){
    if(!bounds(k))return false;
    auto path=tile_path(k,L".xgo");
    if(read_map(path,k,data))return true;
    if(!build){
        // Learn an unseen descriptor before any bytes are required to be
        // resident. The offline layout compiler can prepare its larger mips.
        auto desc=k;std::fill(desc.begin()+12,desc.end(),0);
        auto recipe=tile_path(desc,L".xdesc");
        if(!recipe.empty()){
            auto h=CreateFileW(recipe.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(h!=INVALID_HANDLE_VALUE){DWORD n{};bool ok=WriteFile(h,desc.data(),sizeof(desc),&n,nullptr)&&n==sizeof(desc);
                CloseHandle(h);if(!ok)DeleteFileW(recipe.c_str());}
        }
        return false;
    }
    UINT np{};if(!build_map(k,xg,data,np))return false;save_map(path,k,data,np);return true;
}
HRESULT buffer(ID3D12Device* d,UINT64 n,bool upload,ID3D12Resource** out){
    D3D12_HEAP_PROPERTIES hp{};hp.Type=upload?D3D12_HEAP_TYPE_UPLOAD:D3D12_HEAP_TYPE_DEFAULT;hp.CreationNodeMask=hp.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC b{D3D12_RESOURCE_DIMENSION_BUFFER,0,n,1,1,1,DXGI_FORMAT_UNKNOWN,{1,0},D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        upload?D3D12_RESOURCE_FLAG_NONE:D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    return d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&b,upload?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                      nullptr,__uuidof(ID3D12Resource),reinterpret_cast<void**>(out));
}
}
bool prepare_gpu_layout(const TileKey& k,XgComputer* xg){Data d;return get(k,xg,true,d);}
static D3D12_RESOURCE_DESC describe(const TileKey& k){
    return {D3D12_RESOURCE_DIMENSION(k[0]),k[1],k[2],UINT(k[3]),UINT16(k[4]),UINT16(k[5]),DXGI_FORMAT(k[6]),
            {UINT(k[7]),UINT(k[8])},D3D12_TEXTURE_LAYOUT(k[9]),D3D12_RESOURCE_FLAGS(k[10])};
}
void GpuDetiler::learn(const TileKey& k){
    if(!bounds(k) || tile_path(k,L".xgo").empty())return;
    std::lock_guard g(lock_);if(closing_ || work_.size()>=16 || !queued_.insert(k).second)return;
    try{work_.push_back(k);
        if(!worker_.joinable())worker_=std::thread([this]{
            SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_LOWEST);
            for(;;){
                TileKey key;{std::unique_lock g(lock_);wake_.wait(g,[&]{return closing_ || !work_.empty();});
                    if(closing_)return;
                    key=work_.front();work_.pop_front();}
                // A worker owns its own address computer and reads no live
                // texture bytes. The first consumer can use the CPU fallback.
                try{ComPtr<XgComputer> layout;auto xs=describe(key);
                    if(SUCCEEDED(xg_computer(xs,layout.GetAddressOf())) && layout->v->size(layout.Get())==key[11])
                        prepare_gpu_layout(key,layout.Get());
                }catch(const std::bad_alloc&){}
                {std::lock_guard g(lock_);queued_.erase(key);}
            }
        });
    }catch(const std::system_error&){work_.pop_back();queued_.erase(k);return;}
    wake_.notify_one();
}
GpuDetiler::~GpuDetiler(){
    {std::lock_guard g(lock_);closing_=true;work_.clear();}
    wake_.notify_all();if(worker_.joinable())worker_.join();
}
int prepare_known_tile_layouts(ID3D12Device* dev){
    namespace fs=std::filesystem;wchar_t folder[2048]{};auto n=GetEnvironmentVariableW(L"FROSTY_TILE_PLANS",folder,2048);
    if(!n || n>=2048)return 2;
    std::set<TileKey> unique;std::error_code ec;
    for(fs::directory_iterator it(folder,ec),end;!ec && it!=end;it.increment(ec)){
        auto p=it->path();auto ext=p.extension().wstring();if(ext!=L".xdesc" && ext!=L".xtp" && ext!=L".xgo")continue;
        File f(_wfopen(p.c_str(),L"rb"),std::fclose);TileKey k{};
        if(!f || std::fread(k.data(),1,sizeof(k),f.get())!=sizeof(k) || fs::path(tile_path(k,ext.c_str())).filename()!=p.filename())continue;
        std::fill(k.begin()+12,k.end(),0);unique.insert(k);
    }
    if(ec)return 1;
    std::vector<TileKey> list(unique.begin(),unique.end());std::atomic<size_t> next{};std::atomic<UINT> ready{},skipped{};
    auto prepare=[&]{SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
        for(size_t j;(j=next++)<list.size();){auto& k=list[j];
            if(k[0]<D3D12_RESOURCE_DIMENSION_TEXTURE1D || k[0]>D3D12_RESOURCE_DIMENSION_TEXTURE3D || !k[2] || k[2]>16384 ||
               !k[3] || k[3]>16384 || !k[4] || k[4]>2048 || !k[5] || k[5]>15 || k[7]!=1 || !k[11] || k[11]>2ull<<30)continue;
            auto xs=describe(k),desc=desktop_desc(xs);
            D3D12_FEATURE_DATA_FORMAT_INFO info{xs.Format,0};if(FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO,&info,sizeof(info))) || info.PlaneCount!=1)continue;
            bool bc=(xs.Format>=DXGI_FORMAT_BC1_TYPELESS && xs.Format<=DXGI_FORMAT_BC5_SNORM) ||
                    (xs.Format>=DXGI_FORMAT_BC6H_TYPELESS && xs.Format<=DXGI_FORMAT_BC7_UNORM_SRGB);
            UINT layers=xs.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE3D?1:xs.DepthOrArraySize;
            ComPtr<XgComputer> xg;if(FAILED(xg_computer(xs,xg.GetAddressOf())) || xg->v->size(xg.Get())!=k[11]){++skipped;continue;}
            for(UINT i=0;i<layers*xs.MipLevels;++i){D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT nr{};UINT64 rw{},size{};
                dev->GetCopyableFootprints(&desc,i,1,0,&fp,&nr,&rw,&size);auto mip=i%xs.MipLevels;
                auto w=std::max<UINT64>(1,xs.Width>>mip),col=bc?(w+3)/4:w;
                if(!nr || !fp.Footprint.Depth || size<512u<<10 || size>128ull<<20 || !rw || rw%col)continue;
                auto key=tile_key(xs,k[11],i,col,nr,fp.Footprint.Depth,UINT(rw/col),
                    xg->v->offset(xg.Get(),0,mip,0,0,layers==1?0:i/xs.MipLevels,0,nullptr));
                if(!bounds(key))continue;
                if(prepare_gpu_layout(key,xg.Get()))++ready;else ++skipped;
            }
        }
    };
    {std::vector<std::jthread> workers;for(unsigned i=0;i<3;++i)workers.emplace_back(prepare);prepare();}
    std::printf("GPU tile preparation: %zu exact source descriptors, %u ready subresource maps (%u unavailable)\n",
        list.size(),ready.load(),skipped.load());return 0;
}
bool GpuDetiler::init(ID3D12Device* dev){
    if(kernel_)return true;
    const char shader[]=R"(
ByteAddressBuffer source:register(t0);ByteAddressBuffer mapping:register(t1);RWByteAddressBuffer outputBytes:register(u0);
cbuffer C:register(b0){uint columns;uint rows;uint pitch;uint shift;uint low;}
uint addressOf(uint cell){uint2 block=mapping.Load2((cell>>4u)*8u);return block.x+mapping.Load(block.y+4u*(cell&15u))-low;}
[numthreads(128,1,1)] void main(uint3 t:SV_DispatchThreadID){
    uint row=t.y+t.z*65535u,b=t.x*4u,width=columns<<shift;if(row>=rows || b>=width)return;
    uint mask=(1u<<shift)-1u,value=0u;
    if(shift>=2u)value=source.Load(addressOf(row*columns+(b>>shift))+(b&mask));
    else [unroll]for(uint j=0u;j<4u;++j)if(b+j<width){uint at=addressOf(row*columns+((b+j)>>shift))+((b+j)&mask);
        value|=((source.Load(at&~3u)>>((at&3u)*8u))&255u)<<(j*8u);}
    outputBytes.Store(row*pitch+b,value);
})";
    ComPtr<ID3DBlob> cs,blob,errors;
    if(FAILED(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&cs,&errors)))return false;
    D3D12_ROOT_PARAMETER p[4]{};p[0].ParameterType=p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;p[1].Descriptor.ShaderRegister=1;
    p[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;p[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[3].Constants={0,0,5};
    D3D12_ROOT_SIGNATURE_DESC desc{4,p,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3D12RootSignature> rs;
    if(FAILED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors)) ||
       FAILED(dev->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&rs))))return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};ps.pRootSignature=rs.Get();ps.CS={cs->GetBufferPointer(),cs->GetBufferSize()};
    if(FAILED(dev->CreateComputePipelineState(&ps,IID_PPV_ARGS(&kernel_))))return false;
    root_=std::move(rs);return true;
}
std::shared_ptr<const GpuTileView> GpuDetiler::view(ID3D12Device* d,const TileKey& k,XgComputer* xg,bool build)try{
    {std::lock_guard g(lock_);if(auto i=maps_.find(k);i!=maps_.end())return i->second;}
    Data data;if(!get(k,xg,build,data)){if(!build)learn(k);return {};}
    std::lock_guard g(lock_);if(auto i=maps_.find(k);i!=maps_.end())return i->second;
    if(!init(d))return {};
    auto m=std::make_shared<GpuTileView>();m->bytes=4*data.words.size();m->low=data.low;m->high=data.high;
    if(FAILED(buffer(d,m->bytes,true,m->map.GetAddressOf())))return {};
    D3D12_RANGE no_read{};void* dst{};if(FAILED(m->map->Map(0,&no_read,&dst)))return {};
    std::memcpy(dst,data.words.data(),m->bytes);m->map->Unmap(0,nullptr);
    while(!fifo_.empty() && (bytes_+m->bytes>192u<<20 || fifo_.size()>=2048)){
        auto at=maps_.find(fifo_.front());bytes_-=at->second->bytes;maps_.erase(at);fifo_.pop_front();}
    fifo_.push_back(k);try{maps_.emplace(k,m);}catch(...){fifo_.pop_back();throw;}bytes_+=m->bytes;return m;
}catch(const std::bad_alloc&){return {};}
bool GpuDetiler::stage(ID3D12Device* d,std::shared_ptr<const GpuTileView> m,const BYTE* address,UINT cols,UINT rows,UINT pitch,UINT elem,GpuTileWork& out){
    UINT64 src=UINT64(m->high)-m->low,dst=UINT64(pitch)*rows;
    if(!src || !dst || src>128ull<<20 || dst>128ull<<20 || src>2*UINT64(cols)*rows*elem+131072)return false;
    GpuTileWork w;if(FAILED(buffer(d,src,true,w.tiled.GetAddressOf())) || FAILED(buffer(d,dst,false,w.linear.GetAddressOf())))return false;
    D3D12_RANGE no_reads{};void* data{};if(FAILED(w.tiled->Map(0,&no_reads,&data)))return false;
    // Never enter D3D12 allocation, Map, Unmap or queue waits while holding
    // the title page mapper's shared lock.
    SIZE_T got{};bool ok;
    {MappingLease source(address+m->low,SIZE_T(src));ok=ReadProcessMemory(GetCurrentProcess(),address+m->low,data,SIZE_T(src),&got) && got==src;}
    w.tiled->Unmap(0,nullptr);if(!ok)return false;
    w.constants={cols,rows,pitch,UINT(std::countr_zero(elem)),m->low};w.bytes=src+dst;w.view=std::move(m);out=std::move(w);return true;
}
void GpuDetiler::record(ID3D12GraphicsCommandList* l,const GpuTileWork& w){
    l->SetPipelineState(kernel_.Get());l->SetComputeRootSignature(root_.Get());
    l->SetComputeRootShaderResourceView(0,w.tiled->GetGPUVirtualAddress());l->SetComputeRootShaderResourceView(1,w.view->map->GetGPUVirtualAddress());
    l->SetComputeRootUnorderedAccessView(2,w.linear->GetGPUVirtualAddress());l->SetComputeRoot32BitConstants(3,5,w.constants.data(),0);
    auto rows=w.constants[1];l->Dispatch(((w.constants[0]<<w.constants[3])+511)/512,std::min(65535u,rows),(rows+65534)/65535);
    D3D12_RESOURCE_BARRIER b{};b.Transition={w.linear.Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};l->ResourceBarrier(1,&b);
}
