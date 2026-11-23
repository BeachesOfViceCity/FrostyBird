// Little-endian Xbox One/Series ACP ABI. APU addresses are byte addresses
// in a separate 32-bit address space; they are never truncated CPU pointers.
#pragma once
#define NOMINMAX
#include <windows.h>
#include <cstddef>
#include <cstdint>

namespace shape {
using u32=std::uint32_t;
struct Context { u32 w[16]; };
struct Tables { u32 count[6]; void* cpu[6]; u32 apu[6]; };
struct HeapState { u32 free,used,alignment,largest,allocations; };
struct Message { u32 type,missed,time,detail; u32 tag_low,tag_high,extra; };
static_assert(sizeof(Context)==64 && sizeof(Tables)==96 && offsetof(Tables,cpu[4])==0x38);
static_assert(sizeof(Message)==28 && sizeof(HeapState)==20);
struct Hal {
    virtual HRESULT connect(u32 commands,u32 messages)=0;
    virtual HRESULT disconnect()=0;
    virtual HRESULT submit(u32 op,std::uint64_t tag,u32 flags,const void* args,u32 completion)=0;
    virtual bool pop(Message*)=0;
    virtual u32 available()=0;
    virtual void release()=0;
    virtual HRESULT reserved6()=0;
    virtual HRESULT reserved7()=0;
    virtual HRESULT reserved8()=0;
    virtual HRESULT reserved9()=0;
    virtual HRESULT clock_info(void*)=0;
protected:
    ~Hal()=default;
};
// Internal mapping while the device lock is held. Checks the complete span.
void* map(u32 address,size_t length);
}
extern "C" {
HRESULT WINAPI AcpHalCreate(shape::Hal**);
HRESULT WINAPI AcpHalAllocateShapeContexts(shape::Tables*);
HRESULT WINAPI AcpHalReleaseShapeContexts();
HRESULT WINAPI ApuCreateHeap(shape::u32,shape::u32);
HRESULT WINAPI ApuAlloc(void**,shape::u32*,shape::u32,shape::u32,shape::u32);
HRESULT WINAPI ApuFree(void*);
HRESULT WINAPI ApuHeapGetState(shape::HeapState*,shape::u32);
bool WINAPI ApuIsVirtualAddressValid(const void*,shape::u32);
void* WINAPI ApuMapApuAddress(shape::u32);
shape::u32 WINAPI ApuMapVirtualAddress(const void*);
}
