#define NOMINMAX
#include "gpu_retirement.hpp"
#include "play_timing.hpp"
#include <cstdio>

void GpuCompletion::complete(){
    {std::lock_guard g(mutex_);done_=true;}
    cv_.notify_all();
}
void GpuCompletion::wait(){
    std::unique_lock g(mutex_);if(done_)return;
    play_timing::Scope time(play_timing::dependencies);cv_.wait(g,[&]{return done_;});
}
HRESULT GpuRetirement::enqueue(ID3D12Device* dev,ID3D12CommandQueue* queue,std::shared_ptr<GpuCompletion> ticket,std::function<void()> fn){
    HRESULT hr;
    std::unique_lock g(mutex_);
    if(!worker_.joinable()){
        if(FAILED(hr=dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence_))))return hr;
        worker_=std::thread([this]{work();});
    }
    cv_.wait(g,[&]{return jobs_.size()<32 || closing_;});
    if(closing_)return E_ABORT;
    if(FAILED(hr=queue->Signal(fence_.Get(),++serial_)))return hr;
    jobs_.push_back({serial_,std::move(ticket),std::move(fn)});g.unlock();cv_.notify_all();return S_OK;
}
void GpuRetirement::work(){
    for(;;){
        Job job;
        {std::unique_lock g(mutex_);cv_.wait(g,[&]{return closing_ || !jobs_.empty();});
            if(jobs_.empty())return;
            job=std::move(jobs_.front());jobs_.pop_front();}
        cv_.notify_all();
        auto v=fence_->GetCompletedValue();
        if(v==UINT64_MAX || (v<job.value && FAILED(fence_->SetEventOnCompletion(job.value,nullptr))) ||
           fence_->GetCompletedValue()==UINT64_MAX){
            std::fputs("frosty/queue: GPU retirement fence failed\n",stderr);ExitProcess(ERROR_DEVICE_NOT_AVAILABLE);
        }
        try{job.finish();job.finish={};job.ticket->complete();}
        catch(const std::bad_alloc&){ExitProcess(ERROR_OUTOFMEMORY);}
    }
}
void GpuRetirement::drain(){
    {std::lock_guard g(mutex_);closing_=true;}
    cv_.notify_all();if(worker_.joinable())worker_.join();
}
