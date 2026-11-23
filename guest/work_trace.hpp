#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <compare>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

struct ShaderKeys { UINT64 a{},b{},c{};auto operator<=>(const ShaderKeys&)const=default; };
enum class WorkKind { draw,indexed,dispatch,indirect_draw,indirect_indexed,indirect_dispatch,indirect_other,blas,tlas,rays };
struct WorkKey { ShaderKeys shaders;WorkKind kind;UINT instances{};auto operator<=>(const WorkKey&)const=default; };
struct RecordedWork { WorkKey key;UINT x,y,z;std::array<DWORD,30> rvas{};UINT n{}; };
// Diagnostic recording only: no shader selection, draw, or queue operation
// depends on the trace. Emit a marker only after its command list is submitted.
class WorkTrace {
    FILE* file_{};
    std::mutex lock_;
    std::atomic<UINT64> presents_{};
    UINT64 submissions_{};
    std::map<WorkKey,UINT64> reported_;
    UINT_PTR base_{},end_{};
public:
    WorkTrace();
    ~WorkTrace();
    void presented(){if(file_)++presents_;}
    void record(std::vector<RecordedWork>&,ShaderKeys,WorkKind,UINT x,UINT y,UINT z);
    void submitted(UINT queue_type,const std::vector<RecordedWork>&);
};
