#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <map>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include "gpu_retirement.hpp"
#include "ray_rows.hpp"

// Weak, lifetime-scoped address index. Different live placed buffers can
// alias a heap; do not guess the resource when a VA has multiple owners.
struct RayMemory {
    struct Allocation;
    struct Slice { std::shared_ptr<Allocation> allocation; UINT64 offset; };
    std::mutex lock;
    std::multimap<UINT64,std::weak_ptr<Allocation>> buffers;
    UINT64 largest_buffer{};
    void track(ID3D12Resource*,const BYTE* backing=nullptr);
    Slice locate(UINT64,UINT64);
    void copy(ID3D12Resource*,UINT64,ID3D12Resource*,UINT64,UINT64);
    bool read(UINT64,UINT64,std::vector<BYTE>&);
    ID3D12Resource* live(const Slice&);
};
using RayResource=Microsoft::WRL::ComPtr<ID3D12Resource>;
struct RayInstances {
    UINT64 tlas;
    UINT count;
    RayResource readback;
    UINT64 revision{};
    std::shared_ptr<RayRowBuffers> packed;
};
struct RaySceneLayout{
    std::shared_ptr<GpuCompletion> ready;
    UINT count{};
    bool all_active_opaque{true};
    // CHS-only complete shader tables can be recorded before this CPU
    // summary is ready. Still check opacity on this exact revision,
    // whichever of the producer and consumer reaches the check first.
    std::atomic<unsigned> opacity_check{};
    bool request_check(){return !(opacity_check.fetch_or(1)&2);}
    bool publish_check(){return all_active_opaque || !(opacity_check.fetch_or(2)&1);}
    std::vector<UINT> contributions; // unique hit-group bases of active instances
};
