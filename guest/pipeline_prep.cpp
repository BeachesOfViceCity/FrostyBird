#define NOMINMAX
#include "pipeline_prep.hpp"
#include "recompiled_shader.hpp"
#include "play_timing.hpp"
#include <bcrypt.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
using Data=std::vector<BYTE>;
using Bytes=std::span<const BYTE>;
namespace fs=std::filesystem;
constexpr GUID root_bytes{0xb46efc15,0x388d,0x47cf,{0x90,0xee,0x72,0x03,0xdc,0x89,0xbd,0x38}};
constexpr UINT magic=0x31505358; // XSP1
constexpr size_t max_plan=64ull<<20;
const std::wstring& directory(){
    static const auto d=[](){wchar_t p[2048]{};auto n=GetEnvironmentVariableW(L"FROSTY_PIPELINE_RECIPES",p,2048);
        return n && n<2048?std::wstring(p,n):std::wstring{};}();return d;
}
std::wstring id(Bytes bytes){
    static const auto alg=[](){BCRYPT_ALG_HANDLE a{};return BCryptOpenAlgorithmProvider(&a,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0?a:nullptr;}();
    std::array<BYTE,32> digest{};
    if(!alg || bytes.size()>UINT32_MAX || BCryptHash(alg,nullptr,0,const_cast<BYTE*>(bytes.data()),ULONG(bytes.size()),digest.data(),digest.size())<0)
        throw std::runtime_error("hash unavailable");
    std::wstring s;s.reserve(64);constexpr wchar_t hex[]=L"0123456789abcdef";
    for(auto v:digest){s.push_back(hex[v>>4]);s.push_back(hex[v&15]);}return s;
}
void publish(const std::wstring& path,Bytes bytes){
    if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES)return;
    static std::atomic<UINT> seq{};
    auto tmp=path+L"."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(++seq)+L".tmp";
    auto h=CreateFileW(tmp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE)return;
    DWORD wrote{};bool ok=WriteFile(h,bytes.data(),DWORD(bytes.size()),&wrote,nullptr) && wrote==bytes.size();
    CloseHandle(h);if(!ok || !MoveFileExW(tmp.c_str(),path.c_str(),0))DeleteFileW(tmp.c_str());
}
Data read(const fs::path& path,size_t limit){
    auto f=_wfopen(path.c_str(),L"rb");if(!f)return {};
    std::unique_ptr<FILE,decltype(&std::fclose)> keep(f,std::fclose);
    if(std::fseek(f,0,SEEK_END))return {};
    auto n=std::ftell(f);if(n<=0 || size_t(n)>limit || std::fseek(f,0,SEEK_SET))return {};
    Data b(n);if(std::fread(b.data(),1,b.size(),f)!=b.size())return {};return b;
}
struct Writer{
    Data b;
    template<class T>void field(T v){static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);auto p=reinterpret_cast<const BYTE*>(&v);b.insert(b.end(),p,p+sizeof(T));}
    void blob(const void* p,size_t n){
        if(n>(16u<<20) || (n && !p))throw std::runtime_error("nonportable blob");
        field(UINT(n));if(n){auto s=static_cast<const BYTE*>(p);b.insert(b.end(),s,s+n);}
    }
};
struct Reader{
    Bytes b;
    template<class T>void field(T& x){static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
        if(b.size()<sizeof(x))throw std::runtime_error("truncated state");
        std::memcpy(&x,b.data(),sizeof(x));b=b.subspan(sizeof(x));}
    Bytes blob(){UINT n;field(n);if(n>b.size() || n>(16u<<20))throw std::runtime_error("truncated blob");
        auto r=b.first(n);b=b.subspan(n);return r;}
    void stage(D3D12_SHADER_BYTECODE& s){auto data=blob();s={data.empty()?nullptr:data.data(),data.size()};}
};
template<class C,class D>void graphics_state(C& c,D& d){
    c.field(d.NodeMask);c.field(d.Flags);c.field(d.SampleMask);c.field(d.PrimitiveTopologyType);c.field(d.IBStripCutValue);
    c.field(d.SampleDesc.Count);c.field(d.SampleDesc.Quality);c.field(d.NumRenderTargets);
    if(d.NumRenderTargets>8)throw std::runtime_error("invalid render targets");
    for(UINT i=0;i<d.NumRenderTargets;++i)c.field(d.RTVFormats[i]);
    c.field(d.DSVFormat);
    auto& b=d.BlendState;c.field(b.AlphaToCoverageEnable);c.field(b.IndependentBlendEnable);
    for(auto& t:b.RenderTarget){c.field(t.BlendEnable);c.field(t.LogicOpEnable);c.field(t.SrcBlend);c.field(t.DestBlend);
        c.field(t.BlendOp);c.field(t.SrcBlendAlpha);c.field(t.DestBlendAlpha);c.field(t.BlendOpAlpha);c.field(t.LogicOp);c.field(t.RenderTargetWriteMask);}
    auto& r=d.RasterizerState;c.field(r.FillMode);c.field(r.CullMode);c.field(r.FrontCounterClockwise);c.field(r.DepthBias);
    c.field(r.DepthBiasClamp);c.field(r.SlopeScaledDepthBias);c.field(r.DepthClipEnable);c.field(r.MultisampleEnable);
    c.field(r.AntialiasedLineEnable);c.field(r.ForcedSampleCount);c.field(r.ConservativeRaster);
    auto& s=d.DepthStencilState;c.field(s.DepthEnable);c.field(s.DepthWriteMask);c.field(s.DepthFunc);c.field(s.StencilEnable);
    c.field(s.StencilReadMask);c.field(s.StencilWriteMask);
    auto face=[&](auto& a){c.field(a.StencilFailOp);c.field(a.StencilDepthFailOp);c.field(a.StencilPassOp);c.field(a.StencilFunc);};
    face(s.FrontFace);face(s.BackFace);
}
template<class C,class T>void element(C& c,T& e){c.field(e.SemanticIndex);c.field(e.Format);c.field(e.InputSlot);
    c.field(e.AlignedByteOffset);c.field(e.InputSlotClass);c.field(e.InstanceDataStepRate);}
bool header(Writer& w,UINT kind,ID3D12RootSignature* rs){
    UINT n{};if(!rs || FAILED(rs->GetPrivateData(root_bytes,&n,nullptr)) || n<=4 || n>(16u<<20))return false;
    Data root(n);if(FAILED(rs->GetPrivateData(root_bytes,&n,root.data())))return false;
    w.field(magic);w.field(kind);w.blob(root.data(),n);return true;
}
Data pack(const D3D12_COMPUTE_PIPELINE_STATE_DESC& d){
    Writer w;if(!header(w,1,d.pRootSignature))return {};
    w.field(d.NodeMask);w.field(d.Flags);w.blob(d.CS.pShaderBytecode,d.CS.BytecodeLength);return std::move(w.b);
}
Data pack(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d,BOOL bounds){
    Writer w;if(!header(w,2,d.pRootSignature) || d.HS.BytecodeLength || d.DS.BytecodeLength || d.StreamOutput.NumEntries ||
        d.StreamOutput.NumStrides || d.InputLayout.NumElements>32)return {};
    w.field(bounds);graphics_state(w,d);
    w.field(d.InputLayout.NumElements);
    for(UINT i=0;i<d.InputLayout.NumElements;++i){auto& e=d.InputLayout.pInputElementDescs[i];
        if(!e.SemanticName)throw std::runtime_error("invalid semantic");
        w.blob(e.SemanticName,std::strlen(e.SemanticName)+1);element(w,e);}
    for(auto s:{d.VS,d.PS,d.GS})w.blob(s.pShaderBytecode,s.BytecodeLength);
    return std::move(w.b);
}
struct RayCompile {ID3D12Device5* dev;const D3D12_STATE_OBJECT_DESC* desc;ID3D12StateObject** out;HRESULT hr=E_UNEXPECTED;};
DWORD WINAPI make_ray_on_stack(void* args){
    auto& j=*static_cast<RayCompile*>(args);j.hr=j.dev->CreateStateObject(j.desc,IID_PPV_ARGS(j.out));return 0;
}
HRESULT build_ray(ID3D12Device5* d,ID3D12RootSignature* global,D3D12_RAYTRACING_SHADER_CONFIG config,
        D3D12_RAYTRACING_PIPELINE_CONFIG1 depth,const std::vector<RayPipelineLibrary>& libs,UINT rg,Bytes local,ID3D12StateObject** out){
    if(rg>=libs.size() || libs[rg].alias.empty() || !libs[rg].group.empty())return E_INVALIDARG;
    struct Part{D3D12_DXIL_LIBRARY_DESC lib;D3D12_EXPORT_DESC exp;D3D12_HIT_GROUP_DESC hit;};
    std::vector<Part> parts(libs.size());std::vector<D3D12_STATE_SUBOBJECT> subs;subs.reserve(2*libs.size()+5);
    for(size_t i=0;i<libs.size();++i){
        auto& s=libs[i];auto& p=parts[i];
        p.exp={s.alias.c_str(),s.entry.c_str(),D3D12_EXPORT_FLAG_NONE};p.lib={{s.code.data(),s.code.size()},1,&p.exp};
        subs.push_back({D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,&p.lib});
        if(!s.group.empty()){p.hit.HitGroupExport=s.group.c_str();p.hit.ClosestHitShaderImport=s.alias.c_str();
            subs.push_back({D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,&p.hit});}
    }
    ComPtr<ID3D12RootSignature> rs;
    auto hr=d->CreateRootSignature(0,local.data(),local.size(),IID_PPV_ARGS(&rs));if(FAILED(hr))return hr;
    D3D12_GLOBAL_ROOT_SIGNATURE gr{global};D3D12_LOCAL_ROOT_SIGNATURE lr{rs.Get()};
    subs.push_back({D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG,&config});
    subs.push_back({D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1,&depth});
    subs.push_back({D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,&gr});
    subs.push_back({D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE,&lr});
    LPCWSTR name=libs[rg].alias.c_str();D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION a{&subs.back(),1,&name};
    subs.push_back({D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,&a});
    D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,UINT(subs.size()),subs.data()};
    bool trace=GetEnvironmentVariableA("FROSTY_TRACE_PSO",nullptr,0)!=0;
    if(trace)std::fprintf(stderr,"frosty/pso-begin: %04lx RAY %ls (%zu exports)\n",GetCurrentThreadId(),name,libs.size());
    if(play_timing::offline_bake)hr=d->CreateStateObject(&desc,IID_PPV_ARGS(out));
    else{
        RayCompile job{d,&desc,out};auto t=CreateThread(nullptr,32u<<20,make_ray_on_stack,&job,STACK_SIZE_PARAM_IS_A_RESERVATION,nullptr);
        if(t){auto wait=WaitForSingleObject(t,INFINITE);CloseHandle(t);hr=wait==WAIT_OBJECT_0?job.hr:E_FAIL;}
        else hr=HRESULT_FROM_WIN32(GetLastError());
    }
    if(trace)std::fprintf(stderr,"frosty/pso-end: %04lx RAY %#lx\n",GetCurrentThreadId(),(unsigned long)hr);
    return hr;
}
template<class P,class C,class D>void ray_config(P& p,C& config,D& depth){
    p.field(config.MaxPayloadSizeInBytes);p.field(config.MaxAttributeSizeInBytes);p.field(depth.MaxTraceRecursionDepth);p.field(depth.Flags);
}
Data pack_ray(ID3D12RootSignature* root,D3D12_RAYTRACING_SHADER_CONFIG config,D3D12_RAYTRACING_PIPELINE_CONFIG1 depth,
        const std::vector<RayPipelineLibrary>& libs,UINT rg,Bytes local){
    Writer w;if(!header(w,3,root) || libs.size()>256)return {};
    ray_config(w,config,depth);w.blob(local.data(),local.size());w.field(rg);w.field(UINT(libs.size()));
    for(const auto& s:libs){
        for(const auto* name:{&s.entry,&s.alias,&s.group})w.blob(name->data(),name->size()*sizeof(wchar_t));
        w.blob(s.code.data(),s.code.size());if(w.b.size()>max_plan)return {};
    }
    return std::move(w.b);
}
HRESULT replay_ray(ID3D12Device* dev,ID3D12RootSignature* root,Reader& in,ID3D12StateObject** out){
    ComPtr<ID3D12Device5> d;auto hr=dev->QueryInterface(IID_PPV_ARGS(&d));if(FAILED(hr))return hr;
    D3D12_RAYTRACING_SHADER_CONFIG cfg{};D3D12_RAYTRACING_PIPELINE_CONFIG1 depth{};ray_config(in,cfg,depth);
    auto local=in.blob();UINT rg,n;in.field(rg);in.field(n);
    if(!n || n>256 || rg>=n || local.empty())return E_INVALIDARG;
    std::vector<RayPipelineLibrary> libs(n);
    for(auto& s:libs){
        for(auto* name:{&s.entry,&s.alias,&s.group}){
            auto b=in.blob();if(b.size()%sizeof(wchar_t) || b.size()>4096)return E_INVALIDARG;
            name->resize(b.size()/sizeof(wchar_t));if(!b.empty())std::memcpy(name->data(),b.data(),b.size());
            if(name->find(L'\0')!=name->npos)return E_INVALIDARG;
        }
        auto b=in.blob();if(s.entry.empty() || s.alias.empty() || b.size()<32 || std::memcmp(b.data(),"DXBC",4))return E_INVALIDARG;
        s.code.assign(b.begin(),b.end());
    }
    if(!in.b.empty())return E_INVALIDARG;
    return build_ray(d.Get(),root,cfg,depth,libs,rg,local,out);
}
}
void remember_pipeline_root(ID3D12RootSignature* rs,UINT node,const void* data,SIZE_T n){
    if(directory().empty() || !data || !n || n>(16u<<20)-4)return;
    try{Writer w;w.field(node);auto p=static_cast<const BYTE*>(data);w.b.insert(w.b.end(),p,p+n);rs->SetPrivateData(root_bytes,UINT(w.b.size()),w.b.data());}
    catch(const std::bad_alloc&){/* optional learning */}
}
void pipeline_view_hint(const std::wstring& path,std::span<const BYTE> types){
    if(path.empty() || types.empty() || types.size()>512)return;
    try{publish(path+L"."+id(types)+L".hint",types);}catch(const std::exception&){}
}
struct PipelinePrep::State{
    std::wstring dir;
    struct Prepared{ComPtr<ID3D12PipelineState> pipeline;bool predicted;};
    std::mutex lock;std::map<std::wstring,Prepared> ready;
    std::map<std::wstring,ComPtr<ID3D12StateObject>> ray_ready;
    std::condition_variable published;
    std::set<std::wstring> preparing,requested;
    std::map<std::wstring,ComPtr<ID3D12PipelineState>> recent;
    std::deque<std::wstring> order;
    ComPtr<ID3D12Device> device;std::atomic<bool> started{},stop{};std::thread worker;
    std::atomic<UINT> hits{};
    explicit State(std::wstring d):dir(std::move(d)){}
    ~State(){stop=true;if(worker.joinable())worker.join();}
    std::wstring stem(const std::wstring& key)const{return dir+L"\\"+key;}
    void retain(const std::wstring& key,ID3D12PipelineState* p){
        if(!recent.emplace(key,p).second)return;
        order.push_back(key);
        if(order.size()>192){recent.erase(order.front());order.pop_front();}
    }
    template<class Pack,class Build>HRESULT get(Pack pack,Build build,ID3D12PipelineState** out){
        Data blob;std::wstring key;bool first=false;
        try{blob=pack();if(!blob.empty()){key=id(blob);std::unique_lock g(lock);first=requested.insert(key).second;
            if(preparing.contains(key))published.wait(g,[&]{return !preparing.contains(key) || ready.contains(key) || recent.contains(key);});
            if(auto it=recent.find(key);it!=recent.end())return it->second.CopyTo(out);
            if(auto it=ready.find(key);it!=ready.end()){
                bool promote=it->second.predicted;retain(key,it->second.pipeline.Get());*out=it->second.pipeline.Detach();ready.erase(it);g.unlock();
                if(promote)try{publish(stem(key)+L".plan",blob);}catch(const std::exception&){}
                auto n=++hits;if(n==1 || !(n%128))std::fprintf(stderr,"frosty/pso-cache: %u prepared pipelines consumed\n",n);
                return S_OK;
            }}}
        catch(const std::exception&){key.clear();}
        auto hr=build(out);
        if(FAILED(hr) && !key.empty()){
            wchar_t folder[2048];auto n=GetEnvironmentVariableW(L"XS_DEBUG_FAILED_PSOS",folder,std::size(folder));
            if(n && n<std::size(folder))try{publish(std::wstring(folder)+L"\\"+key+L".plan",blob);}catch(const std::exception&){}
        }
        if(SUCCEEDED(hr) && *out && !key.empty()){
            try{auto base=stem(key);if(first)publish(base+L".plan",blob);set_pipeline_hint_path(*out,base);
                std::lock_guard g(lock);retain(key,*out);}catch(const std::exception&){}
        }
        return hr;
    }
    void replay(bool retain,bool offline=false,unsigned first=0){
        SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
        std::map<std::wstring,std::vector<fs::path>> hints;
        std::set<std::wstring> deferred;
        if(retain){
            // Setup has already retried these PSOs in separate processes.
            // Keep the recipe on disk, but don't bake it again at first present.
            auto list=read(fs::path(dir)/L"deferred-v1.txt",1024);
            for(size_t at=0;at+65<=list.size();at+=65)
                if(list[at+64]=='\n' && std::ranges::all_of(std::span(list).subspan(at,64),[](BYTE c){
                    return (c>='0' && c<='9') || (c>='a' && c<='f');
                }))deferred.emplace(list.begin()+at,list.begin()+at+64);
        }
        std::vector<std::tuple<bool,fs::file_time_type,fs::path>> plans;std::set<std::wstring> seen;
        for(bool speculative:{false,true}){
            std::error_code ec;
            for(fs::directory_iterator it(speculative?fs::path(dir)/L"predicted":fs::path(dir),ec),end;!ec && it!=end;it.increment(ec)){
                auto path=it->path();auto n=path.filename().wstring();
                if(n.size()==69 && path.extension()==L".plan" && !deferred.contains(n.substr(0,64)) && seen.insert(n).second)
                    plans.emplace_back(speculative,it->last_write_time(ec),path);
                else if(n.size()==134 && n[64]==L'.' && path.extension()==L".hint")hints[n.substr(0,64)].push_back(path);
            }
        }
        if(offline)std::sort(plans.begin(),plans.end(),[](const auto& a,const auto& b){
            return std::tie(std::get<0>(a),std::get<2>(a))<std::tie(std::get<0>(b),std::get<2>(b));
        });
        else std::sort(plans.begin(),plans.end());
        if(first>plans.size())throw std::runtime_error("pipeline resume offset exceeds recipe count");
        std::atomic<UINT> built{},specialized{},ray_built{},skipped{};
        const bool install_progress=offline&&GetEnvironmentVariableA("FROSTY_INSTALL_PROGRESS",nullptr,0)!=0;
        if(install_progress)std::fprintf(stderr,"frostybird/pso-progress: %u %zu 0 0 0 0\n",first,plans.size());
        std::atomic<size_t> next{first};
        std::map<std::wstring,ComPtr<ID3D12RootSignature>> roots;std::mutex root_lock;
        auto prepare=[&]{
          struct Role{
              bool was=play_timing::prewarming, baking=play_timing::offline_bake;
              explicit Role(bool offline){play_timing::prewarming=true;play_timing::offline_bake=offline;}
              ~Role(){play_timing::prewarming=was;play_timing::offline_bake=baking;}
          } role{offline};
          SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
          for(;;){
             auto i=next.fetch_add(1);if(i>=plans.size() || stop)break;
             auto& [speculative,stamp,path]=plans[i];
             auto key=path.stem().wstring();
             if(install_progress)std::fprintf(stderr,"frostybird/pso-begin: %zu %ls\n",i,key.c_str());
             struct Progress {
                 bool enabled;size_t done,total;
                 const std::atomic<UINT>& built,&variants,&rays,&skipped;
                 ~Progress(){if(enabled)std::fprintf(stderr,"frostybird/pso-progress: %zu %zu %u %u %u %u\n",
                     done,total,built.load(),variants.load(),rays.load(),skipped.load());}
             } progress{install_progress,i+1,plans.size(),built,specialized,ray_built,skipped};
            {std::lock_guard g(lock);if(ready.size()>=4096)break;if(requested.contains(key))continue;preparing.insert(key);}
            struct Publish{State* s;const std::wstring& key;~Publish(){{std::lock_guard g(s->lock);s->preparing.erase(key);}s->published.notify_all();}} publication{this,key};
            try{
                auto b=read(path,max_plan);if(b.empty() || id(b)!=key){++skipped;continue;}
                Reader in{b};UINT m{},which{};in.field(m);in.field(which);if(m!=magic || which<1 || which>3){++skipped;continue;}
                auto rb=in.blob();if(rb.size()<=4)continue;
                auto root_key=id(rb);ComPtr<ID3D12RootSignature> rs;
                {std::lock_guard g(root_lock);auto& saved=roots[root_key];
                    if(!saved){UINT node{};std::memcpy(&node,rb.data(),4);
                        if(FAILED(device->CreateRootSignature(node,rb.data()+4,rb.size()-4,IID_PPV_ARGS(&saved)))){++skipped;continue;}}
                    rs=saved;}
                if(which==3){
                    if(retain){std::lock_guard g(lock);if(ray_ready.size()>=64)continue;}
                    ComPtr<ID3D12StateObject> ray;
                    if(FAILED(replay_ray(device.Get(),rs.Get(),in,ray.GetAddressOf())) || !ray){++skipped;continue;}
                    if(retain){std::lock_guard g(lock);ray_ready.emplace(key,std::move(ray));}
                    ++ray_built;continue;
                }
                ComPtr<ID3D12PipelineState> native;HRESULT hr=E_FAIL;
                if(which==1){
                    D3D12_COMPUTE_PIPELINE_STATE_DESC d{};d.pRootSignature=rs.Get();in.field(d.NodeMask);in.field(d.Flags);in.stage(d.CS);
                    if(in.b.empty())hr=make_compute_variants(device.Get(),d,0,native.GetAddressOf());
                }else{
                    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};d.pRootSignature=rs.Get();BOOL bounds{};in.field(bounds);graphics_state(in,d);
                    UINT n{};in.field(n);if(n>32)continue;
                    std::array<D3D12_INPUT_ELEMENT_DESC,32> es{};
                    for(UINT j=0;j<n;++j){auto name=in.blob();if(name.empty() || name.back() || name.size()>1024)throw std::runtime_error("bad semantic");
                        es[j].SemanticName=reinterpret_cast<const char*>(name.data());element(in,es[j]);}
                    d.InputLayout={n?es.data():nullptr,n};in.stage(d.VS);in.stage(d.PS);in.stage(d.GS);
                    if(in.b.empty())hr=make_graphics_variants(device.Get(),d,native.GetAddressOf(),bounds);
                }
                if(FAILED(hr)||!native){++skipped;continue;}
                set_pipeline_hint_path(native.Get(),stem(key));
                if(retain){std::lock_guard g(lock);ready.emplace(key,Prepared{native,speculative});}
                ++built;
                published.notify_all();
                // Publish the exact base immediately. A title thread need not
                // wait for the worker to finish other bound permutations.
                auto h=std::as_const(hints).find(key);if(h==hints.cend())continue;
                for(auto& hp:h->second){
                    if(stop)break;
                    auto v=read(hp,512);
                    if(v.empty() || hp.filename().wstring()!=key+L"."+id(v)+L".hint")continue;
                    ComPtr<ID3D12PipelineState> variant;
                    if(SUCCEEDED(select_view_variant(native.Get(),v,variant.GetAddressOf(),false)))++specialized;
                }
            }catch(const std::exception&){++skipped;}
          }
        };
        {
            // Keep diagnostic assembly blocks ordered. In normal play the
            // live compiler pool still has capacity for demand work.
            std::vector<std::jthread> workers;
            if(!offline && !GetEnvironmentVariableA("FROSTY_TRACE_PSO",nullptr,0) && plans.size()>=32){
                try{for(unsigned i=0;i<2;++i)workers.emplace_back(prepare);}catch(const std::system_error&){}
            }
            prepare();
        }
        std::fprintf(stderr,"frosty/pso-cache: prepared %u original pipelines and %u bound variants (%u unavailable)\n",built.load(),specialized.load(),skipped.load());
        if(ray_built)std::fprintf(stderr,"frosty/pso-cache: prepared %u exact ray pipelines\n",ray_built.load());
    }
};
PipelinePrep::PipelinePrep(){if(!directory().empty())state_=std::make_unique<State>(directory());}
PipelinePrep::~PipelinePrep()=default;
UINT PipelinePrep::join_and_count_hits(){
    if(!state_)return 0;
    if(state_->worker.joinable())state_->worker.join();
    return state_->hits.load();
}
void PipelinePrep::bake(ID3D12Device* d,unsigned first){
    if(!state_ || state_->started.exchange(true))throw std::runtime_error("offline pipeline recipes unavailable");
    state_->device=d;
    state_->replay(false,true,first);
}
void PipelinePrep::start(ID3D12Device* d,bool retain){
    if(!state_ || state_->started.exchange(true))return;
    state_->device=d;
    try{state_->worker=std::thread([s=state_.get(),retain]{try{s->replay(retain);}catch(const std::exception& e){std::fprintf(stderr,"frosty/pso-cache: %s\n",e.what());}});}
    catch(const std::system_error&){}
}
HRESULT PipelinePrep::compute(ID3D12Device* dev,const D3D12_COMPUTE_PIPELINE_STATE_DESC& d,UINT64 k,ID3D12PipelineState** out){
    auto build=[&](auto p){return make_compute_variants(dev,d,k,p);};
    return state_?state_->get([&]{return pack(d);},build,out):build(out);
}
HRESULT PipelinePrep::graphics(ID3D12Device* dev,const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d,BOOL bounds,ID3D12PipelineState** out){
    auto build=[&](auto p){return make_graphics_variants(dev,d,p,bounds);};
    return state_?state_->get([&]{return pack(d,bounds);},build,out):build(out);
}
HRESULT PipelinePrep::rays(ID3D12Device5* dev,ID3D12RootSignature* root,D3D12_RAYTRACING_SHADER_CONFIG cfg,
        D3D12_RAYTRACING_PIPELINE_CONFIG1 depth,const std::vector<RayPipelineLibrary>& libs,UINT rg,Bytes local,ID3D12StateObject** out){
    Data blob;std::wstring key;
    if(state_)try{
        blob=pack_ray(root,cfg,depth,libs,rg,local);
        if(!blob.empty()){
            key=id(blob);std::unique_lock g(state_->lock);state_->requested.insert(key);
            state_->published.wait(g,[&]{return !state_->preparing.contains(key) || state_->ray_ready.contains(key);});
            if(auto it=state_->ray_ready.find(key);it!=state_->ray_ready.end()){
                *out=it->second.Detach();state_->ray_ready.erase(it);++state_->hits;
                std::fputs("frosty/pso-cache: reused exact native ray pipeline\n",stderr);return S_OK;
            }
        }
    }catch(const std::exception&){key.clear();}
    auto hr=build_ray(dev,root,cfg,depth,libs,rg,local,out);
    if(SUCCEEDED(hr) && !key.empty())try{publish(state_->stem(key)+L".plan",blob);}catch(const std::exception&){}
    return hr;
}
