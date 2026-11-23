#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <bit>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <vector>
// External linkage: the implementation is supplied by a separate PE DLL.
namespace storage_abi {
using Microsoft::WRL::ComPtr;
struct File:IUnknown{virtual void WINAPI Close()=0;};
struct Status:IUnknown{virtual bool WINAPI IsComplete(UINT)=0;virtual HRESULT WINAPI GetHResult(UINT)=0;};
struct Desc{UINT64 source;UINT16 capacity;signed char priority;};
struct Request{UINT64 options;void* dest;UINT64 page;UINT dest_size,pad;File* file;UINT64 offset;UINT size,intermediate;UINT64 tag;const char* name;};
struct Queue:IUnknown{virtual void WINAPI RequestRead(const Request*)=0;virtual void WINAPI Marker(Status*,UINT)=0;
    virtual void WINAPI Signal(IUnknown*,UINT64)=0;virtual void WINAPI Submit()=0;};
struct Factory:IUnknown{virtual HRESULT WINAPI NewQueue(const Desc*,REFIID,void**)=0;virtual HRESULT WINAPI Open(const wchar_t*,REFIID,void**)=0;
    virtual HRESULT WINAPI NewStatus(UINT,const char*,REFIID,void**)=0;};
void ok(HRESULT hr){if(FAILED(hr))throw std::runtime_error("storage HRESULT "+std::to_string(unsigned(hr)));}
}
using namespace storage_abi;
int main(int argc,char** argv)try{
    const GUID factory{0xfb3b54ac,0xbf82,0x463a,{0xb7,0x77,0x73,0x19,0x58,0x37,0x86,0x4d}};
    const GUID queue{0xa3f6649a,0x8c2c,0x4b3a,{0x8b,0x6e,0x10,0x18,0x6a,0x04,0xb9,0xbc}};
    const GUID status{0xcd66d752,0x2786,0x44f7,{0x90,0x70,0xe2,0xa6,0xfd,0xad,0x3f,0x1b}};
    const GUID file{0x43704f9f,0x27c6,0x4a9f,{0x88,0x15,0xd1,0x65,0x27,0x89,0x4d,0xf4}};
    using Get=HRESULT(WINAPI*)(REFIID,void**);
    auto get=std::bit_cast<Get>(GetProcAddress(LoadLibraryW(L"xcrdapi.dll"),"DStorageGetFactory"));if(!get)return 2;
    ComPtr<Factory> fac;ok(get(factory,reinterpret_cast<void**>(fac.GetAddressOf())));Desc desc{0,64,0};
    ComPtr<Queue> q;ok(fac->NewQueue(&desc,queue,reinterpret_cast<void**>(q.GetAddressOf())));
    ComPtr<File> in;ok(fac->Open(L"storage-samples.bin",file,reinterpret_cast<void**>(in.GetAddressOf())));
    ComPtr<Status> st;ok(fac->NewStatus(6,"order",status,reinterpret_cast<void**>(st.GetAddressOf())));
    ComPtr<ID3D12Device> dev;ok(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&dev)));
    ComPtr<ID3D12Fence> fence;ok(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    std::array<UINT,2> sizes{};std::ifstream src("storage-samples.bin",std::ios::binary);src.read(reinterpret_cast<char*>(sizes.data()),8);
    if(!src || !sizes[0] || !sizes[1])return 2;
    constexpr UINT n=65536;std::array<std::vector<BYTE>,4> output;for(auto& b:output)b.resize(n);
    auto read=[&](UINT index,UINT set,UINT bytes){Request req{1,output[index].data(),0,n,0,in.Get(),8ull+(set?sizes[0]:0),bytes,0,0,nullptr};q->RequestRead(&req);};
    for(UINT j=0;j<4;++j)read(j,j&1,sizes[j&1]);
    read(0,1,sizes[1]);q->Marker(st.Get(),0);
    read(2,0,2);q->Marker(st.Get(),1);read(2,0,sizes[0]);q->Marker(st.Get(),2);
    // Adjacent compressed sources are candidates for a single physical read.
    // The truncated last block must not prevent the valid first block's write.
    read(1,0,sizes[0]);read(3,1,sizes[1]+64);q->Marker(st.Get(),3);
    read(3,1,sizes[1]);q->Marker(st.Get(),4);q->Signal(fence.Get(),9);q->Marker(st.Get(),5);
    if(st->IsComplete(0))return 3;
    in->Close();in.Reset();q->Submit();q.Reset(); // drains even after the application releases both handles
    if(fence->GetCompletedValue()!=9 || !st->IsComplete(5) || st->GetHResult(0)!=S_OK || SUCCEEDED(st->GetHResult(1)) ||
       st->GetHResult(2)!=S_OK || st->GetHResult(3)!=HRESULT_FROM_WIN32(ERROR_HANDLE_EOF) || st->GetHResult(4)!=S_OK || st->GetHResult(5)!=S_OK)return 4;
    unsigned match=0;for(UINT j=0;j<4;++j)for(UINT i=0;i<n;++i)match+=output[j][i]==BYTE(i*13+((j==1||j==2)?7:53));
    if(argc>1){
        auto stats=std::bit_cast<void(WINAPI*)(UINT64*,UINT64*,UINT64*)>(GetProcAddress(GetModuleHandleW(L"xcrdapi.dll"),"FrostyStorageCacheStats"));
        UINT64 hits{},bytes{},reject{};if(!stats)return 6;stats(&hits,&bytes,&reject);
        std::printf("Prepared blocks: %llu hits (%llu bytes), %llu candidates rejected\n",hits,bytes,reject);
        if(!reject || bytes!=hits*n || (hits!=0)!=(std::string_view(argv[1])=="mixed"))return 7;
    }
    std::printf("DirectStorage: %u/%u bytes, overlapping destination order, partial batch EOF recovery, deferred file close and fence=9\n",match,4*n);
    return match!=4*n;
}catch(const std::exception& e){std::fprintf(stderr,"storage-check: %s\n",e.what());return 1;}
