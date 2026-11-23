#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <thread>

extern "C" {
__declspec(dllimport) void* WINAPI XMemVirtualAlloc(void*,size_t,DWORD,std::uint64_t,DWORD);
__declspec(dllimport) BOOL WINAPI XMemAllocatePhysicalPages(DWORD,ULONG_PTR*,ULONG_PTR*);
__declspec(dllimport) void* WINAPI XMemMapPhysicalPages(void*,ULONG_PTR,const ULONG_PTR*);
__declspec(dllimport) BOOL WINAPI XMemFreePhysicalPages(ULONG_PTR,const ULONG_PTR*);
__declspec(dllimport) BOOL WINAPI FrostyLockTextureMapping(const void*,SIZE_T);
__declspec(dllimport) void WINAPI FrostyUnlockTextureMapping();
}
int main() {
    constexpr size_t page=65536;
    ULONG_PTR ids[2],n=2;
    if (!XMemAllocatePhysicalPages(MEM_64K_PAGES,&n,ids) || n!=2) return 1;
    auto a=static_cast<DWORD*>(XMemVirtualAlloc(nullptr,page*3,MEM_RESERVE|MEM_64K_PAGES,0x23,PAGE_READWRITE));
    auto b=static_cast<DWORD*>(XMemVirtualAlloc(nullptr,page*2,MEM_RESERVE|MEM_64K_PAGES,0x23,PAGE_READONLY));
    auto alias=a ? a+page/sizeof(DWORD) : nullptr;
    if (!a || !b || !XMemMapPhysicalPages(alias,2,ids)) {
        std::fprintf(stderr,"partial view failed: Win32 %lu\n",GetLastError());return 2;
    }
    alias[0]=0xe5a1;
    alias[page/sizeof(DWORD)]=0xc763;
    std::swap(ids[0],ids[1]);
    if (!XMemMapPhysicalPages(b,2,ids) || b[0]!=0xc763 || b[page/sizeof(DWORD)]!=0xe5a1) return 3;
    if(!FrostyLockTextureMapping(alias+5,page))return 8;
    HANDLE started=CreateEventW(nullptr,TRUE,FALSE,nullptr),removed=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    bool unmapped=false;std::thread writer([&]{SetEvent(started);unmapped=XMemMapPhysicalPages(alias,2,nullptr)!=nullptr;SetEvent(removed);});
    WaitForSingleObject(started,INFINITE);
    auto blocked=WaitForSingleObject(removed,25)==WAIT_TIMEOUT;
    FrostyUnlockTextureMapping();writer.join();CloseHandle(started);CloseHandle(removed);
    if (!blocked || !unmapped || b[0]!=0xc763 || !XMemMapPhysicalPages(b,2,nullptr)) return 4;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(alias,&info,sizeof(info)) || info.State!=MEM_RESERVE) return 5;
    std::swap(ids[0],ids[1]);
    if (!XMemFreePhysicalPages(2,ids)) return 6;
    for(auto range:{std::pair{a,3u},std::pair{b,2u}}) {
        if(!VirtualFree(range.first,page*range.second,MEM_RELEASE|MEM_COALESCE_PLACEHOLDERS) ||
           !VirtualFree(range.first,0,MEM_RELEASE)) return 7;
    }
    std::puts("XMem: partial map, two-way alias, texture read lease, unmap and release verified");
    return 0;
}
