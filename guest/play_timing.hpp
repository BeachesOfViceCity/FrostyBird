#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdio>

// Opt-in benchmark counters only. Time spent in concurrent workers may
// overlap, so these are attribution hints, not additive GPU pass times.
namespace play_timing {
enum Part {uploads, requests, pipelines, ray_links, dependencies, origins, allocations, queue_submits, count};
inline std::array<std::atomic<UINT64>,count> ticks{};
inline std::atomic<UINT64> direct_planes{},staged_planes{};
inline std::atomic<UINT64> packed_ray_rows{},full_ray_rows{};
inline std::atomic<UINT64> ray_packet_hits{},ray_packet_allocations{};
inline std::atomic<UINT64> upload_reserve_hits{},upload_reserve_misses{};
inline thread_local bool prewarming=false;
// The installer runs serially on its own large-stack PE thread. Submitting
// another task to Wine's thread pool here only adds a second blocking wait.
inline thread_local bool offline_bake=false;
inline UINT64 now(){LARGE_INTEGER n{};QueryPerformanceCounter(&n);return n.QuadPart;}
struct Scope{
    Part part;UINT64 start{};
    explicit Scope(Part p):part(p){
        static const bool enabled=GetEnvironmentVariableW(L"FROSTY_FRAME_TIMES",nullptr,0)!=0;
        if(enabled)start=now();
    }
    ~Scope(){if(start)ticks[part].fetch_add(now()-start,std::memory_order_relaxed);}
};
struct SlowCall{
    Part part;UINT64 begin{},kind,amount;
    UINT heap_flags{},resource_flags{},layout{},state{};UINT64 alignment{};
    SlowCall(Part p,UINT64 k,UINT64 n):part(p),kind(k),amount(n){
        static const bool enabled=GetEnvironmentVariableW(L"FROSTY_FRAME_TIMES",nullptr,0)!=0;
        if(enabled)begin=now();
    }
    ~SlowCall(){
        if(!begin)return;
        auto elapsed=now()-begin;ticks[part].fetch_add(elapsed,std::memory_order_relaxed);
        static const UINT64 hz=[](){LARGE_INTEGER n{};QueryPerformanceFrequency(&n);return UINT64(n.QuadPart);}();
        if(elapsed<hz/50)return;
        FILETIME ft{};GetSystemTimePreciseAsFileTime(&ft);
        auto end=(((UINT64(ft.dwHighDateTime)<<32)|ft.dwLowDateTime)-116444736000000000ull)/10;
        if(part==allocations)std::fprintf(stderr,"frosty/runtime-latency: allocation kind=%llu amount=%llu tid=%lu begin_us=%llu end_us=%llu heap_flags=%#x resource_flags=%#x layout=%u state=%#x alignment=%llu\n",
            kind,amount,GetCurrentThreadId(),end-elapsed*1000000/hz,end,heap_flags,resource_flags,layout,state,alignment);
        else std::fprintf(stderr,"frosty/runtime-latency: submission kind=%llu amount=%llu tid=%lu begin_us=%llu end_us=%llu\n",
            kind,amount,GetCurrentThreadId(),end-elapsed*1000000/hz,end);
    }
};
} 
