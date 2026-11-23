#define NOMINMAX
#include "rt_state.hpp"
#include "recompiled_shader.hpp"
#include "../src/rt_collection.hpp"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <deque>
#include <functional>
#include <mutex>
#include <memory>
using Microsoft::WRL::ComPtr;
namespace {
constexpr GUID bridge_state{0x795ff108,0x1915,0x4537,{0x82,0xd0,0x24,0x46,0xf9,0xc9,0x01,0xfb}};
class State;
struct Recipe;
struct FnRecord{
    const char* name;
    UINT offset;
    UINT16 tag,scratch,lds,reserved;
    UINT usage;
};
struct ExportRecord{
    const wchar_t* name;
    BYTE identifier[32];
    UINT frame,scratch;
    UINT64 reserved;
    UINT16 vgprs,lds;
    UINT tail;
};
struct Table{void* data;UINT64 base;UINT count;UINT pad;};
static_assert(sizeof(FnRecord)==24 && offsetof(FnRecord,usage)==20);
static_assert(sizeof(ExportRecord)==64 && offsetof(ExportRecord,frame)==40);
struct Reflection {
    struct Fn{std::string name;FnRecord row;};
    struct Export{std::wstring name;ExportRecord row;bool identifier;UINT64 key{};std::array<BYTE,32> linked{};};
    std::vector<Fn> functions;
    std::vector<Export> exports;
    std::vector<BYTE> code;
    UINT stack{},scratch{};
    D3D12_RAYTRACING_SHADER_CONFIG shader{};
    D3D12_RAYTRACING_PIPELINE_CONFIG1 pipe{};
    static Reflection from(const frosty::RtCollection&);
    void merge(const Reflection&,const D3D12_EXISTING_COLLECTION_DESC&);
};
Reflection Reflection::from(const frosty::RtCollection& c){
    Reflection out;
    auto& m=c.modules.back();
    out.code.assign(m.code.data.begin(),m.code.data.end());
    auto stats=[](const frosty::RtModule& m,const std::string& symbol){
        FnRecord r{};auto prefix=symbol+".ss_";
        for(size_t j=0;j<m.entries.size();++j)if(m.entries[j].name.starts_with(prefix)){
            auto row=m.metadata.tail(64+24*j);
            r.scratch=std::max(r.scratch,row.at<UINT16>(14));r.lds=std::max(r.lds,row.at<UINT16>(16));
            r.usage=std::max(r.usage&511u,row.at<UINT>(20)&511u) |
                (std::max((r.usage>>11)&255u,(row.at<UINT>(20)>>11)&255u)<<11);
        }
        return r;
    };
    UINT dispatch_scratch=0,traversal=0,worst=0;
    for(size_t j=0;j+1<c.modules.size();++j)
        for(auto& e:c.modules[j].exports)traversal=std::max<UINT>(traversal,stats(c.modules[j],e.symbols[0]).scratch);
    for(size_t j=0;j<m.entries.size();++j){
        auto row=m.metadata.tail(64+24*j);FnRecord f{nullptr,m.entries[j].offset,row.at<UINT16>(12),
            row.at<UINT16>(14),row.at<UINT16>(16),row.at<UINT16>(18),row.at<UINT>(20)};
        if(f.usage&0x400)dispatch_scratch=f.scratch;
        out.functions.push_back({m.entries[j].name,f});
    }
    for(auto& e:m.exports){
        auto s=stats(m,e.symbols[0]);bool group=e.flags&1;
        if(group)for(unsigned k=1;k<4;++k)if(!e.symbols[k].empty()){
            auto child=stats(m,e.symbols[k]);s.scratch=std::max(s.scratch,child.scratch);s.lds=std::max(s.lds,child.lds);
            s.usage=std::max(s.usage&511u,child.usage&511u) | (std::max((s.usage>>11)&255u,(child.usage>>11)&255u)<<11);}
        if(group || e.type==4)worst=std::max(worst,UINT(s.scratch)+traversal);
        else if(e.type==0)worst=std::max(worst,UINT(s.scratch));
        ExportRecord row{};row.frame=e.frame;row.scratch=s.scratch;row.vgprs=UINT16(s.usage&511);row.lds=s.lds;
        auto w=std::wstring(e.name.begin(),e.name.end());auto key=frosty::rt_key(c.blob);
        // An opaque guest SBT identifier; only native identifiers may be
        // consumed by a desktop DispatchRays. This is never a shader/result.
        UINT64 token[]{0x3154525358595453ull,key,frosty::rt_key({{reinterpret_cast<const BYTE*>(e.name.data()),e.name.size()}}),e.type};
        std::memcpy(row.identifier,token,sizeof(token));
        out.exports.push_back({std::move(w),row,!(e.flags&2) && (group||e.type==0||e.type==4),key});
    }
    out.stack=m.stack;out.scratch=4*(worst+dispatch_scratch);
    return out;
}
void Reflection::merge(const Reflection& src,const D3D12_EXISTING_COLLECTION_DESC& c){
    auto base=code.size();if(src.code.size()>UINT_MAX-base)throw std::bad_alloc{};
    code.insert(code.end(),src.code.begin(),src.code.end());
    for(auto v:src.functions){v.row.offset+=UINT(base);functions.push_back(std::move(v));}
    auto insert=[&](Export e){if(std::ranges::none_of(exports,[&](auto& v){return e.name==v.name;}))exports.push_back(std::move(e));};
    for(auto& e:src.exports){
        if(!c.NumExports)insert(e);
        else for(UINT i=0;i<c.NumExports;++i){
            auto& x=c.pExports[i];if(e.name==(x.ExportToRename?x.ExportToRename:x.Name)){auto v=e;v.name=x.Name;insert(std::move(v));}
        }
    }
    stack=std::max(stack,src.stack);scratch=std::max(scratch,src.scratch);
}
class Container final:public IUnknown{
    State& state;
public:
    explicit Container(State& s):state(s){}
    HRESULT WINAPI QueryInterface(REFIID,void**) override;
    ULONG WINAPI AddRef() override;
    ULONG WINAPI Release() override;
    virtual HRESULT WINAPI r3(){return E_NOTIMPL;}
    virtual HRESULT WINAPI r4(){return E_NOTIMPL;}
    virtual HRESULT WINAPI r5(){return E_NOTIMPL;}
    virtual HRESULT WINAPI r6(){return E_NOTIMPL;}
    virtual HRESULT WINAPI r7(){return E_NOTIMPL;}
    virtual HRESULT WINAPI r8(){return E_NOTIMPL;}
    virtual HRESULT WINAPI GetBlobX(UINT,UINT,ID3DBlob**);
};
class Properties final:public ID3D12StateObjectProperties{
    State& state;
public:
    ComPtr<ID3D12StateObjectProperties> host;
    explicit Properties(State& s):state(s){}
    HRESULT WINAPI QueryInterface(REFIID,void**) override;
    ULONG WINAPI AddRef() override;
    ULONG WINAPI Release() override;
    void* WINAPI GetShaderIdentifier(LPCWSTR n) override;
    UINT64 WINAPI GetShaderStackSize(LPCWSTR n) override;
    UINT64 WINAPI GetPipelineStackSize() override;
    void WINAPI SetPipelineStackSize(UINT64 n) override;
    virtual void WINAPI GetInfoX(UINT selector,void*);
};
class State final:public ID3D12StateObject{
    std::atomic<ULONG> refs{1};
public:
    ComPtr<ID3D12StateObject> host;
    ComPtr<IUnknown> device;
    UINT64 source;
    Properties properties{*this};
    Container container{*this};
    Reflection meta;
    ComPtr<ID3D12Resource> isa;
    std::function<HRESULT(ID3D12StateObject**)> link;
    std::shared_ptr<Recipe> empty_base;
    std::mutex link_lock;
    UINT64 requested_stack{};bool set_stack=false,guest_ids=false;
    HRESULT resolve(){
        std::lock_guard g(link_lock);
        if(host)return S_OK;
        if(!link)return E_NOTIMPL;
        ComPtr<ID3D12StateObject> native;auto hr=link(native.GetAddressOf());
        if(FAILED(hr))return hr;
        if(guest_ids){
            // A table was already populated with guest identifiers. The
            // GPU table must be relocated before executing this pipeline.
            std::fprintf(stderr,"frosty/dxr: resolved state needs guest shader-table relocation\n");return E_NOTIMPL;
        }
        hr=native.As(&properties.host);if(FAILED(hr))return hr;
        if(set_stack)properties.host->SetPipelineStackSize(requested_stack);
        host=std::move(native);link={};return S_OK;
    }
    State(ID3D12StateObject* h,IUnknown* d,UINT64 key,Reflection m):host(h),device(d),source(key),meta(std::move(m)){}
    HRESULT WINAPI QueryInterface(REFIID id,void** out) override{
        if(!out)return E_POINTER;
        *out=nullptr;
        if(id==bridge_state || id==__uuidof(ID3D12StateObject) || id==__uuidof(IUnknown) || id==__uuidof(ID3D12Object) ||
           id==__uuidof(ID3D12DeviceChild) || id==__uuidof(ID3D12Pageable))*out=static_cast<ID3D12StateObject*>(this);
        else if(id==__uuidof(ID3D12StateObjectProperties))*out=static_cast<ID3D12StateObjectProperties*>(&properties);
        else return E_NOINTERFACE;
        AddRef();return S_OK;
    }
    ULONG WINAPI AddRef() override{return ++refs;}
    ULONG WINAPI Release() override{auto n=--refs;if(!n)delete this;return n;}
    HRESULT WINAPI GetPrivateData(REFGUID k,UINT* n,void* p) override{return host?host->GetPrivateData(k,n,p):DXGI_ERROR_NOT_FOUND;}
    HRESULT WINAPI SetPrivateData(REFGUID k,UINT n,const void* p) override{return host?host->SetPrivateData(k,n,p):E_NOTIMPL;}
    HRESULT WINAPI SetPrivateDataInterface(REFGUID k,const IUnknown* p) override{return host?host->SetPrivateDataInterface(k,p):E_NOTIMPL;}
    HRESULT WINAPI SetName(LPCWSTR n) override{if(host)return host->SetName(n);label=n?n:L"";return S_OK;}
    std::wstring label;
    HRESULT WINAPI GetDevice(REFIID id,void** out) override{return device->QueryInterface(id,out);}
    virtual void WINAPI GetShaderContainerX(Container** out){if(out)*out=&container;}
};
HRESULT Properties::QueryInterface(REFIID id,void** out){return state.QueryInterface(id,out);}
ULONG Properties::AddRef(){return state.AddRef();}
ULONG Properties::Release(){return state.Release();}
void* Properties::GetShaderIdentifier(LPCWSTR n){
    std::lock_guard g(state.link_lock);if(host)return host->GetShaderIdentifier(n);
    if(!n)return nullptr;
    for(auto& e:state.meta.exports)if(e.name==n && e.identifier){state.guest_ids=true;return e.row.identifier;}
    return nullptr;
}
UINT64 Properties::GetShaderStackSize(LPCWSTR n){
    std::lock_guard g(state.link_lock);if(host)return host->GetShaderStackSize(n);
    if(n)for(auto& e:state.meta.exports)if(e.name==n)return e.row.frame;
    return UINT64_MAX;
}
UINT64 Properties::GetPipelineStackSize(){
    std::lock_guard g(state.link_lock);return host?host->GetPipelineStackSize():state.set_stack?state.requested_stack:state.meta.stack;
}
void Properties::SetPipelineStackSize(UINT64 n){
    std::lock_guard g(state.link_lock);if(host)host->SetPipelineStackSize(n);state.set_stack=true;state.requested_stack=n;
}
void Properties::GetInfoX(UINT selector,void* dest){
    if(!dest)return;
    auto& m=state.meta;
    if(selector==0){
        // SFT stack and lane scratch bytes; the third word is the driver
        // dispatch override (none), the fourth the Xbox property version.
        UINT fields[]{m.stack,m.scratch,0,1};std::memcpy(dest,fields,sizeof(fields));return;
    }
    if(selector!=1)return;
    auto pp=static_cast<ID3DBlob**>(dest);std::memset(dest,0,16);
    SIZE_T bytes=16+sizeof(ExportRecord)*m.exports.size();for(auto& e:m.exports)bytes+=(e.name.size()+1)*sizeof(wchar_t);
    ComPtr<ID3DBlob> blob;if(FAILED(D3DCreateBlob(bytes,&blob)))return;
    auto* p=static_cast<BYTE*>(blob->GetBufferPointer());std::memset(p,0,bytes);
    auto* rows=reinterpret_cast<ExportRecord*>(p+16);std::memcpy(p,&rows,8);UINT count=UINT(m.exports.size());std::memcpy(p+8,&count,4);
    auto* names=reinterpret_cast<wchar_t*>(rows+count);
    std::lock_guard g(state.link_lock);
    for(size_t i=0;i<count;++i){auto& e=m.exports[i];rows[i]=e.row;rows[i].name=names;
        std::memcpy(names,e.name.c_str(),(e.name.size()+1)*sizeof(wchar_t));names+=e.name.size()+1;
        if(e.identifier && host){auto id=host->GetShaderIdentifier(e.name.c_str());
            if(id)std::memcpy(rows[i].identifier,id,sizeof(rows[i].identifier));else std::memset(rows[i].identifier,0,32);}
        if(!e.identifier)std::memset(rows[i].identifier,0,32);
        else if(!host)state.guest_ids=true;}
    *pp=blob.Detach();
}
ULONG Container::AddRef(){return state.AddRef();}
ULONG Container::Release(){return state.Release();}
HRESULT Container::QueryInterface(REFIID id,void** out){
    if(!out)return E_POINTER;
    *out=nullptr;
    if(id!=__uuidof(IUnknown))return E_NOINTERFACE;
    *out=this;AddRef();return S_OK;
}
HRESULT Container::GetBlobX(UINT kind,UINT index,ID3DBlob** out){
    if(!out)return E_POINTER;
    *out=nullptr;
    if(kind!=1 || index)return E_INVALIDARG;
    auto& m=state.meta;SIZE_T bytes=sizeof(Table)+m.functions.size()*sizeof(FnRecord);
    for(auto& f:m.functions)bytes+=f.name.size()+1;
    ComPtr<ID3DBlob> blob;auto hr=D3DCreateBlob(bytes,&blob);if(FAILED(hr))return hr;
    auto* h=static_cast<Table*>(blob->GetBufferPointer());auto* rows=reinterpret_cast<FnRecord*>(h+1);
    *h={rows,state.isa?state.isa->GetGPUVirtualAddress():0,UINT(m.functions.size()),0};auto* names=reinterpret_cast<char*>(rows+h->count);
    for(size_t i=0;i<h->count;++i){auto& f=m.functions[i];rows[i]=f.row;rows[i].name=names;
        std::memcpy(names,f.name.c_str(),f.name.size()+1);names+=f.name.size()+1;}
    *out=blob.Detach();return S_OK;
}
using LinkFn=std::function<HRESULT(ID3D12StateObject**)>;
HRESULT wrap(ID3D12Device5* dev,ID3D12StateObject* host,IUnknown* device,UINT64 key,Reflection m,REFIID iid,void** out,LinkFn link={},std::shared_ptr<Recipe> empty={}){
    auto s=new State(host,device,key,std::move(m));s->link=std::move(link);s->empty_base=std::move(empty);auto hr=host?s->host.As(&s->properties.host):S_OK;
    if(SUCCEEDED(hr) && host)for(auto& e:s->meta.exports)if(e.identifier){
        auto id=s->properties.host->GetShaderIdentifier(e.name.c_str());
        if(id)std::memcpy(e.linked.data(),id,e.linked.size());
    }
    if(SUCCEEDED(hr) && !s->meta.code.empty()){
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_UPLOAD;hp.CreationNodeMask=hp.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_BUFFER,0,s->meta.code.size(),1,1,1,DXGI_FORMAT_UNKNOWN,{1,0},D3D12_TEXTURE_LAYOUT_ROW_MAJOR,D3D12_RESOURCE_FLAG_NONE};
        hr=dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&s->isa));
        void* data{};if(SUCCEEDED(hr))hr=s->isa->Map(0,nullptr,&data);
        if(SUCCEEDED(hr)){std::memcpy(data,s->meta.code.data(),s->meta.code.size());s->isa->Unmap(0,nullptr);}
    }
    if(SUCCEEDED(hr))hr=s->QueryInterface(iid,out);
    s->Release();return hr;
}
// Own every pointer that can outlive the creation call during PSO prewarm.
struct Recipe {
    std::vector<D3D12_STATE_SUBOBJECT> subs;
    std::deque<std::vector<BYTE>> storage;
    std::deque<std::wstring> names;
    std::deque<std::vector<D3D12_EXPORT_DESC>> exports;
    std::deque<std::vector<LPCWSTR>> refs;
    std::vector<ComPtr<IUnknown>> keep;
    D3D12_STATE_OBJECT_TYPE type{};
    template<class T>T* copy(const void* p){if(!p)throw frosty::Error("null state subobject");auto& b=storage.emplace_back(sizeof(T));
        std::memcpy(b.data(),p,sizeof(T));return reinterpret_cast<T*>(b.data());}
    LPCWSTR name(LPCWSTR n){return n?names.emplace_back(n).c_str():nullptr;}
    template<class P> void ex(UINT n,P& p){if(!n){p=nullptr;return;}if(!p)throw frosty::Error("missing RT exports");
        auto& v=exports.emplace_back(p,p+n);for(auto& e:v){e.Name=name(e.Name);e.ExportToRename=name(e.ExportToRename);}p=v.data();}
    explicit Recipe(const D3D12_STATE_OBJECT_DESC& d):subs(d.pSubobjects,d.pSubobjects+d.NumSubobjects),type(d.Type){
        for(auto& s:subs)switch(s.Type){
        case D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION:{auto* c=copy<D3D12_EXISTING_COLLECTION_DESC>(s.pDesc);s.pDesc=c;keep.emplace_back(c->pExistingCollection);ex(c->NumExports,c->pExports);break;}
        case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE:
        case D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE:{auto p=copy<ID3D12RootSignature*>(s.pDesc);s.pDesc=p;keep.emplace_back(*p);break;}
        case D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG:s.pDesc=copy<D3D12_STATE_OBJECT_CONFIG>(s.pDesc);break;
        case D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK:s.pDesc=copy<D3D12_NODE_MASK>(s.pDesc);break;
        case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG:s.pDesc=copy<D3D12_RAYTRACING_SHADER_CONFIG>(s.pDesc);break;
        case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG:s.pDesc=copy<D3D12_RAYTRACING_PIPELINE_CONFIG>(s.pDesc);break;
        case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1:s.pDesc=copy<D3D12_RAYTRACING_PIPELINE_CONFIG1>(s.pDesc);break;
        case D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP:{auto* h=copy<D3D12_HIT_GROUP_DESC>(s.pDesc);s.pDesc=h;h->HitGroupExport=name(h->HitGroupExport);h->AnyHitShaderImport=name(h->AnyHitShaderImport);
            h->ClosestHitShaderImport=name(h->ClosestHitShaderImport);h->IntersectionShaderImport=name(h->IntersectionShaderImport);break;}
        case D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION:{auto* a=copy<D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION>(s.pDesc);s.pDesc=a;
            auto addr=reinterpret_cast<UINT_PTR>(a->pSubobjectToAssociate),base=reinterpret_cast<UINT_PTR>(d.pSubobjects);
            if(addr<base || (addr-base)%sizeof(s) || (addr-base)/sizeof(s)>=subs.size())throw frosty::Error("invalid RT association");
            a->pSubobjectToAssociate=&subs[(addr-base)/sizeof(s)];auto& v=refs.emplace_back();
            for(UINT i=0;i<a->NumExports;++i)v.push_back(name(a->pExports[i]));
            a->pExports=v.data();break;}
        case D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY:{auto* l=copy<D3D12_DXIL_LIBRARY_DESC>(s.pDesc);s.pDesc=l;
            auto src=static_cast<const BYTE*>(l->DXILLibrary.pShaderBytecode);auto& b=storage.emplace_back(src,src+l->DXILLibrary.BytecodeLength);
            l->DXILLibrary.pShaderBytecode=b.data();ex(l->NumExports,l->pExports);break;}
        default:throw frosty::Error("unmapped deferred RT subobject");
        }
    }
};
}
std::vector<RayIdentifier> rt_identifiers(ID3D12StateObject* p){
    std::vector<RayIdentifier> out;ComPtr<State> s;
    if(!p || FAILED(p->QueryInterface(bridge_state,(void**)s.GetAddressOf())))return out;
    std::lock_guard hold(s->link_lock);
    for(auto& e:s->meta.exports)if(e.identifier){
        RayIdentifier v{{},e.name,e.key};std::memcpy(v.id.data(),e.row.identifier,32);out.push_back(v);
        if(e.linked!=std::array<BYTE,32>{}){v.id=e.linked;out.push_back(std::move(v));}
    }
    return out;
}
bool rt_settings(ID3D12StateObject* p,D3D12_RAYTRACING_SHADER_CONFIG& shader,D3D12_RAYTRACING_PIPELINE_CONFIG1& pipe){
    ComPtr<State> s;if(!p || FAILED(p->QueryInterface(bridge_state,(void**)s.GetAddressOf())))return false;
    shader=s->meta.shader;pipe=s->meta.pipe;return shader.MaxPayloadSizeInBytes && pipe.MaxTraceRecursionDepth;
}
HRESULT resolve_rt_state(ID3D12StateObject* p,ID3D12StateObject** out){
    if(!p || !out)return E_POINTER;
    *out=nullptr;ComPtr<State> s;
    if(FAILED(p->QueryInterface(bridge_state,(void**)s.GetAddressOf()))){p->AddRef();*out=p;return S_OK;}
    auto hr=s->resolve();return FAILED(hr)?hr:s->host.CopyTo(out);
}
ID3D12StateObject* native_rt_state(ID3D12StateObject* p){
    if(!p)return nullptr;
    State* s{};
    if(FAILED(p->QueryInterface(bridge_state,(void**)&s)))return p;
    auto native=s->host.Get();s->Release();return native;
}
HRESULT xs_rt_pipeline(ID3D12Device5* dev,IUnknown* device,const D3D12_STATE_OBJECT_DESC* d,REFIID iid,void** out)try{
    if(!out)return E_POINTER;
    *out=nullptr;
    if(!dev || !d || (d->NumSubobjects && !d->pSubobjects))return E_INVALIDARG;
    auto desc=*d;std::vector<D3D12_STATE_SUBOBJECT> subs;Reflection meta;
    std::vector<D3D12_EXISTING_COLLECTION_DESC> collections(d->NumSubobjects);
    std::vector<D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION> links(d->NumSubobjects);
    for(UINT i=0;i<d->NumSubobjects;++i)subs.push_back(d->pSubobjects[i]);
    bool pending=false;
    for(UINT i=0;i<subs.size();++i){
        auto& sub=subs[i];
        if(sub.pDesc && sub.Type==D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG)meta.shader=*static_cast<const D3D12_RAYTRACING_SHADER_CONFIG*>(sub.pDesc);
        if(sub.pDesc && sub.Type==D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG)meta.pipe.MaxTraceRecursionDepth=static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG*>(sub.pDesc)->MaxTraceRecursionDepth;
        if(sub.pDesc && sub.Type==D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1)meta.pipe=*static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG1*>(sub.pDesc);
        if(sub.Type==D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG && sub.pDesc && GetEnvironmentVariableA("FROSTY_TRACE_SBT",nullptr,0)){
            auto& cfg=*static_cast<const D3D12_RAYTRACING_SHADER_CONFIG*>(sub.pDesc);
            char msg[140];std::snprintf(msg,sizeof(msg),"frosty/dxr: pipeline payload=%u attributes=%u subobjects=%u\n",
                cfg.MaxPayloadSizeInBytes,cfg.MaxAttributeSizeInBytes,d->NumSubobjects);OutputDebugStringA(msg);
        }
        if(sub.Type==D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION){
            if(!sub.pDesc)return E_INVALIDARG;
            auto& c=collections[i];c=*static_cast<const D3D12_EXISTING_COLLECTION_DESC*>(sub.pDesc);
            if(c.pExistingCollection){ComPtr<State> src;
                if(SUCCEEDED(c.pExistingCollection->QueryInterface(bridge_state,(void**)src.GetAddressOf())))meta.merge(src->meta,c);}
            c.pExistingCollection=native_rt_state(c.pExistingCollection);pending|=!c.pExistingCollection;sub.pDesc=&c;
        }else if(sub.Type==D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION){
            if(!sub.pDesc)return E_INVALIDARG;
            auto& a=links[i];a=*static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(sub.pDesc);
            auto base=reinterpret_cast<UINT_PTR>(d->pSubobjects),to=reinterpret_cast<UINT_PTR>(a.pSubobjectToAssociate);
            if(to<base || (to-base)%sizeof(sub) || (to-base)/sizeof(sub)>=subs.size())return E_INVALIDARG;
            a.pSubobjectToAssociate=&subs[(to-base)/sizeof(sub)];sub.pDesc=&a;
        }
    }
    if(pending){
        auto recipe=std::make_shared<Recipe>(*d);ComPtr<ID3D12Device5> backend=dev;
        auto link=[recipe,backend](ID3D12StateObject** out)->HRESULT{
            Recipe resolved({recipe->type,UINT(recipe->subs.size()),recipe->subs.data()});
            std::vector<ComPtr<ID3D12StateObject>> natives;
            for(auto& s:resolved.subs)if(s.Type==D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION){
                auto* c=const_cast<D3D12_EXISTING_COLLECTION_DESC*>(static_cast<const D3D12_EXISTING_COLLECTION_DESC*>(s.pDesc));
                auto& native=natives.emplace_back();auto hr=resolve_rt_state(c->pExistingCollection,native.GetAddressOf());
                if(FAILED(hr))return hr;
                c->pExistingCollection=native.Get();}
            D3D12_STATE_OBJECT_DESC desc{resolved.type,UINT(resolved.subs.size()),resolved.subs.data()};return backend->CreateStateObject(&desc,IID_PPV_ARGS(out));};
        return wrap(dev,nullptr,device,0,std::move(meta),iid,out,std::move(link));
    }
    // The Xbox material pipeline can start with configs and no shader. No
    // Vulkan executable exists until its first collection is added.
    if(desc.Type==D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE && !subs.empty() &&
       std::ranges::none_of(subs,[](const auto& s){return s.Type==D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION ||
           s.Type==D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY || s.Type==D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;})){
        auto seed=std::make_shared<Recipe>(*d);
        std::fprintf(stderr,"frosty/dxr: retained configured empty RT pipeline (%u subobjects)\n",d->NumSubobjects);
        return wrap(dev,nullptr,device,0,std::move(meta),iid,out,{},std::move(seed));
    }
    desc.pSubobjects=subs.data();ComPtr<ID3D12StateObject> host;
    auto hr=dev->CreateStateObject(&desc,IID_PPV_ARGS(&host));
    if(FAILED(hr)){std::fprintf(stderr,"frosty/dxr: native state type=%u hr=%#lx subobjects:",UINT(d->Type),(unsigned long)hr);
        for(auto& s:subs)std::fprintf(stderr," %u",UINT(s.Type));
        std::fputc('\n',stderr);return hr;}
    return wrap(dev,host.Get(),device,0,std::move(meta),iid,out);
}catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
catch(const frosty::Error& e){std::fprintf(stderr,"frosty/dxr: %s\n",e.what());return E_NOTIMPL;}
HRESULT xs_rt_extend(ID3D12Device7* dev,IUnknown* device,const D3D12_STATE_OBJECT_DESC* d,ID3D12StateObject* base,REFIID iid,void** out)try{
    if(!out)return E_POINTER;
    *out=nullptr;
    if(!dev || !d || !base || !d->pSubobjects || d->Type!=D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE)return E_INVALIDARG;
    ComPtr<State> parent;if(FAILED(base->QueryInterface(bridge_state,(void**)parent.GetAddressOf())))return E_INVALIDARG;
    auto add=std::make_shared<Recipe>(*d);
    if(auto seed=parent->empty_base){
        // Instantiate the first executable with both sets of subobjects.
        // Export associations point into their own original subobject array.
        std::vector<D3D12_STATE_SUBOBJECT> all;all.reserve(seed->subs.size()+add->subs.size());
        std::deque<D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION> associations;
        for(auto* recipe:{seed.get(),add.get()}){
            size_t start=all.size();all.insert(all.end(),recipe->subs.begin(),recipe->subs.end());
            for(size_t i=start;i<all.size();++i)if(all[i].Type==D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION){
                auto& a=associations.emplace_back(*static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(all[i].pDesc));
                a.pSubobjectToAssociate=all.data()+start+(a.pSubobjectToAssociate-recipe->subs.data());all[i].pDesc=&a;
            }
        }
        D3D12_STATE_OBJECT_DESC combined{d->Type,UINT(all.size()),all.data()};return xs_rt_pipeline(dev,device,&combined,iid,out);
    }
    Reflection meta=parent->meta;bool pending=!parent->host;
    for(auto& s:add->subs)if(s.Type==D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION){
        auto& c=*static_cast<const D3D12_EXISTING_COLLECTION_DESC*>(s.pDesc);ComPtr<State> child;
        if(!c.pExistingCollection)return E_INVALIDARG;
        if(SUCCEEDED(c.pExistingCollection->QueryInterface(bridge_state,(void**)child.GetAddressOf())))meta.merge(child->meta,c);
        pending|=!native_rt_state(c.pExistingCollection);
    }
    ComPtr<ID3D12Device7> backend=dev;
    auto link=[backend,parent,add](ID3D12StateObject** out)->HRESULT{
        ComPtr<ID3D12StateObject> native_parent;auto hr=resolve_rt_state(parent.Get(),native_parent.GetAddressOf());if(FAILED(hr))return hr;
        Recipe r({add->type,UINT(add->subs.size()),add->subs.data()});
        std::vector<ComPtr<ID3D12StateObject>> keep;
        for(auto& s:r.subs)if(s.Type==D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION){
            auto* c=const_cast<D3D12_EXISTING_COLLECTION_DESC*>(static_cast<const D3D12_EXISTING_COLLECTION_DESC*>(s.pDesc));
            auto& child=keep.emplace_back();hr=resolve_rt_state(c->pExistingCollection,child.GetAddressOf());if(FAILED(hr))return hr;c->pExistingCollection=child.Get();
        }
        D3D12_STATE_OBJECT_DESC desc{r.type,UINT(r.subs.size()),r.subs.data()};
        return backend->AddToStateObject(&desc,native_parent.Get(),IID_PPV_ARGS(out));
    };
    if(pending)return wrap(dev,nullptr,device,0,std::move(meta),iid,out,std::move(link));
    ComPtr<ID3D12StateObject> host;auto hr=link(host.GetAddressOf());return FAILED(hr)?hr:wrap(dev,host.Get(),device,0,std::move(meta),iid,out);
}catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
catch(const frosty::Error& e){std::fprintf(stderr,"frosty/dxr: RT addition: %s\n",e.what());return E_NOTIMPL;}
HRESULT xs_rt_collection(ID3D12Device5* dev,IUnknown* device,const void* bytes,UINT size,ID3D12RootSignature* root,UINT flags,REFIID iid,void** out)try{
    if(!out)return E_POINTER;
    *out=nullptr;
    if(iid!=__uuidof(ID3D12StateObject))return E_NOINTERFACE;
    if(!dev || !bytes || size<20 || size>(64u<<20) || flags)return E_INVALIDARG;
    const auto key=shader_key({bytes,size});UINT version{};std::memcpy(&version,bytes,4);
    wchar_t stem[32]{};std::swprintf(stem,std::size(stem),L"\\%016llx",key);
    wchar_t dir[32768]{};auto n=GetEnvironmentVariableW(L"FROSTY_CAPTURE_DIR",dir,DWORD(std::size(dir)));
    if(n && n<std::size(dir)){
        auto name=std::wstring(dir)+stem+L".xsrt";
        auto f=CreateFileW(name.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(f!=INVALID_HANDLE_VALUE){DWORD wrote;WriteFile(f,bytes,size,&wrote,nullptr);CloseHandle(f);}
    }
    n=GetEnvironmentVariableW(L"FROSTY_RT_LIBRARY_PATH",dir,DWORD(std::size(dir)));
    ComPtr<ID3DBlob> original,dxil,abi;
    auto load=[&](const wchar_t* ext,ComPtr<ID3DBlob>& b){
        return n && n<std::size(dir) && SUCCEEDED(D3DReadFileToBlob((std::wstring(dir)+stem+ext).c_str(),&b));
    };
    auto raw=frosty::Bytes{{static_cast<const BYTE*>(bytes),size}};auto parsed=frosty::rt_collection(raw);
    if(version!=2)return E_NOTIMPL;
    if(!load(L".xsrt",original)){
        auto source=std::make_shared<std::vector<BYTE>>(raw.data.begin(),raw.data.end());ComPtr<ID3D12Device5> back=dev;
        ComPtr<IUnknown> parent=device;ComPtr<ID3D12RootSignature> signature=root;auto path=std::wstring(dir)+stem+L".rt.dxil";
        auto link=[source,back,parent,signature,key,path](ID3D12StateObject** o)->HRESULT{
            if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES){
                char msg[160];std::snprintf(msg,sizeof(msg),"frosty/dxr: dispatch needs untranslated collection %016llx (%zu bytes)\n",key,source->size());
                OutputDebugStringA(msg);std::fputs(msg,stderr);return E_NOTIMPL;}
            ComPtr<ID3D12StateObject> state;auto hr=xs_rt_collection(back.Get(),parent.Get(),source->data(),UINT(source->size()),signature.Get(),0,IID_PPV_ARGS(&state));
            if(FAILED(hr))return hr;
            auto native=native_rt_state(state.Get());if(!native)return E_NOTIMPL;
            native->AddRef();*o=native;return S_OK;};
        return wrap(dev,nullptr,device,key,Reflection::from(parsed),iid,out,std::move(link));
    }
    if(original->GetBufferSize()!=size || std::memcmp(original->GetBufferPointer(),bytes,size) || !load(L".rt.dxil",dxil) || !load(L".rt.abi",abi) || abi->GetBufferSize()!=8)return E_INVALIDARG;
    D3D12_RAYTRACING_SHADER_CONFIG config{};std::memcpy(&config,abi->GetBufferPointer(),8);
    D3D12_DXIL_LIBRARY_DESC lib{{dxil->GetBufferPointer(),dxil->GetBufferSize()},0,nullptr};
    D3D12_GLOBAL_ROOT_SIGNATURE global{root};D3D12_RAYTRACING_PIPELINE_CONFIG recursion{1};
    D3D12_STATE_OBJECT_CONFIG sc{D3D12_STATE_OBJECT_FLAG_ALLOW_EXTERNAL_DEPENDENCIES_ON_LOCAL_DEFINITIONS};
    std::vector<D3D12_STATE_SUBOBJECT> subs{{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,&lib},{D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG,&config},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG,&recursion},{D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG,&sc}};
    if(root)subs.push_back({D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,&global});
    struct Group{std::array<std::wstring,4> names;D3D12_HIT_GROUP_DESC d{};};std::vector<Group> groups;
    for(auto& e:parsed.modules.back().exports)if(e.flags&1){auto& g=groups.emplace_back();
        for(size_t i=0;i<4;++i)g.names[i].assign(e.symbols[i].begin(),e.symbols[i].end());}
    for(auto& g:groups){auto str=[&](unsigned i){return g.names[i].empty()?nullptr:g.names[i].c_str();};
        g.d={str(0),str(3)?D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE:D3D12_HIT_GROUP_TYPE_TRIANGLES,str(2),str(1),str(3)};
        subs.push_back({D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,&g.d});}
    D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_COLLECTION,UINT(subs.size()),subs.data()};
    ComPtr<ID3D12StateObject> host;auto hr=dev->CreateStateObject(&desc,IID_PPV_ARGS(&host));
    if(SUCCEEDED(hr))hr=wrap(dev,host.Get(),device,key,Reflection::from(parsed),iid,out);
    if(GetEnvironmentVariableA("FROSTY_TRACE_WORK",nullptr,0) || FAILED(hr))
        std::fprintf(stderr,"frosty/dxr: native collection %016llx DXIL %zu bytes hr=%#lx\n",key,dxil->GetBufferSize(),(unsigned long)hr);
    return hr;
}catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
catch(const frosty::Error& e){std::fprintf(stderr,"frosty/dxr: invalid collection reflection: %s\n",e.what());return E_INVALIDARG;}
