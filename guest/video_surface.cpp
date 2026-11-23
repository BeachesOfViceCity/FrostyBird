#define NOMINMAX
#include "video_surface.hpp"
#include <wrl/client.h>
#include <atomic>
#include <map>
#include <mutex>
#include <new>
using Microsoft::WRL::ComPtr;
namespace {
struct Surface;
std::mutex lock;
std::map<UINT64,Surface*> aliases;
std::map<ID3D12Resource*,Surface*> surfaces;
struct Surface final:ID3D12Resource {
    std::atomic<ULONG> refs{1};
    ComPtr<ID3D12Resource> y,uv,storage;
    ComPtr<ID3D12Device> dev;
    UINT64 va{},chroma{};
    HRESULT WINAPI QueryInterface(REFIID iid,void** out) override {
        if(!out)return E_POINTER;
        *out=nullptr;
        if(iid!=__uuidof(IUnknown)&&iid!=__uuidof(ID3D12Object)&&iid!=__uuidof(ID3D12DeviceChild)&&
           iid!=__uuidof(ID3D12Pageable)&&iid!=__uuidof(ID3D12Resource))return E_NOINTERFACE;
        *out=this;AddRef();return S_OK;
    }
    ULONG WINAPI AddRef() override {return ++refs;}
    ULONG WINAPI Release() override {
        auto n=--refs;if(!n){{std::lock_guard guard(lock);aliases.erase(chroma);surfaces.erase(this);}delete this;}return n;
    }
    HRESULT WINAPI GetPrivateData(REFGUID g,UINT* n,void* d) override {return y->GetPrivateData(g,n,d);}
    HRESULT WINAPI SetPrivateData(REFGUID g,UINT n,const void* d) override{return y->SetPrivateData(g,n,d);}
    HRESULT WINAPI SetPrivateDataInterface(REFGUID g,const IUnknown* o) override{return y->SetPrivateDataInterface(g,o);}
    HRESULT WINAPI SetName(LPCWSTR n) override{return y->SetName(n);}
    HRESULT WINAPI GetDevice(REFIID id,void** out) override{return dev->QueryInterface(id,out);}
    HRESULT WINAPI Map(UINT n,const D3D12_RANGE* r,void** out) override{return y->Map(n,r,out);}
    void WINAPI Unmap(UINT n,const D3D12_RANGE* r) override{y->Unmap(n,r);}
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    D3D12_RESOURCE_DESC* WINAPI GetDesc(D3D12_RESOURCE_DESC* result) override{*result=y->GetDesc();return result;}
#else
    D3D12_RESOURCE_DESC WINAPI GetDesc() override{return y->GetDesc();}
#endif
    D3D12_GPU_VIRTUAL_ADDRESS WINAPI GetGPUVirtualAddress() override{return va;}
    HRESULT WINAPI WriteToSubresource(UINT n,const D3D12_BOX* b,const void* p,UINT row,UINT slice) override{return y->WriteToSubresource(n,b,p,row,slice);}
    HRESULT WINAPI ReadFromSubresource(void* p,UINT row,UINT slice,UINT n,const D3D12_BOX* b) override{return y->ReadFromSubresource(p,row,slice,n,b);}
    HRESULT WINAPI GetHeapProperties(D3D12_HEAP_PROPERTIES* p,D3D12_HEAP_FLAGS* f) override{return y->GetHeapProperties(p,f);}
};
}
extern "C" HRESULT WINAPI FrostyMakeVideoSurface(ID3D12Resource* y,ID3D12Resource* uv,ID3D12Resource* storage,
                                                  UINT64 offset,ID3D12Resource** out) {
    if(!out)return E_POINTER;
    *out=nullptr;if(!y||!uv||!storage||!offset)return E_INVALIDARG;
    auto s=new(std::nothrow) Surface;if(!s)return E_OUTOFMEMORY;
    s->y=y;s->uv=uv;s->storage=storage;s->va=storage->GetGPUVirtualAddress();s->chroma=s->va+offset;
    auto hr=y->GetDevice(IID_PPV_ARGS(&s->dev));if(FAILED(hr)){delete s;return hr;}
    try{
        std::lock_guard guard(lock);
        if(!s->va || !aliases.try_emplace(s->chroma,s).second){delete s;return E_UNEXPECTED;}
        try{surfaces.emplace(s,s);}catch(...){aliases.erase(s->chroma);throw;}
    }catch(const std::bad_alloc&){delete s;return E_OUTOFMEMORY;}
    *out=s;return S_OK;
}
ID3D12Resource* video_native_resource(ID3D12Resource* r){
    std::lock_guard guard(lock);auto i=surfaces.find(r);return i==surfaces.end()?r:i->second->y.Get();
}
HRESULT video_chroma_alias(ID3D12Device* dev,UINT64 address,const D3D12_RESOURCE_DESC& xs,REFIID iid,void** out){
    std::lock_guard guard(lock);auto i=aliases.find(address);if(i==aliases.end())return S_FALSE;
    auto* s=i->second;auto d=s->uv->GetDesc();
    if(dev!=s->dev.Get() || xs.Dimension!=d.Dimension || xs.Format!=d.Format || xs.Width!=d.Width || xs.Height!=d.Height ||
       xs.DepthOrArraySize!=1 || xs.MipLevels!=1)return E_INVALIDARG;
    return s->uv->QueryInterface(iid,out);
}
