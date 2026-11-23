#define NOMINMAX
#include "ray_memory.hpp"
#include "sbt_prefix.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>
using Microsoft::WRL::ComPtr;
struct RayMemory::Allocation {
    ID3D12Resource* resource; // lookup is valid only while the application owns the GPU address
    UINT64 va,size;
    const BYTE* backing;
    bool readable;
    std::mutex lock;
    std::map<UINT64,std::vector<BYTE>> tables;
};
namespace {
constexpr GUID memory_key{0x2103b54e,0x5574,0x42c3,{0x88,0xcc,0x6e,0xb9,0xc1,0xa8,0x16,0xcf}};
struct Owner final:IUnknown {
    std::atomic<ULONG> refs{1};std::shared_ptr<RayMemory::Allocation> allocation;
    explicit Owner(std::shared_ptr<RayMemory::Allocation> a):allocation(std::move(a)){}
    HRESULT WINAPI QueryInterface(REFIID id,void** out) override{
        if(!out)return E_POINTER;
        *out=nullptr;if(id!=__uuidof(IUnknown))return E_NOINTERFACE;
        *out=this;AddRef();return S_OK;
    }
    ULONG WINAPI AddRef() override{return ++refs;}
    ULONG WINAPI Release() override{auto n=--refs;if(!n)delete this;return n;}
};
std::shared_ptr<RayMemory::Allocation> info(ID3D12Resource* r){
    if(!r)return {};
    Owner* o{};UINT n=sizeof(o);
    if(FAILED(r->GetPrivateData(memory_key,&n,&o)) || !o)return {};
    auto allocation=o->allocation;o->Release();return allocation;
}
bool sample(RayMemory::Allocation& a,UINT64 off,UINT64 n,std::vector<BYTE>& out,bool identify){
    if(off>a.size || n>a.size-off || n>(64u<<20) || n<32)return false;
    {
        std::lock_guard hold(a.lock);auto it=a.tables.upper_bound(off);
        if(it!=a.tables.begin()){
            --it;auto rel=off-it->first;
            if(rel<=it->second.size() && n<=it->second.size()-rel){
                out.assign(it->second.begin()+rel,it->second.begin()+rel+n);return true;
            }
        }
    }
    if(!a.readable && !a.backing)return false;
    const BYTE* bytes=a.backing;
    if(!bytes){D3D12_RANGE range{SIZE_T(off),SIZE_T(off+n)};void* p{};
        if(FAILED(a.resource->Map(0,&range,&p)))return false;
        bytes=static_cast<BYTE*>(p);}
    auto src=bytes+off;bool take=!identify;
    if(identify){
        // A title SBT can begin with already-native or null records. Examine
        // the identifier positions in its prefix, not arbitrary texel data.
        take=sbt_prefix::contains(src,n);
    }
    if(take)out.assign(src,src+n);
    if(!a.backing){D3D12_RANGE none{};a.resource->Unmap(0,&none);}
    return take;
}
}
void RayMemory::track(ID3D12Resource* r,const BYTE* backing){
    if(!r)return;
    auto d=r->GetDesc();if(d.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER)return;
    auto va=r->GetGPUVirtualAddress();if(!va || !d.Width || d.Width>UINT64_MAX-va)return;
    if(info(r))return;
    D3D12_HEAP_PROPERTIES h{};D3D12_HEAP_FLAGS flags{};bool mapped=SUCCEEDED(r->GetHeapProperties(&h,&flags)) &&
        (h.Type==D3D12_HEAP_TYPE_UPLOAD || (h.Type==D3D12_HEAP_TYPE_CUSTOM &&
            (h.CPUPageProperty==D3D12_CPU_PAGE_PROPERTY_WRITE_BACK || h.CPUPageProperty==D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE)));
    auto a=std::make_shared<Allocation>();a->resource=r;a->va=va;a->size=d.Width;a->backing=backing;a->readable=mapped;
    auto* owner=new Owner(a);auto hr=r->SetPrivateDataInterface(memory_key,owner);owner->Release();if(FAILED(hr))return;
    std::lock_guard g(lock);auto [first,last]=buffers.equal_range(va);
    for(auto it=first;it!=last;)if(it->second.expired())it=buffers.erase(it);else ++it;
    if(buffers.size()>8192 && !(buffers.size()%1024))std::erase_if(buffers,[](auto& it){return it.second.expired();});
    buffers.emplace(va,a);largest_buffer=std::max(largest_buffer,UINT64(d.Width));
}
RayMemory::Slice RayMemory::locate(UINT64 va,UINT64 n){
    std::lock_guard g(lock);Slice s{};
    for(auto it=buffers.upper_bound(va);it!=buffers.begin();){
        --it;if(va-it->first>largest_buffer)break;
        auto a=it->second.lock();if(!a || va-it->first>a->size || n>a->size-(va-it->first))continue;
        if(s.allocation)return {};
        s={std::move(a),va-it->first};
    }
    return s;
}
ID3D12Resource* RayMemory::live(const Slice& s){return s.allocation?s.allocation->resource:nullptr;}
bool RayMemory::read(UINT64 va,UINT64 n,std::vector<BYTE>& out){
    auto s=locate(va,n);return s.allocation && sample(*s.allocation,s.offset,n,out,false);
}
void RayMemory::copy(ID3D12Resource* dst,UINT64 d,ID3D12Resource* src,UINT64 s,UINT64 n){
    auto to=info(dst);if(!to || !n || d>to->size || n>to->size-d)return;
    std::vector<BYTE> snap;
    if(n>=32 && !(n%32) && !(d%32))if(auto from=info(src))sample(*from,s,n,snap,true);
    std::lock_guard hold(to->lock);
    std::erase_if(to->tables,[&](const auto& it){return d<it.first+it.second.size() && it.first<d+n;});
    if(!snap.empty())to->tables.emplace(d,std::move(snap));
}
