#pragma once
#include <windows.h>
#include <bit>
struct MappingLease {
    using Lock=BOOL(WINAPI*)(const void*,SIZE_T);using Unlock=void(WINAPI*)();
    Unlock release{};
    MappingLease(const BYTE* addr,SIZE_T size){
        static auto module=GetModuleHandleW(L"xmem.dll");
        static auto lock=std::bit_cast<Lock>(GetProcAddress(module,"FrostyLockTextureMapping"));
        static auto unlock=std::bit_cast<Unlock>(GetProcAddress(module,"FrostyUnlockTextureMapping"));
        if(lock && unlock && lock(addr,size))release=unlock;
    }
    void close(){if(release){release();release=nullptr;}}
    ~MappingLease(){close();}
};
