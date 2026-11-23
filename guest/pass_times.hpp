#pragma once
#include "work_trace.hpp"
#include <d3d12.h>
#include <memory>

// An opt-in profiling build only. A recording, including its query memory,
// stays alive until the submitting queue's existing retirement worker runs.
#ifdef FROSTY_PASS_PROFILE
struct TimedRecording;
struct TimedCommand{
    ID3D12GraphicsCommandList* list{};ID3D12QueryHeap* heap{};UINT end{};
    ~TimedCommand(){if(list)list->EndQuery(heap,D3D12_QUERY_TYPE_TIMESTAMP,end);}
};
struct PassTimes{
    std::shared_ptr<TimedRecording> data;
    TimedCommand measure(ID3D12Device*,ID3D12GraphicsCommandList*,const char*,ShaderKeys,UINT=0,UINT=0,UINT=0);
    void close(ID3D12GraphicsCommandList*);
    void reset(){data.reset();}
    static void presented();
    static void save(const TimedRecording&,UINT,UINT64);
};
#else
struct TimedCommand{};
struct PassTimes{
    static TimedCommand measure(ID3D12Device*,ID3D12GraphicsCommandList*,const char*,ShaderKeys,UINT=0,UINT=0,UINT=0){return {};}
    static void close(ID3D12GraphicsCommandList*){}
    static void reset(){}
    static void presented(){}
};
#endif
