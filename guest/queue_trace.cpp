#define NOMINMAX
#include "queue_trace.hpp"
#include <cwchar>

QueueTrace::QueueTrace(ID3D12Device* dev,UINT type){
    static std::atomic<UINT> uid{};
    wchar_t dir[MAX_PATH]{},path[MAX_PATH+64]{};
    auto n=GetEnvironmentVariableW(L"FROSTY_CAPTURE_DIR",dir,MAX_PATH);
    if(!n||n>=MAX_PATH||!GetEnvironmentVariableA("FROSTY_TRACE_FENCES",nullptr,0))return;
    if(FAILED(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence_))))return;
    quit_=CreateEventW(nullptr,TRUE,FALSE,nullptr);event_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(!quit_||!event_)return;
    std::swprintf(path,std::size(path),L"%ls\\queue-%02u-type-%u.tsv",dir,++uid,type);
    file_=_wfopen(path,L"wb");if(!file_)return;
    std::fputs("tick_ms\tevent\tobject_or_submit\tvalue\tcompleted\n",file_);
    worker_=std::thread([this]{
        HANDLE handles[]{quit_,event_};UINT64 last=0;
        for(;;){
            auto wait=WaitForMultipleObjects(2,handles,FALSE,10000);if(wait==WAIT_OBJECT_0 || wait==WAIT_FAILED)break;
            auto completed=fence_->GetCompletedValue(),now=next_.load();
            if(completed!=last || (wait==WAIT_TIMEOUT && completed<now))log(wait==WAIT_TIMEOUT?"pending":"retired",now,0,completed);
            last=completed;
        }
    });
}
QueueTrace::~QueueTrace(){
    if(quit_)SetEvent(quit_);
    if(worker_.joinable())worker_.join();
    if(file_)std::fclose(file_);
    if(quit_)CloseHandle(quit_);
    if(event_)CloseHandle(event_);
}
void QueueTrace::log(const char* op,UINT64 obj,UINT64 value,UINT64 completed){
    std::lock_guard g(log_lock_);std::fprintf(file_,"%llu\t%s\t%llu\t%llu\t%llu\n",GetTickCount64(),op,obj,value,completed);std::fflush(file_);
}
void QueueTrace::submitted(ID3D12CommandQueue* q){
    if(!file_)return;
    auto n=++next_;
    auto hr=q->Signal(fence_.Get(),n);
    if(SUCCEEDED(hr))hr=fence_->SetEventOnCompletion(n,event_);
    log(FAILED(hr)?"observer-error":"submit",n,UINT(hr),fence_->GetCompletedValue());
}
void QueueTrace::dependency(const char* op,ID3D12Fence* f,UINT64 val){
    if(file_ && f)log(op,reinterpret_cast<UINT_PTR>(f),val,f->GetCompletedValue());
}
