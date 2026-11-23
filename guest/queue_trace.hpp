#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>

// The observer never waits on the submission thread or signals a game fence.
class QueueTrace {
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    HANDLE quit_{},event_{};
    std::atomic<UINT64> next_{};
    FILE* file_{};
    std::mutex log_lock_;
    std::thread worker_;
    void log(const char*,UINT64,UINT64,UINT64);
public:
    QueueTrace(ID3D12Device*,UINT type);
    ~QueueTrace();
    void submitted(ID3D12CommandQueue*);
    void dependency(const char*,ID3D12Fence*,UINT64);
};
