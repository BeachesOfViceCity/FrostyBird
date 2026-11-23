#define NOMINMAX
#include "present.hpp"
#include "texture_upload.hpp"
#include "play_timing.hpp"
#include <bit>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace {
void message(const char* text){
    DWORD written{};WriteFile(GetStdHandle(STD_ERROR_HANDLE),text,DWORD(std::strlen(text)),&written,nullptr);
}
HRESULT failed(const char* operation,HRESULT hr){
    char text[180];std::snprintf(text,sizeof(text),"frosty/present: %s failed: %#lx\n",operation,(unsigned long)hr);
    message(text);return hr;
}
BOOL CALLBACK find_window(HWND hwnd,LPARAM dst){
    DWORD pid{};RECT area{};wchar_t cls[80]{};
    GetWindowThreadProcessId(hwnd,&pid);
    if(pid==GetCurrentProcessId() && !GetWindow(hwnd,GW_OWNER) &&
       GetClassNameW(hwnd,cls,80) && !std::wcscmp(cls,L"UnrealWindowGDK") &&
       GetClientRect(hwnd,&area) && area.right>=64 && area.bottom>=64){
        *reinterpret_cast<HWND*>(dst)=hwnd;return FALSE;
    }
    return TRUE;
}
DXGI_FORMAT swap_format(DXGI_FORMAT f){
    switch(f){
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UNORM:return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_UNORM:case DXGI_FORMAT_R16G16B16A16_FLOAT:return f;
    default:return DXGI_FORMAT_UNKNOWN;
    }
}
D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
    D3D12_RESOURCE_BARRIER v{};v.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};return v;
}
UINT64 counter(){LARGE_INTEGER t{};QueryPerformanceCounter(&t);return t.QuadPart;}
}

HRESULT Presentation::wait_for(UINT64 value){
    if(!value)return S_OK;
    auto done=fence_->GetCompletedValue();
    if(done==UINT64_MAX)return DXGI_ERROR_DEVICE_REMOVED;
    if(done>=value)return S_OK;
    return fence_->SetEventOnCompletion(value,nullptr);
}
HRESULT Presentation::init(ID3D12Device* dev,ID3D12CommandQueue* q,IDXGIAdapter* adapter,const D3D12_RESOURCE_DESC& src){
    HWND hwnd{};EnumWindows(find_window,reinterpret_cast<LPARAM>(&hwnd));
    if(!hwnd)return failed("find UnrealWindowGDK",DXGI_ERROR_NOT_FOUND);
    Ptr<IDXGIFactory2> factory;HRESULT hr=adapter->GetParent(IID_PPV_ARGS(&factory));
    if(FAILED(hr))return failed("get DXGI factory",hr);
    DXGI_SWAP_CHAIN_DESC1 sd{};sd.Width=UINT(src.Width);sd.Height=src.Height;sd.Format=swap_format(src.Format);
    sd.SampleDesc.Count=1;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=UINT(slots_.size());
    sd.Scaling=DXGI_SCALING_STRETCH;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;sd.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
    char text[220];std::snprintf(text,sizeof(text),"frosty/present: GPU swapchain hwnd=%p class=UnrealWindowGDK owner=%lu render=%lu, %ux%u format=%u buffers=%u\n",
        static_cast<void*>(hwnd),GetWindowThreadProcessId(hwnd,nullptr),GetCurrentThreadId(),sd.Width,sd.Height,sd.Format,sd.BufferCount);
    message(text);
    Ptr<IDXGISwapChain1> sc;
    hr=factory->CreateSwapChainForHwnd(q,hwnd,&sd,nullptr,nullptr,&sc);
    if(FAILED(hr))return failed("CreateSwapChainForHwnd",hr);
    if(FAILED(hr=sc.As(&swap_)))return failed("query swapchain3",hr);
    factory->MakeWindowAssociation(hwnd,DXGI_MWA_NO_WINDOW_CHANGES|DXGI_MWA_NO_ALT_ENTER);
    if(FAILED(hr=dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence_))))return failed("create frame fence",hr);
    for(auto& slot:slots_){
        if(FAILED(hr=dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&slot.alloc))) ||
           FAILED(hr=dev->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,slot.alloc.Get(),nullptr,IID_PPV_ARGS(&slot.list))) ||
           FAILED(hr=slot.list->Close()))return failed("create frame slot",hr);
    }
    width_=sd.Width;height_=sd.Height;format_=sd.Format;window_=hwnd;
    if(!IsWindowVisible(hwnd))ShowWindowAsync(hwnd,SW_SHOWNOACTIVATE);
    wchar_t path[1024]{};auto n=GetEnvironmentVariableW(L"FROSTY_FRAME_TIMES",path,1024);
    if(n && n<1024 && (times_=_wfopen(path,L"wb"))){
        LARGE_INTEGER hz{};QueryPerformanceFrequency(&hz);hz_=double(hz.QuadPart);
        std::fputs("present\tunix_us\tinterval_ms\tslot_wait_ms\tdxgi_present_ms\tlast_display_stat\tupload_wall_ms\tshader_request_wall_ms\tpso_wall_ms\tray_link_wall_ms\tdependency_wait_ms\torigin_wait_ms\tresource_alloc_wall_ms\tqueue_submit_wall_ms\tdirect_depth_planes\tstaged_depth_planes\tpacked_ray_rows\tfull_ray_rows\tstorage_hits_total\tstorage_bytes_total\tstorage_rejected_total\tray_packet_reuses\tray_packet_allocations\tupload_reserve_hits\tupload_reserve_misses\n",times_);
    }
    return S_OK;
}
Presentation::~Presentation(){
    if(fence_)wait_for(serial_);
    if(times_)std::fclose(times_);
}
HRESULT Presentation::present(ID3D12Device* dev,ID3D12CommandQueue* q,IDXGIAdapter* adapter,UINT n,const XsPlane* planes,
                              XboxTextures& textures,UploadConsumer& consumer){
    if(n!=1 || !planes || planes->count!=1 || !planes->resources || !planes->resources[0])return E_INVALIDARG;
    auto* src=planes->resources[0];auto desc=src->GetDesc();auto fmt=swap_format(desc.Format);
    if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.MipLevels!=1 || desc.DepthOrArraySize!=1 ||
       desc.SampleDesc.Count!=1 || fmt==DXGI_FORMAT_UNKNOWN)return DXGI_ERROR_UNSUPPORTED;
    std::lock_guard guard(lock_);HRESULT hr;
    if(!window_ && FAILED(hr=init(dev,q,adapter,desc)))return hr;
    if(width_!=desc.Width || height_!=desc.Height || format_!=fmt){
        if(FAILED(hr=wait_for(serial_)))return failed("drain for resize",hr);
        for(auto& s:slots_)s.source.Reset();
        hr=swap_->ResizeBuffers(UINT(slots_.size()),UINT(desc.Width),desc.Height,fmt,0);
        if(FAILED(hr))return failed("resize GPU swapchain",hr);
        width_=UINT(desc.Width);height_=desc.Height;format_=fmt;
    }
    const auto index=swap_->GetCurrentBackBufferIndex();
    if(index>=slots_.size())return E_UNEXPECTED;
    auto& s=slots_[index];
    auto start=times_?counter():0;
    if(FAILED(hr=wait_for(s.retire)))return failed("reuse frame slot",hr);
    auto ready=times_?counter():0;
    Ptr<ID3D12Resource> dest;
    if(FAILED(hr=swap_->GetBuffer(index,IID_PPV_ARGS(&dest))) || FAILED(hr=s.alloc->Reset()) ||
       FAILED(hr=s.list->Reset(s.alloc.Get(),nullptr)))return failed("reset GPU present list",hr);
    s.source=src; // the game can release its previous scanout as soon as PresentX returns
    D3D12_RESOURCE_BARRIER barriers[]{transition(src,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_SOURCE),
        transition(dest.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST)};
    s.list->ResourceBarrier(2,barriers);
    s.list->CopyResource(dest.Get(),src);
    for(auto& b:barriers)std::swap(b.Transition.StateBefore,b.Transition.StateAfter);
    s.list->ResourceBarrier(2,barriers);
    if(FAILED(hr=s.list->Close()))return failed("close GPU present list",hr);
    TextureReads reads;reads.add(textures.access(src));const TextureReads* usage[]{&reads};
    ID3D12CommandList* lists[]{s.list.Get()};
    if(FAILED(hr=textures.submit(dev,q,consumer,usage,1,lists)))return failed("submit GPU copy",hr);
    const auto value=serial_+1;
    if(FAILED(hr=q->Signal(fence_.Get(),value)))return failed("signal GPU copy",hr);
    serial_=s.retire=value;
    auto before=times_?counter():0;
    hr=swap_->Present(1,0);
    if(FAILED(hr))return failed("DXGI Present",hr);
    if(hr==S_OK){
        if(!first_){message("frosty/present: first GPU present to UnrealWindowGDK\n");first_=true;}
        if(times_){
            auto end=counter();FILETIME ft{};GetSystemTimePreciseAsFileTime(&ft);
            auto us=(((UINT64(ft.dwHighDateTime)<<32)|ft.dwLowDateTime)-116444736000000000ull)/10;
            bool sample=!last_ || (serial_%32)==0 || (end-flushed_)>=hz_;
            if(sample){DXGI_FRAME_STATISTICS stats{};if(SUCCEEDED(swap_->GetFrameStatistics(&stats)))displayed_=stats.PresentCount;}
            std::fprintf(times_,"%llu\t%llu\t%.3f\t%.3f\t%.3f\t%u",serial_,us,last_?1000.*(end-last_)/hz_:0.,
                         1000.*(ready-start)/hz_,1000.*(end-before)/hz_,displayed_);
            for(auto& t:play_timing::ticks)std::fprintf(times_,"\t%.3f",1000.*t.exchange(0,std::memory_order_relaxed)/hz_);
            using Stats=void(WINAPI*)(UINT64*,UINT64*,UINT64*);
            static auto storage=std::bit_cast<Stats>(GetProcAddress(GetModuleHandleW(L"xcrdapi.dll"),"FrostyStorageCacheStats"));
            UINT64 hits{},bytes{},reject{};if(storage)storage(&hits,&bytes,&reject);
            std::fprintf(times_,"\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\n",play_timing::direct_planes.exchange(0),play_timing::staged_planes.exchange(0),
                play_timing::packed_ray_rows.exchange(0),play_timing::full_ray_rows.exchange(0),hits,bytes,reject,
                play_timing::ray_packet_hits.exchange(0),play_timing::ray_packet_allocations.exchange(0),
                play_timing::upload_reserve_hits.exchange(0),play_timing::upload_reserve_misses.exchange(0));
            // A bounded benchmark shuts down the private prefix at its wall
            // deadline. Keep its trailing completed presents even in a long
            // stall rather than losing the last stdio buffer on termination.
            std::fflush(times_);if(sample)flushed_=end;
            last_=end;
        }
    }
    return hr;
}
