// User-mode title memory for the x64 Wine guest. The page IDs below name
// pagefile sections within this process; they are never host physical addresses.
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <map>
#include <mutex>
#include <shared_mutex>

namespace {
constexpr size_t page = 65536;
constexpr size_t title_budget = 27ull << 29; // 13.5 GiB, Series X advanced memory
using Attr = std::uint64_t;
struct Hooks {
    void* (WINAPI *alloc)(size_t, Attr);
    void (WINAPI *free)(void*, Attr);
};
const Hooks* hooks() {
    static auto ptr = std::bit_cast<const Hooks*>(GetProcAddress(GetModuleHandleW(nullptr), "__XMemAllocationHooks__"));
    return ptr;
}
struct Pages { HANDLE section; size_t count; };
struct Reservation { size_t size; DWORD prot; bool mapped; };
std::shared_mutex page_lock;
std::map<ULONG_PTR, Pages> blocks;
std::map<void*, Reservation> reservations;
std::map<UINT_PTR,size_t> mapped_views;
ULONG_PTR next_id = 32;

void* fail(DWORD error) { SetLastError(error); return nullptr; }
BOOL reject(DWORD error) { SetLastError(error); return FALSE; }
}

extern "C" {
// The desktop tiled-image reader snapshots Xbox shared pages. Keep a
// partially resident texture on one set of section mappings until its
// source snapshot is complete; unrelated ordinary VirtualAlloc memory is
// not owned by this mapper.
__declspec(dllexport) BOOL WINAPI FrostyLockTextureMapping(const void* address,SIZE_T bytes){
    auto lo=reinterpret_cast<UINT_PTR>(address);if(!bytes || bytes>UINTPTR_MAX-lo)return FALSE;
    page_lock.lock_shared();
    auto after=reservations.upper_bound(const_cast<void*>(address));
    if(after!=reservations.end() && reinterpret_cast<UINT_PTR>(after->first)<lo+bytes)return TRUE;
    if(after!=reservations.begin()){
        auto prev=std::prev(after);
        if(lo-reinterpret_cast<UINT_PTR>(prev->first)<prev->second.size)return TRUE;
    }
    page_lock.unlock_shared();return FALSE;
}
__declspec(dllexport) void WINAPI FrostyUnlockTextureMapping(){page_lock.unlock_shared();}

__declspec(dllexport) void* WINAPI XMemAllocDefault(size_t size, Attr attr) {
    auto align = std::max<size_t>(16, size_t(1) << ((attr >> 24) & 31));
    if (size > SIZE_MAX - align - sizeof(void*)) return fail(ERROR_NOT_ENOUGH_MEMORY);
    return _aligned_malloc(std::max<size_t>(size, 1), align);
}
__declspec(dllexport) void WINAPI XMemFreeDefault(void* ptr, Attr) { _aligned_free(ptr); }
__declspec(dllexport) void* WINAPI XMemAlloc(size_t size, Attr attr) {
    auto h = hooks();
    return h && h->alloc ? h->alloc(size, attr) : XMemAllocDefault(size, attr);
}
__declspec(dllexport) void WINAPI XMemFree(void* ptr, Attr attr) {
    if (!ptr) return;
    auto h = hooks();
    if (h && h->free) h->free(ptr, attr);
    else XMemFreeDefault(ptr, attr);
}

__declspec(dllexport) BOOL WINAPI XMemGetWorkingSetStatistics(DWORD flags, void* out) {
    auto version = flags >> 28;
    if (!out || version > 2 || (flags & 0xfffffff) != 1) return reject(ERROR_NOT_SUPPORTED);
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    PROCESS_MEMORY_COUNTERS_EX pm{};
    if (!GlobalMemoryStatusEx(&ms) || !GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof(pm))) return FALSE;
    std::memset(out, 0, version == 2 ? 0x78 : version == 1 ? 0x50 : 0x48);
    auto v = static_cast<ULONG64*>(out);
    v[0] = std::min<ULONG64>(title_budget, ms.ullTotalPhys);
    v[1] = std::min<ULONG64>(v[0], pm.WorkingSetSize);
    v[5] = v[0];
    v[6] = v[1];
    if (version == 2) v[11] = std::min<ULONG64>(v[0], pm.PrivateUsage);
    return TRUE;
}

__declspec(dllexport) void* WINAPI XMemVirtualAlloc(void* addr, size_t size, DWORD type,
                                                     Attr flags, DWORD prot) {
    DWORD host_type = type & (MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN);
    DWORD access = prot & 0xff; // CPU permissions; GPU cache policies are separate
    if (!host_type || !size) return fail(ERROR_INVALID_PARAMETER);
    if ((flags & 0x20) && (host_type & MEM_RESERVE)) {
        if ((size & (page-1)) || (host_type & MEM_COMMIT)) return fail(ERROR_INVALID_PARAMETER);
        void* p = VirtualAlloc2(GetCurrentProcess(), addr, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                PAGE_NOACCESS, nullptr, 0);
        if (p) {
            std::lock_guard guard(page_lock);
            try { reservations.insert_or_assign(p, Reservation{size, access, false}); }
            catch (...) { VirtualFree(p, 0, MEM_RELEASE); return fail(ERROR_NOT_ENOUGH_MEMORY); }
        }
        return p;
    }
    size_t granularity = (type & MEM_64K_PAGES) == MEM_64K_PAGES ? page :
                         (type & MEM_LARGE_PAGES) ? 2 * 1024 * 1024 : 4096;
    if (size > SIZE_MAX - granularity + 1) return fail(ERROR_INVALID_PARAMETER);
    size = (size + granularity - 1) & ~(granularity - 1);
    if (!addr && granularity > page) {
        MEM_ADDRESS_REQUIREMENTS req{nullptr, nullptr, granularity};
        MEM_EXTENDED_PARAMETER param{};
        param.Type = MemExtendedParameterAddressRequirements;
        param.Pointer = &req;
        return VirtualAlloc2(GetCurrentProcess(), nullptr, size, host_type | MEM_RESERVE, access, &param, 1);
    }
    return VirtualAlloc(addr, size, host_type, access);
}

__declspec(dllexport) BOOL WINAPI XMemAllocatePhysicalPages(DWORD flags, ULONG_PTR* count, ULONG_PTR* ids) {
    if (!count || !ids || !*count || *count > title_budget / page || flags != MEM_64K_PAGES)
        return reject(ERROR_INVALID_PARAMETER);
    auto n = *count;
    auto bytes = n * page;
    auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                      DWORD(bytes >> 32), DWORD(bytes), nullptr);
    if (!section) { *count = 0; return FALSE; }
    std::lock_guard guard(page_lock);
    if (next_id > UINTPTR_MAX - n) { CloseHandle(section); *count = 0; return reject(ERROR_NOT_ENOUGH_MEMORY); }
    try { blocks.emplace(next_id, Pages{section, n}); }
    catch (...) { CloseHandle(section); *count = 0; return reject(ERROR_NOT_ENOUGH_MEMORY); }
    for (size_t i = 0; i < n; ++i) ids[i] = next_id + i;
    next_id += n;
    return TRUE;
}

__declspec(dllexport) void* WINAPI XMemMapPhysicalPages(void* addr, ULONG_PTR n, const ULONG_PTR* ids) {
    std::lock_guard guard(page_lock);
    const auto start=reinterpret_cast<UINT_PTR>(addr);
    if (!n || n>title_budget/page || start%page || n*page>UINTPTR_MAX-start) return fail(ERROR_INVALID_PARAMETER);
    auto it = reservations.upper_bound(addr);
    if (it == reservations.begin()) return fail(ERROR_INVALID_ADDRESS);
    --it;
    auto& r = it->second;
    auto within=start-reinterpret_cast<UINT_PTR>(it->first);
    if (within>r.size || n*page>r.size-within) return fail(ERROR_INVALID_ADDRESS);
    // A 64 KiB view per page permits independent unmapping and true aliasing
    // when a streaming mip is populated after the resource is created.
    for(size_t i=0;i<n;++i) {
        auto va=start+i*page;
        auto p=reinterpret_cast<void*>(va);
        auto old=mapped_views.find(va);
        HANDLE backing{};
        UINT64 offset{};
        if (ids) {
            auto b=blocks.upper_bound(ids[i]);
            if (b==blocks.begin()) return fail(ERROR_INVALID_PARAMETER);
            --b;
            if (ids[i]-b->first >= b->second.count) return fail(ERROR_INVALID_PARAMETER);
            backing=b->second.section;
            offset=(ids[i]-b->first)*page;
        }
        if (old!=mapped_views.end()) {
            if (!UnmapViewOfFile2(GetCurrentProcess(),p,MEM_PRESERVE_PLACEHOLDER)) return nullptr;
            mapped_views.erase(old);
        }
        if (!ids) continue;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(p,&mbi,sizeof(mbi)) || mbi.State!=MEM_RESERVE) return fail(ERROR_INVALID_ADDRESS);
        if (mbi.BaseAddress!=p || mbi.RegionSize!=page) {
            if (!VirtualFree(p,page,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)) return nullptr;
        }
        if (!MapViewOfFile3(backing,GetCurrentProcess(),p,offset,page,MEM_REPLACE_PLACEHOLDER,r.prot,nullptr,0)) return nullptr;
        try { mapped_views.emplace(va,page); }
        catch (...) { UnmapViewOfFile2(GetCurrentProcess(),p,MEM_PRESERVE_PLACEHOLDER); return fail(ERROR_NOT_ENOUGH_MEMORY); }
    }
    return addr;
}

__declspec(dllexport) BOOL WINAPI XMemFreePhysicalPages(ULONG_PTR n, const ULONG_PTR* ids) {
    if (!n || !ids) return reject(ERROR_INVALID_PARAMETER);
    std::lock_guard guard(page_lock);
    auto it = blocks.find(ids[0]);
    if (it == blocks.end() || it->second.count != n) return reject(ERROR_NOT_SUPPORTED);
    for (size_t i = 1; i < n; ++i) if (ids[i] != ids[0] + i) return reject(ERROR_NOT_SUPPORTED);
    CloseHandle(it->second.section);
    blocks.erase(it);
    return TRUE;
}
}
