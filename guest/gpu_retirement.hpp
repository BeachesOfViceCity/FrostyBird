#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

class GpuCompletion {
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_{};
public:
    void complete();
    void wait();
};

// One bounded retirement stream per native queue. The worker owns the
// submission's references through GPU completion and validates ray status.
class GpuRetirement {
    struct Job {UINT64 value;std::shared_ptr<GpuCompletion> ticket;std::function<void()> finish;};
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    UINT64 serial_{};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::thread worker_;
    bool closing_{};
    void work();
public:
    HRESULT enqueue(ID3D12Device*,ID3D12CommandQueue*,std::shared_ptr<GpuCompletion>,std::function<void()>);
    void drain();
    ~GpuRetirement(){drain();}
};
