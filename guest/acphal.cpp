#include "shape_xma.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace shape {
namespace {
SRWLOCK device_lock=SRWLOCK_INIT;
struct Lock {
    Lock(){AcquireSRWLockExclusive(&device_lock);}
    ~Lock(){ReleaseSRWLockExclusive(&device_lock);}
};
constexpr HRESULT acp(unsigned code){return HRESULT(0x8ac80000u|code);}
constexpr HRESULT heap_error(unsigned code){return HRESULT(0x8aca0000u|code);}
struct AlignedFree {void operator()(BYTE* p)const{_aligned_free(p);}};
struct Allocation {
    u32 address,bytes,start;std::unique_ptr<BYTE,AlignedFree> cpu;
};
struct Bank {
    u32 base{},size{};
    std::vector<Allocation> blocks; // sorted by APU address
    HeapState state()const{
        HeapState s{};uint64_t cursor=base;
        for(auto& b:blocks){
            u32 gap=u32(b.start-cursor);s.largest=std::max(s.largest,gap);
            s.used+=b.bytes;s.alignment+=b.address-b.start;cursor=uint64_t(b.address)+b.bytes;
        }
        s.largest=std::max(s.largest,u32(uint64_t(base)+size-cursor));s.free=size-s.used-s.alignment;
        s.allocations=u32(blocks.size());return s;
    }
    HRESULT allocate(void** ptr,u32* addr,u32 bytes,u32 align){
        uint64_t start=base;
        for(auto i=blocks.begin();;){
            uint64_t a=(start+align-1)&~uint64_t(align-1);
            uint64_t end=i==blocks.end()?uint64_t(base)+size:i->start;
            if(a+bytes<=end){
                Allocation b{u32(a),bytes,u32(start),{}};
                b.cpu.reset(static_cast<BYTE*>(_aligned_malloc(bytes,std::max<size_t>(align,sizeof(void*)))));
                if(!b.cpu)return E_OUTOFMEMORY;
                auto* p=b.cpu.get();
                try{blocks.insert(i,std::move(b));}catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
                *ptr=p;if(addr)*addr=u32(a);return S_OK;
            }
            if(i==blocks.end())return E_OUTOFMEMORY;
            start=uint64_t(i->address)+i->bytes;++i;
        }
    }
};
Bank banks[2];
Tables tables{};
bool reserved=false;unsigned connections=0;
std::vector<std::unique_ptr<Xma>> streams;
HRESULT create_banks(u32 main,u32 control){
    constexpr u32 base=0x10000;
    if(!main)return E_INVALIDARG;
    if(banks[0].size)return heap_error(1);
    if(!control)control=0x114000;
    uint64_t second=(uint64_t(base)+main+65535)&~uint64_t(65535);
    if(second+control>uint64_t(UINT32_MAX)+1)return E_OUTOFMEMORY;
    banks[0].base=base;banks[0].size=main;banks[1].base=u32(second);banks[1].size=control;return S_OK;
}
void idle_heap(){
    if(!connections && !reserved && banks[0].blocks.empty() && banks[1].blocks.empty())
        banks[0].size=banks[1].size=0;
}
u32 address_of(const void* ptr){
    auto p=reinterpret_cast<uintptr_t>(ptr);
    for(auto& bank:banks)for(auto& b:bank.blocks){
        auto start=reinterpret_cast<uintptr_t>(b.cpu.get());
        if(p>=start && p-start<b.bytes)return b.address+u32(p-start);
    }
    return 0;
}
HRESULT allocate(void** ptr,u32* addr,u32 bytes,u32 align,u32 type){
    if(ptr)*ptr=nullptr;
    if(addr)*addr=0;
    if(!ptr || !bytes || type<1 || type>2 || align<4 || !std::has_single_bit(align))return E_INVALIDARG;
    if(!banks[0].size)create_banks(64u<<20,0);
    return banks[type-1].allocate(ptr,addr,bytes,align);
}
HRESULT free_block(void* ptr){
    for(auto& bank:banks){
        auto i=std::find_if(bank.blocks.begin(),bank.blocks.end(),[&](auto& b){return b.cpu.get()==ptr;});
        if(i!=bank.blocks.end()){bank.blocks.erase(i);return S_OK;}
    }
    return E_INVALIDARG;
}
void trace(const char* text){
    static const bool enabled=GetEnvironmentVariableA("XS_AUDIO_TRACE",nullptr,0)!=0;
    if(enabled)OutputDebugStringA(text);
}
class Client final:public Hal {
    // Commands are consumed in submission order under device_lock. The first
    // quantum is visible before its completion fence. If left enabled, a
    // context continues at the configured sample rate until blocked or stopped.
    struct Lane {ULONGLONG due{};};
    std::vector<Lane> lanes;
    std::vector<Message> messages;
    u32 head{},count{},missed{},mask{};
    HANDLE wake{},worker{};
    bool connected{},stopping{},logged{},logged_error{};
    void post(u32 type,u32 detail,uint64_t tag,u32 extra=0){
        if(!(mask&type) || messages.empty())return;
        if(count==messages.size()){if(missed!=UINT32_MAX)++missed;return;}
        messages[(head+count++)%messages.size()]={type,std::exchange(missed,0u),GetTickCount(),detail,u32(tag),u32(tag>>32),extra};
    }
    void quantum(u32 id){
        auto& s=streams[id];
        auto& c=static_cast<Context*>(tables.cpu[4])[id];
        unsigned n=s->run(c);
        if(n && !logged){trace("XS audio: SHAPE decoded XMA2 into the guest PCM ring\n");logged=true;}
        if(c.w[3]>>31 && !logged_error){trace("XS audio: invalid XMA2 bitstream or unmapped buffer\n");logged_error=true;}
        if(!n || !(c.w[1]>>31) || (c.w[2]>>31) || (c.w[3]>>31) || (c.w[9]&(1u<<30))){
            lanes[id].due=0;
            if(c.w[9]>>31)post(8,id,0);
        }else{
            constexpr unsigned rates[]={24000,32000,44100,48000};
            unsigned rate=rates[(c.w[1]>>27)&3];
            lanes[id].due=GetTickCount64()+(n*128000+rate-1)/rate;
        }
    }
    static DWORD WINAPI work(void* opaque){
        auto self=static_cast<Client*>(opaque);
        for(;;){
            DWORD wait=INFINITE;
            {
                Lock lk;if(self->stopping)return 0;
                auto now=GetTickCount64();
                for(u32 i=0;i<self->lanes.size();++i){
                    auto& due=self->lanes[i].due;
                    if(due && due<=now)self->quantum(i);
                    if(due)wait=std::min(wait,DWORD(due>now?due-now:0));
                }
            }
            WaitForSingleObject(self->wake,wait);
        }
    }
public:
    HRESULT connect(u32 commands,u32 capacity)override{
        Lock lk;if(connected || stopping)return acp(9);
        if(!commands || commands>UINT32_MAX/256 || capacity>UINT32_MAX/256)return E_INVALIDARG;
        if(!banks[0].size)create_banks(64u<<20,0);
        try{messages.resize(capacity);lanes.resize(streams.size());}catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
        wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!wake)return HRESULT_FROM_WIN32(GetLastError());
        worker=CreateThread(nullptr,0,work,this,0,nullptr);
        if(!worker){auto hr=HRESULT_FROM_WIN32(GetLastError());CloseHandle(wake);return hr;}
        connected=true;++connections;return S_OK;
    }
    HRESULT disconnect()override{
        {Lock lk;if(!connected || stopping)return acp(10);stopping=true;SetEvent(wake);}
        WaitForSingleObject(worker,INFINITE);
        Lock lk;CloseHandle(worker);CloseHandle(wake);worker=wake=nullptr;connected=stopping=false;
        --connections;messages.clear();lanes.clear();head=count=missed=mask=0;idle_heap();return S_OK;
    }
    HRESULT submit(u32 op,uint64_t tag,u32 flags,const void* args,u32 completion)override{
        Lock lk;if(!connected || stopping)return HRESULT(0x8000000eu);
        if(op<1 || op>27 || (op==4)!=(args==nullptr))return E_INVALIDARG;
        volatile LONG* fence=nullptr;
        if(completion){
            if((completion&3) || !(fence=static_cast<LONG*>(map(completion,sizeof(LONG)))))return E_INVALIDARG;
        }
        auto words=static_cast<const u32*>(args);
        if(op==2)mask|=*words;
        else if(op==3)mask&=~*words;
        else if(op>=5 && op<=8){
            if(!reserved)return acp(2);
            bool batch=op==6 || op==8,enable=op<=6;
            auto selected=[&](u32 i){return batch?((words[i/32]>>(i%32))&1)!=0:i==*words;};
            if(!batch && *words>=streams.size())return E_INVALIDARG;
            if(batch)for(u32 i=u32(streams.size());i<512;++i)if(selected(i))return E_INVALIDARG;
            // Clients can connect before allocating their first context table.
            try{
                lanes.resize(streams.size());
                if(enable)for(u32 i=0;i<streams.size();++i)if(selected(i) && !streams[i])streams[i]=std::make_unique<Xma>();
            }catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
            for(u32 i=0;i<streams.size();++i)if(selected(i)){
                if(enable)quantum(i);else lanes[i].due=0;
            }
            SetEvent(wake);
        }else if(op!=4)return E_NOTIMPL;
        post(0x10,op,tag);
        if(fence)InterlockedExchange(fence,LONG(flags));
        return S_OK;
    }
    bool pop(Message* dst)override{
        Lock lk;if(!dst || !count)return false;
        *dst=messages[head];head=(head+1)%messages.size();--count;return true;
    }
    u32 available()override{Lock lk;return count;}
    void release()override{disconnect();delete this;}
    HRESULT reserved6()override{return acp(0xf0e);}
    HRESULT reserved7()override{return acp(0xf0e);}
    HRESULT reserved8()override{return acp(0xf0e);}
    HRESULT reserved9()override{return acp(0xf0e);}
    HRESULT clock_info(void*)override{return E_NOTIMPL;}
};
}
void* map(u32 a,size_t n){
    if(!a || !n)return nullptr;
    for(auto& bank:banks){
        auto& v=bank.blocks;
        auto i=std::upper_bound(v.begin(),v.end(),a,[](u32 key,const auto& b){return key<b.address;});
        if(i!=v.begin()){
            auto& b=*std::prev(i);u32 off=a-b.address;
            if(off<b.bytes && n<=b.bytes-off)return b.cpu.get()+off;
        }
    }
    return nullptr;
}
}
using namespace shape;
extern "C" HRESULT WINAPI ApuCreateHeap(u32 main,u32 control){Lock lk;return create_banks(main,control);}
extern "C" HRESULT WINAPI ApuAlloc(void** p,u32* a,u32 n,u32 align,u32 type){Lock lk;return allocate(p,a,n,align,type);}
extern "C" HRESULT WINAPI ApuFree(void* p){
    Lock lk;if(reserved && tables.cpu[4]==p)return E_INVALIDARG;
    auto hr=free_block(p);if(SUCCEEDED(hr))idle_heap();return hr;
}
extern "C" HRESULT WINAPI ApuHeapGetState(HeapState* s,u32 type){
    Lock lk;if(!s || type<1 || type>2)return E_INVALIDARG;
    if(!banks[0].size)return heap_error(2);
    *s=banks[type-1].state();return S_OK;
}
extern "C" void* WINAPI ApuMapApuAddress(u32 a){Lock lk;return map(a,1);}
extern "C" u32 WINAPI ApuMapVirtualAddress(const void* p){Lock lk;return address_of(p);}
extern "C" bool WINAPI ApuIsVirtualAddressValid(const void* p,u32 align){
    Lock lk;u32 a=address_of(p);return a && (!align || (std::has_single_bit(align) && !(a&(align-1))));
}
extern "C" HRESULT WINAPI AcpHalAllocateShapeContexts(Tables* t){
    Lock lk;if(!t)return E_INVALIDARG;
    if(reserved)return acp(1);
    if(t->count[4]>512)return E_INVALIDARG;
    for(unsigned i:{0u,1u,2u,3u,5u})if(t->count[i])return E_NOTIMPL;
    Tables tmp{};tmp.count[4]=t->count[4];
    try{streams.resize(tmp.count[4]);}catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
    if(tmp.count[4]){
        u32 bytes=tmp.count[4]*sizeof(Context);
        auto hr=allocate(&tmp.cpu[4],&tmp.apu[4],bytes,64,2);
        if(FAILED(hr)){streams.clear();return hr;}
        std::memset(tmp.cpu[4],0,bytes);
    }
    tables=*t=tmp;reserved=true;return S_OK;
}
extern "C" HRESULT WINAPI AcpHalReleaseShapeContexts(){
    Lock lk;if(!reserved)return acp(2);if(connections)return acp(0x10);
    streams.clear();if(tables.cpu[4])free_block(tables.cpu[4]);tables={};reserved=false;idle_heap();return S_OK;
}
extern "C" HRESULT WINAPI AcpHalCreate(Hal** h){
    if(!h)return E_INVALIDARG;
    *h=new(std::nothrow) Client;return *h?S_OK:E_OUTOFMEMORY;
}
