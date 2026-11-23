// Frosty's root device ABI. Only forward methods whose slot and signature
// agree with the Series caller. DXGI identity must refer to the same adapter
// as the underlying Vulkan device.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include "present.hpp"
#include "texture_upload.hpp"
#include "recompiled_shader.hpp"
#include "pipeline_prep.hpp"
#include "depth_alias.hpp"
#include "video_surface.hpp"
#include "work_trace.hpp"
#include "pass_capture.hpp"
#include "pass_times.hpp"
#include "queue_trace.hpp"
#include "play_timing.hpp"
#include "striped_views.hpp"
#include "upload_reserve.hpp"
#include "xs_ray.hpp"
#include "rt_state.hpp"
#include "ray_memory.hpp"
#include "ray_dispatch.hpp"
#include "../src/view_fields.hpp"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>
#include <new>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>
#include <cstdint>

namespace {
using Microsoft::WRL::ComPtr;
using Slot = void (*)();
template<class F> Slot slot(F fn) { return std::bit_cast<Slot>(fn); }
template<size_t N,int Object=0> [[noreturn]] void WINAPI missing() {
    char message[192];
    auto caller=reinterpret_cast<UINT_PTR>(__builtin_return_address(0));
    auto base=reinterpret_cast<UINT_PTR>(GetModuleHandleW(nullptr));
    std::snprintf(message,sizeof(message),"frosty/d3d12_xs: unimplemented %s slot %zu (offset %#zx), return RVA %#llx\n",
                  Object==2 ? "list" : Object==1 ? "queue" : "device",N, N*8, static_cast<unsigned long long>(caller-base));
    OutputDebugStringA(message);
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE),message,DWORD(std::strlen(message)),&written,nullptr);
    ExitProcess(ERROR_NOT_SUPPORTED);
}
template<int Object=0,size_t... I> auto empty_table(std::index_sequence<I...>) {
    return std::array<Slot,sizeof...(I)>{slot(&missing<I,Object>)...};
}
struct Root;
struct Dxgi final : IDXGIDevice1 {
    Root* root;
    ComPtr<IDXGIAdapter> adapter;
    INT priority{};
    UINT latency{3};
    explicit Dxgi(Root* r) : root(r) {}
    HRESULT WINAPI QueryInterface(REFIID, void**) override;
    ULONG WINAPI AddRef() override;
    ULONG WINAPI Release() override;
    HRESULT WINAPI SetPrivateData(REFGUID,UINT,const void*) override { return E_NOTIMPL; }
    HRESULT WINAPI SetPrivateDataInterface(REFGUID,const IUnknown*) override { return E_NOTIMPL; }
    HRESULT WINAPI GetPrivateData(REFGUID,UINT*,void*) override { return DXGI_ERROR_NOT_FOUND; }
    HRESULT WINAPI GetParent(REFIID id, void** out) override { return adapter->QueryInterface(id,out); }
    HRESULT WINAPI GetAdapter(IDXGIAdapter** out) override { return adapter.CopyTo(out); }
    HRESULT WINAPI CreateSurface(const DXGI_SURFACE_DESC*,UINT,DXGI_USAGE,const DXGI_SHARED_RESOURCE*,IDXGISurface**) override { return DXGI_ERROR_UNSUPPORTED; }
    HRESULT WINAPI QueryResourceResidency(IUnknown* const*,DXGI_RESIDENCY*,UINT) override { return DXGI_ERROR_UNSUPPORTED; }
    HRESULT WINAPI SetGPUThreadPriority(INT v) override { if (v < -7 || v > 7) return E_INVALIDARG; priority = v; return S_OK; }
    HRESULT WINAPI GetGPUThreadPriority(INT* v) override { if (!v) return E_POINTER; *v=priority; return S_OK; }
    HRESULT WINAPI SetMaximumFrameLatency(UINT v) override { if (v>16) return E_INVALIDARG; latency=v?v:3; return S_OK; }
    HRESULT WINAPI GetMaximumFrameLatency(UINT* v) override { if (!v) return E_POINTER; *v=latency; return S_OK; }
};
struct ViewFields {
    UINT64 generation{};
    DXGI_FORMAT format{};
    UINT dimension{};
    bool image{};
    UINT width{},height{},base{};
    float min_lod{};
    UINT64 buffer_va{};
    UINT buffer_stride{},buffer_elements{};
    bool buffer_valid{};
};
struct BoundView : ViewFields {
    std::shared_ptr<TextureUse> upload;
    // This path is used only when both slots have no upload reference.
    void copy_settled(const BoundView& v){static_cast<ViewFields&>(*this)=v;}
};
void buffer_view(BoundView& v,ID3D12Resource* res,UINT64 first,UINT count,UINT stride,bool raw){
    if(!res)return;
    UINT element=stride;
    if(raw){if(count>UINT_MAX/4)return;element=4;count*=4;}
    else if(!element){
        // For a typed buffer only the base-address field is exported to the
        // scalar ABI. Get the actual element width before applying FirstElement.
        auto fmt=UINT(v.format);
        if(fmt>=1 && fmt<=4)element=16;
        else if(fmt>=5 && fmt<=8)element=12;
        else if(fmt>=9 && fmt<=18)element=8;
        else if((fmt>=23 && fmt<=43) || fmt==67)element=4;
        else if(fmt>=48 && fmt<=59)element=2;
        else if(fmt>=60 && fmt<=65)element=1;
        else return;
    }
    if(!element || first>UINT64_MAX/element)return;
    auto va=res->GetGPUVirtualAddress();if(!va || va>UINT64_MAX-first*element)return;
    v.buffer_va=va+first*element;v.buffer_stride=stride;v.buffer_elements=count;v.buffer_valid=true;
}
struct Root {
    const Slot* vtbl;
    std::atomic<ULONG> refs{1};
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12Device5> ray_device;
    ComPtr<ID3D12Device7> ray_device7;
    bool position_fetch{};
    Dxgi dxgi{this};
    std::mutex pacing;
    UINT interval_us{16667}, origin_offset{};
    ULONGLONG frame_token{}, next_origin{};
    HANDLE flip_signal{};
    XboxTextures textures;
    UploadReserve upload_reserve;
    PipelinePrep prep;
    RayMemory memory;
    RayDispatch ray_dispatch;
    RayRowPacker ray_rows;
    bool demand_rays=GetEnvironmentVariableA("FROSTY_RT_DEMAND_PATH",nullptr,0)!=0;
    bool dump_ray_tables=GetEnvironmentVariableA("FROSTY_TRACE_SBT",nullptr,0)!=0 && GetEnvironmentVariableA("FROSTY_CAPTURE_DIR",nullptr,0)!=0;
    bool ray_tables=demand_rays || dump_ray_tables;
    bool log_work=GetEnvironmentVariableA("FROSTY_TRACE_WORK",nullptr,0)!=0;
    std::mutex geometry_lock;
    std::map<UINT64,UINT> geometry_count;
    struct SceneEntry{UINT64 revision;std::shared_ptr<RaySceneLayout> layout;};
    std::map<UINT64,SceneEntry> ray_scenes;
    UINT64 scene_revision{};
    WorkTrace trace;
    PassCapture captures;
    struct DescriptorLock {
        SRWLOCK native=SRWLOCK_INIT;
        void lock(){AcquireSRWLockExclusive(&native);}
        void unlock(){ReleaseSRWLockExclusive(&native);}
        void lock_shared(){AcquireSRWLockShared(&native);}
        void unlock_shared(){ReleaseSRWLockShared(&native);}
    } sampler_lock;
    StripedViews<BoundView,DescriptorLock> view_formats;
    std::atomic<UINT64> view_generation{};
    std::atomic<UINT64> upload_revision{};
    std::unordered_map<SIZE_T,D3D12_SAMPLER_DESC> sampler_desc;
    void set_view(SIZE_T addr,BoundView* view){
        if(view)view->generation=view_generation.fetch_add(1,std::memory_order_relaxed)+1;
        auto hold=view_formats.guard(0,view_formats.mask(addr));
        if(view_formats.write(addr,view))++upload_revision;
    }
    explicit Root(const Slot* t) : vtbl(t) {}
    ~Root() { if (flip_signal) CloseHandle(flip_signal); }
};
ULONG WINAPI add(Root* r) { return ++r->refs; }
ULONG WINAPI release(Root* r) { auto n=--r->refs; if (!n) delete r; return n; }
HRESULT WINAPI query(Root* r, REFIID id, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (id == __uuidof(IUnknown) || id == __uuidof(ID3D12Device) || id == __uuidof(ID3D12Device1) ||
        id == __uuidof(ID3D12Device2) || id == __uuidof(ID3D12Object)) *out = r;
    else if(r->ray_device && (id==__uuidof(ID3D12Device3) || id==__uuidof(ID3D12Device4) || id==__uuidof(ID3D12Device5) || id==xs_serialized_device)) *out=r;
    else if(r->ray_device7 && (id==__uuidof(ID3D12Device6) || id==__uuidof(ID3D12Device7))) *out=r;
    else if (id == __uuidof(IDXGIDevice) || id == __uuidof(IDXGIDevice1) || id == __uuidof(IDXGIObject)) *out = &r->dxgi;
    else return E_NOINTERFACE;
    add(r);
    return S_OK;
}
HRESULT Dxgi::QueryInterface(REFIID id, void** out) { return query(root,id,out); }
ULONG Dxgi::AddRef() { return add(root); }
ULONG Dxgi::Release() { return release(root); }

HRESULT WINAPI features(Root* r,D3D12_FEATURE f,void* data,UINT size) {
    if (f==D3D12_FEATURE_D3D12_OPTIONS5 && data && size==sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5)) {
        auto hr=r->dev->CheckFeatureSupport(f,data,size);
        if (SUCCEEDED(hr) && !r->ray_device)static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS5*>(data)->RaytracingTier=D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
        return hr;
    }
    if (f == D3D12_FEATURE_D3D12_OPTIONS && size == 64 && data) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS caps{};
        auto hr = r->dev->CheckFeatureSupport(f,&caps,sizeof(caps));
        if (FAILED(hr)) return hr;
        // Frosty's 210604 layout reads binding tier at +0x10, heap tier at
        // +0x3c (desktop: +0x38). Leave unknown optional capabilities zero.
        std::memset(data,0,size);
        auto d = static_cast<UINT*>(data);
        d[4] = caps.ResourceBindingTier;
        d[15] = caps.ResourceHeapTier;
        return S_OK;
    }
    return r->dev->CheckFeatureSupport(f,data,size);
}
// The XS caller reads both descriptor handle returns directly from RAX.
// Desktop COM uses a hidden result pointer even for these 8-byte structs.
struct Heap {
    const Slot* vtbl;
    ComPtr<ID3D12DescriptorHeap> host;
    Root* parent;
    std::atomic<ULONG> refs{1};
};
HRESULT WINAPI heap_query(Heap* h,REFIID id,void** out) {
    if (!out) return E_POINTER;
    *out=nullptr;
    if(id!=__uuidof(IUnknown) && id!=__uuidof(ID3D12Object) && id!=__uuidof(ID3D12DeviceChild) &&
       id!=__uuidof(ID3D12Pageable) && id!=__uuidof(ID3D12DescriptorHeap)) return E_NOINTERFACE;
    ++h->refs;
    *out=h;
    return S_OK;
}
const std::array heap_slots{
    slot(&heap_query),slot(+[](Heap* h)->ULONG {return ++h->refs;}),
    slot(+[](Heap* h)->ULONG {
        auto n=--h->refs;
        if(!n){
            auto* r=h->parent;auto desc=h->host->GetDesc();
            if(desc.Type==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV){
                auto first=h->host->GetCPUDescriptorHandleForHeapStart().ptr;
                auto guard=r->view_formats.guard(0,r->view_formats.mask(first,desc.NumDescriptors));
                if(r->view_formats.erase(first,desc.NumDescriptors))++r->upload_revision;
            }else if(desc.Type==D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER){
                auto first=h->host->GetCPUDescriptorHandleForHeapStart().ptr;
                auto span=SIZE_T(desc.NumDescriptors)*r->dev->GetDescriptorHandleIncrementSize(desc.Type);
                std::lock_guard guard(r->sampler_lock);
                std::erase_if(r->sampler_desc,[&](const auto& it){return it.first>=first && it.first-first<span;});
            }
            release(r);delete h;
        }
        return n;}),
    slot(+[](Heap* h,REFGUID g,UINT* n,void* d){return h->host->GetPrivateData(g,n,d);}),
    slot(+[](Heap* h,REFGUID g,UINT n,const void* d){return h->host->SetPrivateData(g,n,d);}),
    slot(+[](Heap* h,REFGUID g,const IUnknown* o){return h->host->SetPrivateDataInterface(g,o);}),
    slot(+[](Heap* h,LPCWSTR n){return h->host->SetName(n);}),
    slot(+[](Heap* h,REFIID id,void** o){return query(h->parent,id,o);}),
    slot(+[](Heap* h,D3D12_DESCRIPTOR_HEAP_DESC* o){*o=h->host->GetDesc();return o;}),
    slot(+[](Heap* h){return h->host->GetCPUDescriptorHandleForHeapStart().ptr;}),
    slot(+[](Heap* h){return h->host->GetGPUDescriptorHandleForHeapStart().ptr;})
};
HRESULT WINAPI make_heap(Root* r,const D3D12_DESCRIPTOR_HEAP_DESC* d,REFIID id,void** out) {
    if (!out) return E_POINTER;
    *out=nullptr;
    if (id!=__uuidof(ID3D12DescriptorHeap)) return E_NOINTERFACE;
    ComPtr<ID3D12DescriptorHeap> host;
    auto hr=r->dev->CreateDescriptorHeap(d,IID_PPV_ARGS(&host));
    if (FAILED(hr)) return hr;
    auto h=new(std::nothrow) Heap{heap_slots.data(),std::move(host),r};
    if (!h) return E_OUTOFMEMORY;
    add(r);
    *out=h;
    return S_OK;
}
constexpr GUID indirect_kind_key{0xba92dd1c,0x615c,0x491f,{0x9b,0xb2,0xbb,0xc8,0x96,0x62,0x22,0xf0}};
constexpr GUID indirect_pure_key{0xb4bf4e8a,0x889c,0x4729,{0xa6,0xa1,0x77,0xb8,0x15,0x02,0xa8,0x21}};
HRESULT WINAPI command_signature(Root* r,const D3D12_COMMAND_SIGNATURE_DESC* in,
                                ID3D12RootSignature* rs,REFIID iid,void** out) {
    if (!out) return E_POINTER;
    *out=nullptr;
    if (!in || !in->pArgumentDescs || !in->NumArgumentDescs || in->NumArgumentDescs>128) return E_INVALIDARG;
    try {
        auto desc=*in;
        std::vector<D3D12_INDIRECT_ARGUMENT_DESC> args(in->pArgumentDescs,in->pArgumentDescs+in->NumArgumentDescs);
        for (auto& a:args) {
            switch (static_cast<UINT>(a.Type)) {
            case 0x69: a.Type=D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH; break;
            case 0x6b: a.Type=D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH; break;
            default: break;
            }
        }
        desc.pArgumentDescs=args.data();
        ComPtr<ID3D12CommandSignature> sig;
        // The XS indirect-rays signature includes the global ray root even
        // when its only argument is DISPATCH_RAYS. On desktop that record
        // changes no root argument, and CreateCommandSignature requires NULL.
        bool writes_root=std::ranges::any_of(args,[](const auto& a){
            return a.Type==D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT || a.Type==D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW ||
                   a.Type==D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW || a.Type==D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW;
        });
        auto hr=r->dev->CreateCommandSignature(&desc,writes_root?rs:nullptr,IID_PPV_ARGS(&sig));
        if(FAILED(hr))return hr;
        auto kind=args.back().Type;
        if(FAILED(hr=sig->SetPrivateData(indirect_kind_key,sizeof(kind),&kind)))return hr;
        if(args.size()==1)sig->SetPrivateData(indirect_pure_key,sizeof(kind),&kind);
        return sig->QueryInterface(iid,out);
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}
struct FrameObjects { UINT count; HANDLE* handles; };
HRESULT WINAPI set_interval(Root* r,IDXGIOutput* output,UINT us,UINT,UINT flags) {
    if (!output || !us || us>1000000 || flags) return E_INVALIDARG;
    std::lock_guard guard(r->pacing);
    if(GetEnvironmentVariableA("FROSTY_FRAME_TIMES",nullptr,0) && r->interval_us!=us)
        std::fprintf(stderr,"frosty/pacing: game requested %u us per origin\n",us);
    if(r->interval_us!=us)r->next_origin=0;
    r->interval_us=us;
    return S_OK;
}
HRESULT WINAPI schedule(Root* r,UINT type,UINT offset,const FrameObjects* signals,UINT flags) {
    if (flags) return E_INVALIDARG;
    std::lock_guard guard(r->pacing);
    if (!type && !signals) { if(r->origin_offset!=offset)r->next_origin=0;r->origin_offset=offset; return S_OK; }
    if (type!=2 || !signals || signals->count!=1 || !signals->handles) return E_NOTIMPL;
    HANDLE h{};
    if (!DuplicateHandle(GetCurrentProcess(),signals->handles[0],GetCurrentProcess(),&h,0,FALSE,DUPLICATE_SAME_ACCESS))
        return HRESULT_FROM_WIN32(GetLastError());
    if (r->flip_signal) CloseHandle(r->flip_signal);
    r->flip_signal=h;
    return S_OK;
}
HRESULT WINAPI wait_origin(Root* r,UINT type,UINT timeout,void* waits,UINT flags,UINT64* token) {
    if (type || waits || (flags & ~2u) || !token) return E_NOTIMPL;
    static const UINT64 hz=[](){LARGE_INTEGER f{};QueryPerformanceFrequency(&f);return UINT64(f.QuadPart);}();
    auto now_ticks=[](){LARGE_INTEGER t{};QueryPerformanceCounter(&t);return UINT64(t.QuadPart);};
    UINT64 deadline{};
    {
        std::lock_guard guard(r->pacing);
        auto now=now_ticks();
        auto period=std::max<UINT64>(1,(hz*r->interval_us+500000)/1000000);
        deadline=r->next_origin?r->next_origin:now+(hz*r->origin_offset+500000)/1000000;
        // A missed origin is already due. Consume the last elapsed one
        // once; do not tack the offset onto every late frame or drift by
        // rounding a 33,333 us interval up to 34 ms.
        if(deadline<now)deadline+=(now-deadline)/period*period;
        if(deadline>now && timeout!=INFINITE && (deadline-now)*1000>UINT64(timeout)*hz)return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        r->next_origin=deadline+period;
        *token=++r->frame_token;
    }
    auto now=now_ticks();
    if(now<deadline){
        play_timing::Scope time(play_timing::origins);
        struct Timer{HANDLE h=CreateWaitableTimerExW(nullptr,nullptr,0x2,TIMER_MODIFY_STATE|SYNCHRONIZE);
            Timer(){if(!h)h=CreateWaitableTimerW(nullptr,FALSE,nullptr);}~Timer(){if(h)CloseHandle(h);}};
        static thread_local Timer timer;
        auto delay=deadline-now;LARGE_INTEGER due{};
        due.QuadPart=-LONGLONG((delay/hz)*10000000+(delay%hz*10000000+hz-1)/hz);
        if(timer.h && SetWaitableTimer(timer.h,&due,0,nullptr,nullptr,FALSE)){
            if(WaitForSingleObject(timer.h,INFINITE)==WAIT_FAILED)return HRESULT_FROM_WIN32(GetLastError());
        }else Sleep(DWORD((delay*1000+hz-1)/hz));
    }
    return S_OK;
}
struct Queue {
    const Slot* vtbl;
    UINT reserved[2]{};
    UINT type; // FXboxCommon reads queue->type at +0x10 before SignalX.
    std::atomic<ULONG> refs{1};
    ComPtr<ID3D12CommandQueue> host;
    Root* parent;
    Presentation display;
    QueueTrace progress;
    GpuRetirement retirement;
    std::mutex submission;
    UploadConsumer upload;
    Queue(const Slot* v,UINT t,ComPtr<ID3D12CommandQueue> q,Root* r) : vtbl(v),type(t),host(std::move(q)),parent(r),progress(r->dev.Get(),t) { add(r); }
    ~Queue() { retirement.drain();release(parent); }
};
struct List {
    const Slot* vtbl;
    // XSX callers cache shader state at +0x540 and inline command writes
    // when current (+0x18) < packet limit (+0x28/+0x30). A zero-sized
    // command arena selects the SDK's virtual recording functions instead.
    // Keep guest-written state physically separate from host pointers.
    alignas(8) std::array<BYTE,0x8000-sizeof(void*)> xs_state{};
    ComPtr<ID3D12GraphicsCommandList> host;
    ComPtr<ID3D12GraphicsCommandList4> ray_list;
    ComPtr<ID3D12StateObject> pending_rt;
    ComPtr<ID3D12GraphicsCommandList5> rate_list;
    Root* parent;
    std::atomic<ULONG> refs{1};
    ComPtr<ID3D12PipelineState> typed;
    TypedViews requests;
    std::shared_ptr<const TypedViews> prepared_views;
    std::vector<BYTE> type_scratch,last_types;
    ComPtr<ID3D12PipelineState> bound;
    ComPtr<ID3D12RootSignature> cs_root,graphics_root;
    std::shared_ptr<const RootViews> cs_layout,graphics_layout;
    ComPtr<ID3D12DescriptorHeap> views;
    ComPtr<ID3D12DescriptorHeap> samplers;
    struct Window{UINT64 gpu{},span{};SIZE_T cpu{};UINT stride{},count{};} view_window,sampler_window;
    std::vector<ComPtr<ID3D12Resource>> media_copies;
    TextureReads texture_reads;
    struct RangeCheck{UINT64 revision{};bool retry{};};
    std::map<std::pair<SIZE_T,SIZE_T>,RangeCheck> upload_ranges;
    UINT64 checked_upload_revision{};
    std::vector<RayInstances> instances;
    std::vector<RayExecution> ray_exec;
    std::shared_ptr<GpuCompletion> last_execution;
    ShaderKeys shader_keys{};
    std::vector<RecordedWork> work;
    std::vector<PassReadback> pass_readbacks;
    [[no_unique_address]] PassTimes pass_times;
    std::array<UINT64,64> tables{},graphics_tables{};
    List(ComPtr<ID3D12GraphicsCommandList> h,Root* r,const Slot* t) : vtbl(t),host(std::move(h)),parent(r) {
        if(r->ray_device){host.As(&ray_list);host.As(&rate_list);}
    }
};
struct Pipeline {
    const Slot* vtbl;
    alignas(8) std::array<BYTE,0x800-sizeof(void*)> xs_state{};
    std::atomic<ULONG> refs{1};
    ComPtr<ID3D12PipelineState> host;
    Root* parent;
    ShaderKeys keys;
    std::shared_ptr<const TypedViews> views;
    bool views_known=true;
    Pipeline(const Slot* t,ID3D12PipelineState* p,Root* r,ShaderKeys k):vtbl(t),host(p),parent(r),keys(k){
        try{TypedViews v;if(get_typed_views(p,v))views=std::make_shared<TypedViews>(std::move(v));}
        catch(const std::bad_alloc&){views_known=false;}add(r);}
    ~Pipeline(){release(parent);}
};
const std::array pipeline_slots{
    slot(+[](Pipeline* p,REFIID id,void** out)->HRESULT {
        if(!out) return E_POINTER;
        *out=nullptr;
        if(id!=__uuidof(IUnknown) && id!=__uuidof(ID3D12PipelineState) && id!=__uuidof(ID3D12Pageable) &&
           id!=__uuidof(ID3D12DeviceChild) && id!=__uuidof(ID3D12Object)) return E_NOINTERFACE;
        *out=p;++p->refs;return S_OK;}),
    slot(+[](Pipeline* p)->ULONG{return ++p->refs;}),
    slot(+[](Pipeline* p)->ULONG{auto n=--p->refs;if(!n)delete p;return n;}),
    slot(+[](Pipeline* p,REFGUID g,UINT* n,void* d){return p->host->GetPrivateData(g,n,d);}),
    slot(+[](Pipeline* p,REFGUID g,UINT n,const void* d){return p->host->SetPrivateData(g,n,d);}),
    slot(+[](Pipeline* p,REFGUID g,const IUnknown* o){return p->host->SetPrivateDataInterface(g,o);}),
    slot(+[](Pipeline* p,LPCWSTR n){return p->host->SetName(n);}),
    slot(+[](Pipeline* p,REFIID id,void** o){return query(p->parent,id,o);}),
    slot(+[](Pipeline* p,ID3DBlob** blob){return p->host->GetCachedBlob(blob);})
};
ID3D12PipelineState* unwrap(ID3D12PipelineState* p) {
    auto* xs=reinterpret_cast<Pipeline*>(p);
    return xs && xs->vtbl==pipeline_slots.data() ? xs->host.Get() : p;
}
ShaderKeys work_keys(ID3D12PipelineState* p){
    auto* xs=reinterpret_cast<Pipeline*>(p);return xs && xs->vtbl==pipeline_slots.data()?xs->keys:ShaderKeys{};
}
void pipeline_metadata(List* l,ID3D12PipelineState* source){
    auto xs=reinterpret_cast<Pipeline*>(source);bool wrapped=xs && xs->vtbl==pipeline_slots.data() && xs->views_known;
    auto p=unwrap(source);l->prepared_views=wrapped?xs->views:nullptr;
    l->bound=p;l->last_types.clear();
    if(wrapped)l->typed=l->prepared_views?p:nullptr;
    else l->typed=get_typed_views(p,l->requests)?p:nullptr;
}
void WINAPI set_pipeline(List* l,ID3D12PipelineState* pso) {
    l->pending_rt.Reset();
    l->shader_keys=work_keys(pso);
    auto host=unwrap(pso);
    if(host && ((l->typed.Get()==host && l->bound) || (!l->typed && l->bound.Get()==host)))return;
    pipeline_metadata(l,pso);
    l->host->SetPipelineState(host);
}
void bind_variant(List* l,bool compute) {
    if (l->typed) {
        auto r=l->parent;
        auto* root=compute?l->cs_layout.get():l->graphics_layout.get();
        auto& tables=compute?l->tables:l->graphics_tables;
        auto number=[&](ViewStage stage,UINT request) -> UINT {
            auto reg=request&0x17fu;
            ViewBinding binding{};
            if(request&frosty::view::sampler){
                D3D12_SAMPLER_DESC s{};
                bool found=static_sampler(root,stage,reg,s);
                if(!found && l->samplers && find_view_binding(root,stage,reg|0x300u,binding) && binding.table<tables.size()){
                    auto& d=l->sampler_window;auto stride=d.stride;auto start=d.gpu;
                    auto addr=tables[binding.table]+UINT64(binding.offset)*stride;
                    if(addr>=start && addr-start<d.span){
                        auto cpu=d.cpu+addr-start;
                        if(auto it=r->sampler_desc.find(cpu);it!=r->sampler_desc.end()){s=it->second;found=true;}
                    }
                }
                if(!found){OutputDebugStringA("frosty/binding: explicit gather missing sampler\n");ExitProcess(ERROR_NOT_SUPPORTED);}
                UINT field=(request&~frosty::view::sampler)>>frosty::view::field_shift;
                if(field==frosty::view::sampler_addr)return (UINT(s.AddressU)-1u)|((UINT(s.AddressV)-1u)<<3u);
                float v=field<frosty::view::sampler_max?s.MinLOD:field<frosty::view::sampler_bias?s.MaxLOD:
                        field<frosty::view::sampler_border?s.MipLODBias:s.BorderColor[(field-frosty::view::sampler_border)/4];
                return (std::bit_cast<UINT>(v)>>(8*((field-1)%4)))&255u;
            }
            if(find_view_binding(root,stage,reg,binding) && l->views && binding.table<tables.size()) {
                auto start=l->view_window.gpu;auto stride=l->view_window.stride;
                auto gpu=tables[binding.table]+UINT64(binding.offset)*stride;
                if(gpu>=start && gpu-start<l->view_window.span) {
                    auto cpu=l->view_window.cpu+(gpu-start);
                      if(auto slot=r->view_formats.find(cpu)){
                           const auto& v=*slot;
                         if(request&frosty::view::image){
                             if(v.image)switch(request>>frosty::view::field_shift){
                                 case frosty::view::width_lo:return v.width & 255u;
                                 case frosty::view::width_hi:return v.width >> 8;
                                 case frosty::view::height_lo:return v.height & 255u;
                                 case frosty::view::height_hi:return v.height >> 8;
                                  case frosty::view::base_mip:return v.base;
                                  case frosty::view::min_lod0:case frosty::view::min_lod1:case frosty::view::min_lod2:case frosty::view::min_lod3:
                                      return (std::bit_cast<UINT>(v.min_lod)>>(8*((request>>frosty::view::field_shift)-frosty::view::min_lod0)))&255u;
                             }
                          }else if(request&frosty::view::atomic64){
                              if(v.format==DXGI_FORMAT_R32G32_UINT)return UINT(Numeric::unsigned_int);
                              std::fprintf(stderr,"frosty/binding: 64-bit image atomic at u%u requires R32G32_UINT, got %u\n",reg&127u,UINT(v.format));
                              ExitProcess(ERROR_NOT_SUPPORTED);
                          }else if(v.format!=DXGI_FORMAT_UNKNOWN)return UINT(numeric_format(v.format));
                     }
                }
            }
            char buf[256];
            std::snprintf(buf,sizeof(buf),"frosty/binding: missing %s%u: root=%p parsed=%d slot=%u+%u table=%#llx heap=%p gpu-start=%#llx\n",
                compute?"CS view ":stage==ViewStage::vertex?"VS view ":"PS view ",reg,static_cast<const void*>(root),int(find_view_binding(root,stage,reg,binding)),binding.table,binding.offset,
                binding.table<64?tables[binding.table]:0,static_cast<void*>(l->views.Get()),
                l->views?l->views->GetGPUDescriptorHandleForHeapStart().ptr:0);
            OutputDebugStringA(buf);
            DWORD written;WriteFile(GetStdHandle(STD_ERROR_HANDLE),buf,DWORD(std::strlen(buf)),&written,nullptr);
            ExitProcess(ERROR_NOT_SUPPORTED);
        };
        const auto& requests=l->prepared_views?*l->prepared_views:l->requests;
        auto& types=l->type_scratch;types.clear();types.reserve(requests.size());
        UINT32 banks=0;bool samplers=false;
        for(auto b:requests){
            if(b.slot&frosty::view::sampler){samplers=true;continue;}
            if((b.slot&frosty::view::table_word) && !(b.slot&frosty::view::buffer_word))continue;
            ViewBinding bind{};if(!l->views || !find_view_binding(root,b.stage,b.slot&0x17fu,bind) || bind.table>=tables.size())continue;
            auto& w=l->view_window;auto gpu=tables[bind.table]+UINT64(bind.offset)*w.stride;
            if(gpu>=w.gpu && gpu-w.gpu<w.span)banks|=r->view_formats.mask(w.cpu+gpu-w.gpu);
        }
        std::shared_lock sampler_guard(r->sampler_lock,std::defer_lock);if(samplers)sampler_guard.lock();
        auto read_views=r->view_formats.guard(banks,0);
        for(auto b:requests){
            if((b.stage==ViewStage::compute)!=compute)ExitProcess(ERROR_INVALID_DATA);
            if(b.slot&frosty::view::buffer_word){
                UINT param;ViewBinding bind{};auto reg=b.slot&0x17fu;
                if(!compute||!l->views||!table_constant_parameter(root,param)||!find_view_binding(root,b.stage,reg,bind)||bind.table>=tables.size())ExitProcess(ERROR_INVALID_DATA);
                auto start=l->view_window.gpu;auto stride=l->view_window.stride;
                auto addr=tables[bind.table]+UINT64(bind.offset)*stride;
                if(addr<start || addr-start>=l->view_window.span)ExitProcess(ERROR_INVALID_DATA);
                auto cpu=l->view_window.cpu+addr-start;UINT value{};
                {
                     auto slot=r->view_formats.find(cpu);
                     if(!slot || !slot->buffer_valid || slot->buffer_stride>=16384){
                        OutputDebugStringA("frosty/binding: missing buffer SRD for scalar word\n");ExitProcess(ERROR_INVALID_DATA);
                    }
                     auto& bv=*slot;auto part=(b.slot>>17)&3u;
                    value=part==0?UINT(bv.buffer_va):part==1?UINT((bv.buffer_va>>32)&65535u)|(bv.buffer_stride<<16):bv.buffer_elements;
                }
                auto slot=(b.slot>>frosty::view::field_shift)&(frosty::view::table_count-1);
                l->host->SetComputeRoot32BitConstant(param,value,slot);types.push_back(0);continue;
            }
            if(b.slot&frosty::view::table_word){
                UINT abi;ViewBinding bind{};UINT key=b.slot&0x37fu;
                if(!compute || !table_constant_parameter(root,abi) || !find_view_binding(root,b.stage,key,bind) ||
                   bind.table>=tables.size()){
                    OutputDebugStringA("frosty/binding: no raw user-SGPR table mapping\n");ExitProcess(ERROR_INVALID_DATA);
                }
                auto slot=(b.slot & ~frosty::view::table_word)>>frosty::view::field_shift;
                l->host->SetComputeRoot32BitConstant(abi,UINT(tables[bind.table]),slot);
                types.push_back(0);continue; // dynamic root data, never part of a DXIL cache key
            }
            UINT type=number(b.stage,b.slot);
            if((b.slot&0x200u) && type==UINT(Numeric::floating)){OutputDebugStringA("frosty/binding: noninteger atomic UAV\n");ExitProcess(ERROR_INVALID_DATA);}
            types.push_back(BYTE(type));
        }
        read_views.unlock();
        if(samplers)sampler_guard.unlock();
        if(types==l->last_types)return;
        ComPtr<ID3D12PipelineState> chosen;
        if(FAILED(select_view_variant(l->typed.Get(),types,&chosen)) || !chosen)ExitProcess(ERROR_NOT_SUPPORTED);
        if(l->bound.Get()!=chosen.Get())l->host->SetPipelineState(chosen.Get());
        l->bound=std::move(chosen);l->last_types=types;
    }
}
// On the console, placement and even the creation of an SRV can precede the
// game's memcpy into its shared pages. A published descriptor is consumed
// with the current page contents. Only read-only placed images need detiling.
void collect_uploads(List* l,SIZE_T lo,SIZE_T hi,std::vector<std::shared_ptr<TextureUse>>& pending){
    l->parent->view_formats.collect(lo,hi,[&](const auto& use){
        l->texture_reads.add(use->access);if(!use->ready.load())pending.push_back(use);
    });
}
void refresh_views(Root* r,const std::vector<std::shared_ptr<TextureUse>>& pending){
    for(auto& use:pending){
        std::lock_guard guard(use->lock);
        if(!use->pending || use->ready.load())continue;
        auto hr=r->textures.refresh(r->dev.Get(),use->pending.Get(),use.get());
        if(FAILED(hr)){std::fprintf(stderr,"frosty/placement: sampled mip upload failed %#lx\n",(unsigned long)hr);ExitProcess(ERROR_INVALID_DATA);}
        if(use->ready.load())use->pending.Reset();
    }
}
void prepare_table(List* l,const RootViews* root,UINT table,D3D12_GPU_DESCRIPTOR_HANDLE h){
    if(!root || !l->views || table>=root->ranges.size())return;
    auto& srvs=root->ranges[table];if(srvs.empty())return;
    auto r=l->parent;auto base=l->view_window.gpu;auto stride=l->view_window.stride,count=l->view_window.count;
    if(h.ptr<base || h.ptr-base>=UINT64(count)*stride || (h.ptr-base)%stride)return;
    auto start=UINT((h.ptr-base)/stride);auto cpu=l->view_window.cpu;
    std::vector<std::shared_ptr<TextureUse>> pending;
    for(const auto& range:srvs){
        if(range.offset>=count-start)continue;
        auto first=start+range.offset,last=first+std::min<UINT>(range.count,count-first);
        auto lo=cpu+SIZE_T(first)*stride,hi=cpu+SIZE_T(last)*stride;
        auto revision=r->upload_revision.load(std::memory_order_acquire);
        if(l->upload_ranges.empty())l->checked_upload_revision=revision;
        auto [it,fresh]=l->upload_ranges.try_emplace({lo,hi});
        auto& check=it->second;
        if(!fresh && !check.retry && check.revision==revision)continue;
        auto before=pending.size();
        if(r->view_formats.possible(lo,hi)){
            auto guard=r->view_formats.guard(0,r->view_formats.mask(lo,last-first));
            revision=r->upload_revision.load(std::memory_order_relaxed);
            collect_uploads(l,lo,hi,pending);
        }
        // Read-only access references already belong to this recording. A
        // still-unready view must retry even without a descriptor write:
        // the title may have populated previously missing backing pages.
        check={revision,pending.size()!=before};
    }
    refresh_views(r,pending);
}
void recheck_tables(List* l){
    if(l->upload_ranges.empty() || l->checked_upload_revision==l->parent->upload_revision.load(std::memory_order_acquire))return;
    std::vector<std::shared_ptr<TextureUse>> pending;
    {
        // A volatile descriptor may have been populated since recording.
        auto& views=l->parent->view_formats;auto current=l->parent->upload_revision.load(std::memory_order_acquire);
        for(auto& [range,check]:l->upload_ranges)if(check.revision!=current){
            auto before=pending.size();
            if(views.possible(range.first,range.second)){
                auto hold=views.guard(0,views.mask(range.first,UINT((range.second-range.first)/views.stride())));
                collect_uploads(l,range.first,range.second,pending);
            }
            check={current,pending.size()!=before};
        }l->checked_upload_revision=current;}
    refresh_views(l->parent,pending);
}
void WINAPI build_as(List* l,const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* d,
                     UINT n,const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* info){
    if(!l->ray_list || !d){OutputDebugStringA("frosty/dxr: build without DXR list or descriptor\n");ExitProcess(ERROR_NOT_SUPPORTED);}
    auto desc=*d;
    if(l->parent->position_fetch && desc.Inputs.Type==D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL)
        desc.Inputs.Flags=static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(UINT(desc.Inputs.Flags)|xs_fetch_build_flag);
    [[maybe_unused]] auto time=l->pass_times.measure(l->parent->dev.Get(),l->host.Get(),
        desc.Inputs.Type==D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL?"tlas+metadata":"blas",{},desc.Inputs.NumDescs,UINT(desc.Inputs.Flags));
    l->ray_list->BuildRaytracingAccelerationStructure(&desc,n,info);
    UINT64 revision=0;
    if(l->parent->ray_tables){
        std::lock_guard hold(l->parent->geometry_lock);revision=++l->parent->scene_revision;
        if(desc.Inputs.Type==D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL)l->parent->ray_scenes[desc.DestAccelerationStructureData]={revision,{}};
        else l->parent->ray_scenes.erase(desc.DestAccelerationStructureData);
    }
    if(l->parent->dump_ray_tables && desc.Inputs.Type==D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL){
        std::lock_guard hold(l->parent->geometry_lock);l->parent->geometry_count[desc.DestAccelerationStructureData]=desc.Inputs.NumDescs;
    }else if(l->parent->ray_tables && desc.Inputs.Type==D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL &&
             desc.Inputs.DescsLayout==D3D12_ELEMENTS_LAYOUT_ARRAY && desc.Inputs.NumDescs && desc.Inputs.NumDescs<1000000){
        auto bytes=UINT64(desc.Inputs.NumDescs)*sizeof(D3D12_RAYTRACING_INSTANCE_DESC);auto where=l->parent->memory.locate(desc.Inputs.InstanceDescs,bytes);
        if(auto src=l->parent->memory.live(where)){
            D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;hp.CreationNodeMask=hp.VisibleNodeMask=1;
            D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER,0,bytes,1,1,1,DXGI_FORMAT_UNKNOWN,{1,0},D3D12_TEXTURE_LAYOUT_ROW_MAJOR,D3D12_RESOURCE_FLAG_NONE};
            RayInstances cap{desc.DestAccelerationStructureData,desc.Inputs.NumDescs,{},revision,{}};
            if(!l->parent->dump_ray_tables && cap.count>=1024)
                cap.packed=l->parent->ray_rows.record(l->parent->dev.Get(),l->host.Get(),src,where.offset,cap.count);
            static const bool timing=GetEnvironmentVariableA("FROSTY_FRAME_TIMES",nullptr,0)!=0;
            if(cap.packed){
                if(timing)play_timing::packed_ray_rows.fetch_add(cap.count,std::memory_order_relaxed);
                cap.readback=cap.packed->readback;l->instances.push_back(std::move(cap));
            }else
            if(SUCCEEDED(l->parent->dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&cap.readback)))){
                if(timing)play_timing::full_ray_rows.fetch_add(cap.count,std::memory_order_relaxed);
                D3D12_RESOURCE_BARRIER b{};b.Transition={src,0,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE};
                l->host->ResourceBarrier(1,&b);l->host->CopyBufferRegion(cap.readback.Get(),0,src,where.offset,bytes);
                std::swap(b.Transition.StateBefore,b.Transition.StateAfter);l->host->ResourceBarrier(1,&b);l->instances.push_back(std::move(cap));
            }
        }
    }
    static std::atomic<unsigned> observations{};
    if(l->parent->log_work && observations.fetch_add(1)<8)
        std::fprintf(stderr,"frosty/dxr: recorded %s count=%u flags=%#x destination=%#llx scratch=%#llx\n",
            d->Inputs.Type==D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL?"TLAS":"BLAS",
            d->Inputs.NumDescs,UINT(d->Inputs.Flags),d->DestAccelerationStructureData,d->ScratchAccelerationStructureData);
    l->parent->trace.record(l->work,{},d->Inputs.Type==D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL?WorkKind::tlas:WorkKind::blas,
                            d->Inputs.NumDescs,UINT(d->Inputs.Flags),n);
}
void bind_pending_ray(List* l){
    if(!l->pending_rt)return;
    ComPtr<ID3D12StateObject> native;
    auto hr=resolve_rt_state(l->pending_rt.Get(),native.GetAddressOf());
    if(FAILED(hr) || !native){
        char msg[110];std::snprintf(msg,sizeof(msg),"frosty/dxr: DispatchRays could not link its bound pipeline: %#lx\n",(unsigned long)hr);
        OutputDebugStringA(msg);std::fputs(msg,stderr);ExitProcess(ERROR_NOT_SUPPORTED);
    }
    l->ray_list->SetPipelineState1(native.Get());l->pending_rt.Reset();
}
std::map<UINT,RayLiveView> ray_views(List* l){
    std::map<UINT,RayLiveView> result;
    if(!l->views || !l->cs_root)return result;
    auto step=l->parent->dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto start=l->views->GetGPUDescriptorHandleForHeapStart().ptr,cpu=l->views->GetCPUDescriptorHandleForHeapStart().ptr;
    auto span=UINT64(l->views->GetDesc().NumDescriptors)*step;
    auto hold=l->parent->view_formats.guard(~UINT32(0),0);
    for(UINT cls:{0u,0x100u})for(UINT n=0;n<128;++n){
        ViewBinding b{};
        if(!find_view_binding(l->cs_layout.get(),ViewStage::ray,n|cls,b) || b.table>=l->tables.size())continue;
        auto table=l->tables[b.table];if(!table || table<start || table-start>=span)continue;
        auto off=table-start+UINT64(b.offset)*step;if(off>=span)continue;
        auto slot=l->parent->view_formats.find(cpu+off);if(!slot)continue;
        auto& v=*slot;auto num=numeric_format(v.format);
        result[n|cls]={v.dimension,v.buffer_stride?std::to_string(v.buffer_stride):v.format==DXGI_FORMAT_R32_TYPELESS?"raw":
            num==Numeric::floating?"float":num==Numeric::unsigned_int?"uint":"int",v.buffer_va};
    }
    return result;
}
void trace_ray_resources(List* l,const D3D12_DISPATCH_RAYS_DESC& d){
    if(!l->parent->log_work)return;
    static std::atomic<unsigned> calls{};if(calls.fetch_add(1)>=16)return;
    char msg[320];
    std::snprintf(msg,sizeof(msg),"frosty/dxr: XS DispatchRays %ux%ux%u, raygen=%#llx/%llu miss=%#llx/%llu/%llu hit=%#llx/%llu/%llu\n",
        d.Width,d.Height,d.Depth,d.RayGenerationShaderRecord.StartAddress,d.RayGenerationShaderRecord.SizeInBytes,
        d.MissShaderTable.StartAddress,d.MissShaderTable.SizeInBytes,d.MissShaderTable.StrideInBytes,
        d.HitGroupTable.StartAddress,d.HitGroupTable.SizeInBytes,d.HitGroupTable.StrideInBytes);
    OutputDebugStringA(msg);
    if(l->parent->ray_tables && l->pending_rt){
        auto entries=rt_identifiers(l->pending_rt.Get());wchar_t dir[32768]{};auto len=GetEnvironmentVariableW(L"FROSTY_CAPTURE_DIR",dir,DWORD(std::size(dir)));
        FILE* file=len && len<std::size(dir)?_wfopen((std::wstring(dir)+L"\\ray-tables.tsv").c_str(),L"a"):nullptr;
        if(file){
            auto rows=[&](const char* role,UINT64 address,UINT64 size,UINT64 stride){
                if(!size)return;
                std::vector<BYTE> data;
                if(!l->parent->memory.read(address,size,data)){std::fprintf(file,"unreadable\t%s\t%#llx\t%llu\n",role,address,size);return;}
                if(!stride)stride=size;
                if(stride<32 || size%stride)return;
                for(UINT64 offset=0;offset<size;offset+=stride){
                    auto p=data.data()+offset;auto it=std::ranges::find_if(entries,[&](auto& e){return !std::memcmp(e.id.data(),p,32);});
                    std::fprintf(file,"%s\t%llu\t%016llx\t",role,offset/stride,it==entries.end()?0ull:it->key);
                    for(UINT k=0;k<32;++k)std::fprintf(file,"%02x",p[k]);
                    std::fprintf(file,"\t%ls\n",it==entries.end()?L"-":it->name.c_str());
                }
            };
            rows("raygen",d.RayGenerationShaderRecord.StartAddress,d.RayGenerationShaderRecord.SizeInBytes,0);
            rows("miss",d.MissShaderTable.StartAddress,d.MissShaderTable.SizeInBytes,d.MissShaderTable.StrideInBytes);
            rows("hit",d.HitGroupTable.StartAddress,d.HitGroupTable.SizeInBytes,d.HitGroupTable.StrideInBytes);
            rows("call",d.CallableShaderTable.StartAddress,d.CallableShaderTable.SizeInBytes,d.CallableShaderTable.StrideInBytes);std::fclose(file);
        }
    }
    if(!l->views || !l->cs_root)return;
    auto step=l->parent->dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto start=l->views->GetGPUDescriptorHandleForHeapStart().ptr,cpu=l->views->GetCPUDescriptorHandleForHeapStart().ptr;
    auto span=UINT64(l->views->GetDesc().NumDescriptors)*step;
    auto hold=l->parent->view_formats.guard(~UINT32(0),0);
    for(UINT cls:{0u,0x100u})for(UINT n=0;n<64;++n){
        ViewBinding b{};
        if(!find_view_binding(l->cs_layout.get(),ViewStage::ray,n|cls,b) || b.table>=l->tables.size())continue;
        auto table=l->tables[b.table];if(!table || table<start || table-start>=span)continue;
        auto off=table-start+UINT64(b.offset)*step;if(off>=span)continue;
        auto slot=l->parent->view_formats.find(cpu+off);if(!slot)continue;
        auto& v=*slot;
        std::snprintf(msg,sizeof(msg),"frosty/dxr: space1 %c%u table=%u:%u dimension=%u format=%u stride=%u va=%#llx\n",
            cls?'u':'t',n,b.table,b.offset,v.dimension,UINT(v.format),v.buffer_stride,v.buffer_va);
        OutputDebugStringA(msg);
    }
}
// Normalize both ends of the same subresource transition. SetSRVs adds the
// context's compression-preserve fields to the actual pixel/nonpixel access.
bool normalize_transition(D3D12_RESOURCE_BARRIER& b){
    if(b.Type!=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !b.Transition.pResource)return false;
    auto& t=b.Transition;auto before=t.StateBefore,after=t.StateAfter;
    if(!((UINT(before)|UINT(after))&xs_compression_preserve))return false;
    auto d=t.pResource->GetDesc();t.StateBefore=desktop_state(d.Dimension,before);t.StateAfter=desktop_state(d.Dimension,after);
    if(t.StateBefore==before && t.StateAfter==after)return false;
    static std::atomic<UINT> reports{};
    static const bool trace=GetEnvironmentVariableA("FROSTY_TRACE_WORK",nullptr,0)!=0;
    if(trace && reports.fetch_add(1)<4){
        char msg[180];std::snprintf(msg,sizeof(msg),"frosty/barrier: format=%u subresource=%u XS %#x->%#x desktop %#x->%#x\n",
            UINT(d.Format),t.Subresource,UINT(before),UINT(after),UINT(t.StateBefore),UINT(t.StateAfter));OutputDebugStringA(msg);
    }
    return t.StateBefore==t.StateAfter;
}
void note_texture(List* l,ID3D12Resource* r){l->texture_reads.add(l->parent->textures.access(r));}
void note_barrier(List* l,const D3D12_RESOURCE_BARRIER& b){
    if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)note_texture(l,b.Transition.pResource);
    else if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING){note_texture(l,b.Aliasing.pResourceBefore);note_texture(l,b.Aliasing.pResourceAfter);}
    else if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_UAV)note_texture(l,b.UAV.pResource);
}
const auto list_slots=[]{
    auto t=empty_table<2>(std::make_index_sequence<256>{});
    t[0]=slot(+[](List* l,REFIID id,void** out)->HRESULT{
        if (!out) return E_POINTER;
        *out=nullptr;
        if (id!=__uuidof(ID3D12GraphicsCommandList) && id!=__uuidof(ID3D12CommandList) && id!=__uuidof(IUnknown) &&
            id!=__uuidof(ID3D12Object) && id!=__uuidof(ID3D12DeviceChild) &&
            !(l->ray_list && (id==__uuidof(ID3D12GraphicsCommandList1)||id==__uuidof(ID3D12GraphicsCommandList2)||
                                id==__uuidof(ID3D12GraphicsCommandList3)||id==__uuidof(ID3D12GraphicsCommandList4)||
                                (l->rate_list && (id==xs_list5 || id==xs_list6)))))return E_NOINTERFACE;
        *out=l;++l->refs;return S_OK;
    });
    t[1]=slot(+[](List* l)->ULONG{return ++l->refs;});
    t[2]=slot(+[](List* l)->ULONG{auto n=--l->refs;if(!n){release(l->parent);delete l;}return n;});
    t[4]=slot(+[](List* l,REFGUID g,UINT n,const void* d){return l->host->SetPrivateData(g,n,d);});
    t[6]=slot(+[](List* l,LPCWSTR name){return l->host->SetName(name);});
    t[7]=slot(+[](List* l,REFIID id,void** out){return query(l->parent,id,out);});
    t[8]=slot(+[](List* l){return l->host->GetType();});
    t[9]=slot(+[](List* l){l->pass_times.close(l->host.Get());return l->host->Close();});
    t[10]=slot(+[](List* l,ID3D12CommandAllocator* a,ID3D12PipelineState* p){
        l->shader_keys=work_keys(p);l->work.clear();l->pass_readbacks.clear();l->pass_times.reset();
           l->media_copies.clear();l->texture_reads.clear();l->upload_ranges.clear();l->instances.clear();l->ray_exec.clear();l->pending_rt.Reset();l->last_execution.reset();
        pipeline_metadata(l,p);l->cs_root.Reset();l->graphics_root.Reset();l->cs_layout.reset();l->graphics_layout.reset();
        l->views.Reset();l->samplers.Reset();l->view_window={};l->sampler_window={};
        l->tables.fill(0);l->graphics_tables.fill(0);
        return l->host->Reset(a,unwrap(p));});
    t[12]=slot(+[](List* l,UINT vertices,UINT instances,UINT first,UINT instance){bind_variant(l,false);
        [[maybe_unused]] auto time=l->pass_times.measure(l->parent->dev.Get(),l->host.Get(),"draw",l->shader_keys,vertices,instances);
        l->host->DrawInstanced(vertices,instances,first,instance);l->parent->trace.record(l->work,l->shader_keys,WorkKind::draw,vertices,instances,first);});
    t[13]=slot(+[](List* l,UINT indices,UINT instances,UINT first,INT base,UINT instance){bind_variant(l,false);
        [[maybe_unused]] auto time=l->pass_times.measure(l->parent->dev.Get(),l->host.Get(),"indexed",l->shader_keys,indices,instances);
        l->host->DrawIndexedInstanced(indices,instances,first,base,instance);l->parent->trace.record(l->work,l->shader_keys,WorkKind::indexed,indices,instances,first);});
    t[14]=slot(+[](List* l,UINT x,UINT y,UINT z){bind_variant(l,true);
        [[maybe_unused]] auto time=l->pass_times.measure(l->parent->dev.Get(),l->host.Get(),"dispatch",l->shader_keys,x,y,z);
        l->host->Dispatch(x,y,z);l->parent->trace.record(l->work,l->shader_keys,WorkKind::dispatch,x,y,z);});
    t[15]=slot(+[](List* l,ID3D12Resource* dst,UINT64 d,ID3D12Resource* src,UINT64 s,UINT64 n){
        if(l->parent->ray_tables)l->parent->memory.copy(dst,d,src,s,n);
        l->host->CopyBufferRegion(dst,d,src,s,n);});
    t[16]=slot(+[](List* l,const D3D12_TEXTURE_COPY_LOCATION* dst,UINT x,UINT y,UINT z,
                   const D3D12_TEXTURE_COPY_LOCATION* src,const D3D12_BOX* b){
        note_texture(l,dst->pResource);note_texture(l,src->pResource);l->host->CopyTextureRegion(dst,x,y,z,src,b);});
    t[20]=slot(+[](List* l,D3D_PRIMITIVE_TOPOLOGY t){l->host->IASetPrimitiveTopology(
        UINT(t)==0x11?D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:t);});
    t[21]=slot(+[](List* l,UINT n,const D3D12_VIEWPORT* v){l->host->RSSetViewports(n,v);});
    t[22]=slot(+[](List* l,UINT n,const D3D12_RECT* v){l->host->RSSetScissorRects(n,v);});
    t[23]=slot(+[](List* l,const FLOAT* c){l->host->OMSetBlendFactor(c);});
    t[24]=slot(+[](List* l,UINT ref){l->host->OMSetStencilRef(ref);});
    t[26]=slot(+[](List* l,UINT count,const D3D12_RESOURCE_BARRIER* b){
        [[maybe_unused]] auto time=l->pass_times.measure(l->parent->dev.Get(),l->host.Get(),"barriers",{},count);
        BarrierBatch batch(l->host.Get());
        for(UINT i=0;i<count;++i) {
             auto barrier=b[i];note_barrier(l,barrier);if(normalize_transition(barrier)){
                // A preserve-only update between identical shader reads
                // writes neither data nor application-managed compression
                // metadata. It need not drain every UAV on the GPU.
                constexpr UINT reads=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                auto state=UINT(barrier.Transition.StateAfter);
                if((!state || (state&~reads)) && !(barrier.Flags&D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY) &&
                    l->host->GetType()!=D3D12_COMMAND_LIST_TYPE_COPY){
                    D3D12_RESOURCE_BARRIER order{};order.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
                    if(state==D3D12_RESOURCE_STATE_UNORDERED_ACCESS && (barrier.Transition.pResource->GetDesc().Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
                        order.UAV.pResource=barrier.Transition.pResource;
                    batch.add(order);}
                continue;
            }
            auto hr=depth_barrier(l->parent->dev.Get(),batch,barrier);
            if(hr==S_FALSE)batch.add(barrier);
            else if(FAILED(hr)){OutputDebugStringA("frosty/depth: unsupported alias barrier\n");ExitProcess(ERROR_NOT_SUPPORTED);}
        }
        batch.flush();
    });
    t[25]=slot(&set_pipeline);
    t[29]=slot(+[](List* l,ID3D12QueryHeap* heap,D3D12_QUERY_TYPE type,UINT i){l->host->EndQuery(heap,type,i);});
    t[28]=slot(+[](List* l,ID3D12QueryHeap* heap,D3D12_QUERY_TYPE type,UINT i){l->host->BeginQuery(heap,type,i);});
    t[30]=slot(+[](List* l,ID3D12QueryHeap* heap,D3D12_QUERY_TYPE type,UINT i,UINT n,ID3D12Resource* dst,UINT64 offset){
        l->host->ResolveQueryData(heap,type,i,n,dst,offset);});
    t[32]=slot(+[](List* l,UINT n,Heap* const* heaps){
        if(n>2) return;
        ID3D12DescriptorHeap* native[2]{};
        l->views.Reset();l->samplers.Reset();l->view_window={};l->sampler_window={};
        for(UINT i=0;i<n;++i) {
            if(!heaps[i] || heaps[i]->vtbl!=heap_slots.data()) return;
            native[i]=heaps[i]->host.Get();
            auto desc=native[i]->GetDesc();auto stride=l->parent->dev->GetDescriptorHandleIncrementSize(desc.Type);
            List::Window w{native[i]->GetGPUDescriptorHandleForHeapStart().ptr,UINT64(desc.NumDescriptors)*stride,
                native[i]->GetCPUDescriptorHandleForHeapStart().ptr,stride,desc.NumDescriptors};
            if(desc.Type==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV){l->views=native[i];l->view_window=w;}
            else if(desc.Type==D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER){l->samplers=native[i];l->sampler_window=w;}
        }
        l->host->SetDescriptorHeaps(n,native);});
    t[33]=slot(+[](List* l,ID3D12RootSignature* rs){if(l->cs_root.Get()!=rs){l->tables.fill(0);l->cs_root=rs;l->cs_layout=root_views(rs);}l->host->SetComputeRootSignature(rs);});
    t[34]=slot(+[](List* l,ID3D12RootSignature* rs){if(l->graphics_root.Get()!=rs){l->graphics_tables.fill(0);l->graphics_root=rs;l->graphics_layout=root_views(rs);}l->host->SetGraphicsRootSignature(rs);});
    t[35]=slot(+[](List* l,UINT i,D3D12_GPU_DESCRIPTOR_HANDLE h){if(i<l->tables.size())l->tables[i]=h.ptr;prepare_table(l,l->cs_layout.get(),i,h);l->host->SetComputeRootDescriptorTable(i,h);});
    t[36]=slot(+[](List* l,UINT i,D3D12_GPU_DESCRIPTOR_HANDLE h){if(i<l->graphics_tables.size())l->graphics_tables[i]=h.ptr;prepare_table(l,l->graphics_layout.get(),i,h);l->host->SetGraphicsRootDescriptorTable(i,h);});
    t[47]=slot(+[](List* l,const D3D12_INDEX_BUFFER_VIEW* v){l->host->IASetIndexBuffer(v);});
    t[48]=slot(+[](List* l,UINT start,UINT n,const D3D12_VERTEX_BUFFER_VIEW* v){l->host->IASetVertexBuffers(start,n,v);});
    t[50]=slot(+[](List* l,UINT n,const D3D12_CPU_DESCRIPTOR_HANDLE* rt,BOOL single,const D3D12_CPU_DESCRIPTOR_HANDLE* ds){
        l->host->OMSetRenderTargets(n,rt,single,ds);});
    // Unlike desktop D3D12, the 210604 command list's RTV and DSV clears
    // occupy slots 52 and 51. Calling desktop slot 52 would begin a query.
    t[51]=slot(+[](List* l,D3D12_CPU_DESCRIPTOR_HANDLE d,D3D12_CLEAR_FLAGS f,FLOAT depth,UINT8 stencil,UINT n,const D3D12_RECT* rect){
        l->host->ClearDepthStencilView(d,f,depth,stencil,n,rect);});
    t[52]=slot(+[](List* l,D3D12_CPU_DESCRIPTOR_HANDLE d,const FLOAT* color,UINT n,const D3D12_RECT* rect){
         l->host->ClearRenderTargetView(d,color,n,rect);});
    // The retail buffer-clear path copies the UAV into the visible heap,
    // then passes GPU handle, original CPU handle, resource, values and rects.
    t[53]=slot(+[](List* l,D3D12_GPU_DESCRIPTOR_HANDLE gpu,D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                   ID3D12Resource* resource,const UINT* values,UINT count,const D3D12_RECT* rects){
        note_texture(l,resource);l->host->ClearUnorderedAccessViewUint(gpu,cpu,resource,values,count,rects);});
    t[55]=slot(+[](List* l,ID3D12Resource* resource,const D3D12_DISCARD_REGION* region){
         l->host->DiscardResource(resource,region);});
    t[59]=slot(+[](List* l,ID3D12CommandSignature* sig,UINT max_count,ID3D12Resource* args,UINT64 offset,
                    ID3D12Resource* counts,UINT64 count_offset){
         if(l->pending_rt){D3D12_INDIRECT_ARGUMENT_TYPE kind{};UINT size=sizeof(kind);
             if(!sig || FAILED(sig->GetPrivateData(indirect_kind_key,&size,&kind)))ExitProcess(ERROR_INVALID_DATA);
             if(kind==D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS)bind_pending_ray(l);}
        if(l->typed) {
            D3D12_INDIRECT_ARGUMENT_TYPE kind{};UINT size=sizeof(kind);
            if(!sig || FAILED(sig->GetPrivateData(indirect_kind_key,&size,&kind)) || size!=sizeof(kind)){
                OutputDebugStringA("frosty/list: missing indirect command signature metadata\n");ExitProcess(ERROR_INVALID_DATA);
            }
            bind_variant(l,kind==D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH);
        }
         D3D12_INDIRECT_ARGUMENT_TYPE pure{};UINT ps=sizeof(pure);
          if(l->parent->captures.enabled() && sig && SUCCEEDED(sig->GetPrivateData(indirect_pure_key,&ps,&pure)) && ps==sizeof(pure))
             l->parent->captures.arguments(l->parent->dev.Get(),l->host.Get(),pure==D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH?l->shader_keys.a:l->shader_keys.b,
                 pure,max_count,args,offset,counts,count_offset,l->pass_readbacks);
          [[maybe_unused]] auto time=l->pass_times.measure(l->parent->dev.Get(),l->host.Get(),"indirect",l->shader_keys,max_count);
          l->host->ExecuteIndirect(sig,max_count,args,offset,counts,count_offset);
         D3D12_INDIRECT_ARGUMENT_TYPE k{};UINT ks=sizeof(k);
          if(l->parent->log_work && sig && SUCCEEDED(sig->GetPrivateData(indirect_kind_key,&ks,&k)) && ks==sizeof(k))
             l->parent->trace.record(l->work,l->shader_keys,k==D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH?WorkKind::indirect_dispatch:
                 k==D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED?WorkKind::indirect_indexed:k==D3D12_INDIRECT_ARGUMENT_TYPE_DRAW?WorkKind::indirect_draw:WorkKind::indirect_other,max_count,counts!=nullptr,0);
    });
    t[67]=slot(+[](List* l,UINT,UINT64,UINT64){
        [[maybe_unused]] auto time=l->pass_times.measure(l->parent->dev.Get(),l->host.Get(),"XS cache order",{});
        D3D12_RESOURCE_BARRIER b{};
        b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
        l->host->ResourceBarrier(1,&b); // conservative whole-queue cache ordering
    });
    t[68]=slot(+[](List* l,FLOAT low,FLOAT high){
        ComPtr<ID3D12GraphicsCommandList1> list;
        if(FAILED(l->host.As(&list)))ExitProcess(ERROR_NOT_SUPPORTED);
        list->OMSetDepthBounds(low,high);
    });
    t[83]=slot(+[](List* l,const D3D12_TEXTURE_COPY_LOCATION* dst,UINT x,UINT y,UINT z,
                   const D3D12_TEXTURE_COPY_LOCATION* src,const D3D12_BOX* box,UINT flags){
        if (flags) {OutputDebugStringA("frosty/list: unsupported CopyTextureRegionX flags\n");ExitProcess(ERROR_NOT_SUPPORTED);}
         note_texture(l,dst->pResource);note_texture(l,src->pResource);l->host->CopyTextureRegion(dst,x,y,z,src,box);});
    // CopyResourceX: the resource-pool relocation path passes the entire
    // destination and source resources, followed by Xbox copy flags.
    t[84]=slot(+[](List* l,ID3D12Resource* dst,ID3D12Resource* src,UINT flags){
        if(flags){OutputDebugStringA("frosty/list: unsupported CopyResourceX flags\n");ExitProcess(ERROR_NOT_SUPPORTED);}
        note_texture(l,dst);note_texture(l,src);l->host->CopyResource(dst,src);});
    t[96]=slot(+[](List*,const UINT64* budget){
        // FXboxCommonD3D12CommandContext::SetAsyncComputeBudgetInternal
        // passes a zero-initialized 24-byte scheduling hint. This requests
        // the default compute budget, which the Vulkan queue already uses.
        if(!budget || budget[0] || budget[1] || budget[2]){
            OutputDebugStringA("frosty/list: unsupported nondefault async-compute budget\n");ExitProcess(ERROR_NOT_SUPPORTED);
        }
    });
    t[117]=slot(&build_as);
    t[118]=slot(+[](List* l,const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* d,UINT n,const UINT64* src){
        if(!l->ray_list)ExitProcess(ERROR_NOT_SUPPORTED);
        l->ray_list->EmitRaytracingAccelerationStructurePostbuildInfo(d,n,src);});
    t[119]=slot(+[](List* l,UINT64 dst,UINT64 src,D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode){
        if(!l->ray_list || mode==D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE){
            OutputDebugStringA("frosty/dxr: console BLAS serialization requires the title's runtime BLAS build mode\n");ExitProcess(ERROR_NOT_SUPPORTED);}
         if(l->parent->ray_tables){
             std::lock_guard hold(l->parent->geometry_lock);l->parent->ray_scenes.erase(dst);
             if(l->parent->geometry_count.contains(src))l->parent->geometry_count[dst]=l->parent->geometry_count.at(src);
             else l->parent->geometry_count.erase(dst);
         }
         l->ray_list->CopyRaytracingAccelerationStructure(dst,src,mode);});
    t[120]=slot(+[](List* l,ID3D12StateObject* p){
        if(!l->ray_list)ExitProcess(ERROR_NOT_SUPPORTED);
        l->typed.Reset();l->bound.Reset();l->last_types.clear();l->shader_keys={};auto native=native_rt_state(p);
        l->pending_rt=native?nullptr:p;if(native)l->ray_list->SetPipelineState1(native);});
    t[121]=slot(+[](List* l,const D3D12_DISPATCH_RAYS_DESC* d){
        if(!l->ray_list || !d)ExitProcess(ERROR_NOT_SUPPORTED);
        trace_ray_resources(l,*d);
        if(l->parent->demand_rays && l->pending_rt){
            auto live=ray_views(l);RayExecution exec;HRESULT hr;
            auto lookup=[&](UINT64 va)->std::shared_ptr<RaySceneLayout>{
                std::lock_guard hold(l->parent->geometry_lock);auto it=l->parent->ray_scenes.find(va);
                return it==l->parent->ray_scenes.end()?nullptr:it->second.layout;
            };
            hr=l->parent->ray_dispatch.link(l->parent->prep,l->parent->ray_device.Get(),l->cs_root.Get(),
                l->pending_rt.Get(),*d,l->parent->memory,live,lookup,exec);
             if(hr==S_OK){[[maybe_unused]] auto time=l->pass_times.measure(l->parent->dev.Get(),l->host.Get(),"rays",{exec.source,0,0},d->Width,d->Height,d->Depth);
                 exec.record(l->ray_list.Get());
                l->parent->trace.record(l->work,{exec.source,0,0},WorkKind::rays,d->Width,d->Height,d->Depth);l->ray_exec.push_back(std::move(exec));return;}
            if(FAILED(hr))ExitProcess(ERROR_NOT_SUPPORTED);
        }
         bind_pending_ray(l);
         [[maybe_unused]] auto time=l->pass_times.measure(l->parent->dev.Get(),l->host.Get(),"rays",{},d->Width,d->Height,d->Depth);
        l->ray_list->DispatchRays(d);l->parent->trace.record(l->work,{},WorkKind::rays,d->Width,d->Height,d->Depth);});
    t[124]=slot(+[](List* l,D3D12_SHADING_RATE rate,const D3D12_SHADING_RATE_COMBINER* combiners){
        if(!l->rate_list)ExitProcess(ERROR_NOT_SUPPORTED);
        l->rate_list->RSSetShadingRate(rate,combiners);});
    t[125]=slot(+[](List* l,ID3D12Resource* image){
        if(!l->rate_list)ExitProcess(ERROR_NOT_SUPPORTED);
        l->rate_list->RSSetShadingRateImage(image);});
    // FXboxCommonD3D12CommandContext copies the standard 48-byte record to
    // a zero-extended 80-byte X batch element. Each build remains independent.
    t[131]=slot(+[](List* l,UINT count,const XsBatchBuild* builds){
        for(UINT i=0;i<count;++i){
            if(std::ranges::any_of(builds[i].extension,[](auto x){return x!=0;})){
                OutputDebugStringA("frosty/dxr: unsupported batch build extension\n");ExitProcess(ERROR_NOT_SUPPORTED);}
            build_as(l,&builds[i].build,0,nullptr);
        }});
    return t;
}();
// The package has a distinct, shorter copy-list ABI. Electra requests this
// IID and calls slots 12/15 for its luma/chroma copies and transitions.
constexpr GUID xs_copy_iid{0x30bc77b9,0x8379,0x4b25,{0x95,0xde,0xb8,0x58,0x8c,0x42,0x27,0x5d}};
const auto copy_slots=[]{
    auto t=empty_table<2>(std::make_index_sequence<32>{});
    for(unsigned i=1;i<=10;++i)t[i]=list_slots[i];
    t[0]=slot(+[](List* l,REFIID iid,void** out)->HRESULT{
        if(!out)return E_POINTER;
        *out=nullptr;
        if(iid!=xs_copy_iid && iid!=__uuidof(ID3D12CommandList) && iid!=__uuidof(ID3D12DeviceChild) &&
           iid!=__uuidof(ID3D12Object) && iid!=__uuidof(IUnknown))return E_NOINTERFACE;
        *out=l;++l->refs;return S_OK;
    });
    t[11]=list_slots[15];
    t[12]=slot(+[](List* l,const D3D12_TEXTURE_COPY_LOCATION* dst,UINT x,UINT y,UINT z,
                   const D3D12_TEXTURE_COPY_LOCATION* src,const D3D12_BOX* box){
        try{l->media_copies.emplace_back(src->pResource);l->media_copies.emplace_back(dst->pResource);}
        catch(const std::bad_alloc&){ExitProcess(ERROR_OUTOFMEMORY);}
         auto from=*src,to=*dst;from.pResource=video_native_resource(from.pResource);to.pResource=video_native_resource(to.pResource);
         note_texture(l,from.pResource);note_texture(l,to.pResource);
        l->host->CopyTextureRegion(&to,x,y,z,&from,box);
    });
    t[15]=slot(+[](List* l,UINT n,const D3D12_RESOURCE_BARRIER* barriers){
        for(UINT i=0;i<n;++i){
            auto b=barriers[i];
             if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)b.Transition.pResource=video_native_resource(b.Transition.pResource);
             note_barrier(l,b);
            if(normalize_transition(b))continue;
            l->host->ResourceBarrier(1,&b);
        }
    });
    return t;
}();
HRESULT WINAPI make_list(Root* r,const UINT* x,ID3D12CommandAllocator* alloc,ID3D12PipelineState* initial,REFIID id,void** out) {
    if (!out) return E_POINTER;
    *out=nullptr;
    if (!x || x[1] || x[3]) return E_INVALIDARG;
    bool copy=id==xs_copy_iid && x[0]==D3D12_COMMAND_LIST_TYPE_COPY;
    if (!copy && id!=__uuidof(ID3D12GraphicsCommandList)) return E_NOINTERFACE;
    ComPtr<ID3D12GraphicsCommandList> host;
    auto keys=work_keys(initial);
    auto hr=r->dev->CreateCommandList(x[2],static_cast<D3D12_COMMAND_LIST_TYPE>(x[0]),alloc,unwrap(initial),IID_PPV_ARGS(&host));
    if (FAILED(hr)) return hr;
    auto l=new(std::nothrow) List(std::move(host),r,copy?copy_slots.data():list_slots.data());
    if(!l) return E_OUTOFMEMORY;
    l->shader_keys=keys;
    pipeline_metadata(l,initial);
    add(r); *out=l; return S_OK;
}
struct RetainedWork{
    std::vector<ComPtr<ID3D12Resource>> media;
    std::vector<std::pair<RayInstances,std::shared_ptr<RaySceneLayout>>> layouts;
    std::vector<RayExecution> rays;
    std::vector<PassReadback> passes;
#ifdef FROSTY_PASS_PROFILE
    std::vector<std::shared_ptr<TimedRecording>> timings;
    UINT queue_type{};UINT64 hz{};
#endif
    void finish(Root* r){
#ifdef FROSTY_PASS_PROFILE
        for(auto& t:timings)PassTimes::save(*t,queue_type,hz);
#endif
        static std::atomic<UINT> reports{};
        for(auto& e:rays){
            if(!e.retired())ExitProcess(ERROR_NOT_SUPPORTED);
            if(r->log_work && reports.fetch_add(1)<32){
                char msg[130];std::snprintf(msg,sizeof(msg),"frosty/ray-link: fenced XS DispatchRays %016llx %ux%ux%u\n",
                    e.source,e.desc.Width,e.desc.Height,e.desc.Depth);OutputDebugStringA(msg);
            }
        }
        r->captures.save(passes);
        for(auto& [cap,scene]:layouts){
            void* ptr{};D3D12_RANGE range{0,SIZE_T(cap.packed?RayRowPacker::bytes(cap.count):UINT64(cap.count)*sizeof(D3D12_RAYTRACING_INSTANCE_DESC))};
            if(FAILED(cap.readback->Map(0,&range,&ptr)))ExitProcess(ERROR_DEVICE_NOT_AVAILABLE);
            auto* data=static_cast<const D3D12_RAYTRACING_INSTANCE_DESC*>(ptr);
            std::unordered_set<UINT> bases;
            auto accept=[&](UINT base,bool opaque){scene->all_active_opaque&=opaque;bases.insert(base);};
            if(cap.packed){
                if(!RayRowPacker::visit({static_cast<const UINT*>(ptr),range.End/4},cap.count,accept))ExitProcess(ERROR_INVALID_DATA);
            }else for(UINT j=0;j<cap.count;++j)if(data[j].InstanceMask){
                auto flags=data[j].Flags;
                accept(data[j].InstanceContributionToHitGroupIndex,(flags&D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE) && !(flags&D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE));
            }
            scene->contributions.assign(bases.begin(),bases.end());
            if(!scene->publish_check()){std::fputs("frosty/ray-link: completed TLAS contains non-opaque instance\n",stderr);ExitProcess(ERROR_NOT_SUPPORTED);}
            if(r->dump_ray_tables && !cap.packed){
                wchar_t dir[32768]{};auto size=GetEnvironmentVariableW(L"FROSTY_CAPTURE_DIR",dir,DWORD(std::size(dir)));wchar_t name[72];
                std::swprintf(name,std::size(name),L"\\instances-%016llx.tsv",cap.tlas);
                std::lock_guard g(r->geometry_lock);
                if(size && size<std::size(dir))if(auto f=_wfopen((std::wstring(dir)+name).c_str(),L"w")){
                    for(UINT j=0;j<cap.count;++j){auto& v=data[j];auto it=r->geometry_count.find(v.AccelerationStructure);
                        std::fprintf(f,"%u\t%u\t%u\t%u\t%016llx\t%u\n",j,v.InstanceMask,v.InstanceContributionToHitGroupIndex,v.Flags,v.AccelerationStructure,it==r->geometry_count.end()?0:it->second);}
                    std::fclose(f);
                }
            }
            D3D12_RANGE empty{};cap.readback->Unmap(0,&empty);
        }
    }
};
const auto queue_slots=[]{
    auto t=empty_table<1>(std::make_index_sequence<64>{});
    t[0]=slot(+[](Queue* q,REFIID id,void** o)->HRESULT {
        if (!o) return E_POINTER;
        *o=nullptr;
        if (id!=__uuidof(IUnknown) && id!=__uuidof(ID3D12CommandQueue) && id!=__uuidof(ID3D12Object) &&
            id!=__uuidof(ID3D12DeviceChild) && id!=__uuidof(ID3D12Pageable)) return E_NOINTERFACE;
        ++q->refs; *o=q; return S_OK;});
    t[1]=slot(+[](Queue* q)->ULONG{return ++q->refs;});
    t[2]=slot(+[](Queue* q)->ULONG{auto n=--q->refs; if (!n) delete q; return n;});
    t[4]=slot(+[](Queue* q,REFGUID g,UINT n,const void* d){return q->host->SetPrivateData(g,n,d);});
    t[6]=slot(+[](Queue* q,LPCWSTR name){return q->host->SetName(name);});
    t[7]=slot(+[](Queue* q,REFIID id,void** o){return query(q->parent,id,o);});
    t[10]=slot(+[](Queue* q,UINT n,List* const* lists){
        try {
            std::vector<ID3D12CommandList*> host;
            host.reserve(n);
            for (UINT i=0;i<n;++i) {
                if(!lists[i] || (lists[i]->vtbl!=list_slots.data() && lists[i]->vtbl!=copy_slots.data())) {OutputDebugStringA("frosty/queue: foreign command list\n");return;}
                recheck_tables(lists[i]);
                host.push_back(lists[i]->host.Get());
            }
            std::lock_guard order(q->submission);
            for(UINT i=0;i<n;++i)if(lists[i]->last_execution)lists[i]->last_execution->wait();
            std::vector<const TextureReads*> use;use.reserve(n);
            for(UINT i=0;i<n;++i)if(!lists[i]->texture_reads.images.empty())use.push_back(&lists[i]->texture_reads);
            std::shared_ptr<RetainedWork> batch;std::shared_ptr<GpuCompletion> ticket;
            for(UINT i=0;i<n;++i){
                auto* l=lists[i];
                if(l->media_copies.empty() && l->instances.empty() && l->ray_exec.empty() && l->pass_readbacks.empty()
#ifdef FROSTY_PASS_PROFILE
                   && !l->pass_times.data
#endif
                   )continue;
                if(!batch){batch=std::make_shared<RetainedWork>();ticket=std::make_shared<GpuCompletion>();}
                // Copy owning references: Reset may discard the recording,
                // and Execute may legally submit a closed list again.
                l->last_execution=ticket;
#ifdef FROSTY_PASS_PROFILE
                if(l->pass_times.data){batch->timings.push_back(l->pass_times.data);batch->queue_type=q->type;
                    if(!batch->hz)q->host->GetTimestampFrequency(&batch->hz);}
#endif
                batch->media.insert(batch->media.end(),l->media_copies.begin(),l->media_copies.end());
                batch->rays.insert(batch->rays.end(),l->ray_exec.begin(),l->ray_exec.end());
                batch->passes.insert(batch->passes.end(),l->pass_readbacks.begin(),l->pass_readbacks.end());
                for(auto& cap:l->instances){
                    auto scene=std::make_shared<RaySceneLayout>();scene->ready=ticket;scene->count=cap.count;
                    std::lock_guard g(q->parent->geometry_lock);auto it=q->parent->ray_scenes.find(cap.tlas);
                    if(it!=q->parent->ray_scenes.end() && it->second.revision==cap.revision)it->second.layout=scene;
                    batch->layouts.emplace_back(cap,std::move(scene));
                }
            }
            // Publish the pending scene ticket before handing work to the
            // driver. A concurrently recording consumer can now wait on
            // this submission rather than mistaking it for a missing build.
            {
                play_timing::SlowCall cost(play_timing::queue_submits,q->type,n);
                if(FAILED(q->parent->textures.submit(q->parent->dev.Get(),q->host.Get(),q->upload,use,n,host.data())))
                    ExitProcess(ERROR_DEVICE_NOT_AVAILABLE);
            }
            q->progress.submitted(q->host.Get());
            if(q->parent->log_work)for(UINT i=0;i<n;++i)q->parent->trace.submitted(q->type,lists[i]->work);
            if(batch && FAILED(q->retirement.enqueue(q->parent->dev.Get(),q->host.Get(),ticket,
                [batch,r=q->parent]{batch->finish(r);})))ExitProcess(ERROR_DEVICE_NOT_AVAILABLE);
        } catch (const std::bad_alloc&) {ExitProcess(ERROR_OUTOFMEMORY);}
          catch (const std::system_error&) {ExitProcess(ERROR_NOT_ENOUGH_MEMORY);}
    });
    t[14]=slot(+[](Queue* q,ID3D12Fence* f,UINT64 v){q->progress.dependency("signal",f,v);return q->host->Signal(f,v);});
    t[15]=slot(+[](Queue* q,ID3D12Fence* f,UINT64 v){q->progress.dependency("wait",f,v);return q->host->Wait(f,v);});
    t[16]=slot(+[](Queue* q,UINT64* freq){return q->host->GetTimestampFrequency(freq);});
    t[17]=slot(+[](Queue* q,UINT64* gpu,UINT64* cpu){return q->host->GetClockCalibration(gpu,cpu);});
    t[23]=slot(+[](Queue* q,ID3D12Fence* f,UINT64 v,UINT flags)->HRESULT{
         if (flags & ~0x08000000u) return E_NOTIMPL;
         q->progress.dependency("signalX",f,v);
        return q->host->Signal(f,v);});
    t[30]=slot(+[](Queue* q,UINT n,const XsPlane* planes,const void*)->HRESULT {
        auto r=q->parent;
        std::lock_guard g(q->submission);
        auto hr=q->display.present(r->dev.Get(),q->host.Get(),r->dxgi.adapter.Get(),n,planes,r->textures,q->upload);
        if (hr==S_OK) {r->prep.start(r->dev.Get());PassTimes::presented();r->trace.presented();r->captures.present();std::lock_guard guard(r->pacing); if (r->flip_signal) SetEvent(r->flip_signal);}
        return hr;
    });
    return t;
}();
HRESULT WINAPI make_queue(Root* r,const D3D12_COMMAND_QUEUE_DESC* desc,REFIID id,void** out) {
    if (!out) return E_POINTER;
    *out=nullptr;
    if (!desc) return E_INVALIDARG;
    if (id!=__uuidof(ID3D12CommandQueue)) return E_NOINTERFACE;
    ComPtr<ID3D12CommandQueue> host;
    auto hr=r->dev->CreateCommandQueue(desc,IID_PPV_ARGS(&host));
    if (FAILED(hr)) return hr;
    *out=new(std::nothrow) Queue(queue_slots.data(),desc->Type,std::move(host),r);
    return *out ? S_OK : E_OUTOFMEMORY;
}
HRESULT WINAPI placed_x(Root* r,void* address,const D3D12_RESOURCE_DESC* xs,D3D12_RESOURCE_STATES state,
                        const D3D12_CLEAR_VALUE* clear,REFIID id,void** out) {
    if (!out) return E_POINTER;
    *out=nullptr;
    if (!xs) return E_INVALIDARG;
    state=desktop_state(xs->Dimension,state);
    if(xs->Format==DXGI_FORMAT_R8G8_UNORM){
        auto hr=video_chroma_alias(r->dev.Get(),reinterpret_cast<UINT_PTR>(address),*xs,id,out);
        if(hr!=S_FALSE)return hr;
    }
    MEMORY_BASIC_INFORMATION region{};
    VirtualQuery(address,&region,sizeof(region));
    // With partially resident mip chains the logical image base can precede
    // the first backed page. XboxTextures validates every range before reading.
    if (!address || (xs->Dimension==D3D12_RESOURCE_DIMENSION_BUFFER && region.State!=MEM_COMMIT)) {
        char buf[192];
        std::snprintf(buf,sizeof(buf),"frosty/placement: backing %p region state %#lx, dim %u %llux%u format %u flags %#x layout %#x\n",
            address,static_cast<unsigned long>(region.State),UINT(xs->Dimension),xs->Width,xs->Height,
            UINT(xs->Format),UINT(xs->Flags),UINT(xs->Layout));
        OutputDebugStringA(buf);
        return E_INVALIDARG;
    }
    auto base=reinterpret_cast<UINT_PTR>(region.AllocationBase);
    UINT64 offset=reinterpret_cast<UINT_PTR>(address)-base;
    HRESULT hr=E_NOTIMPL;
    if (xs->Dimension==D3D12_RESOURCE_DIMENSION_BUFFER) {
        ComPtr<ID3D12Device3> v3;
        ComPtr<ID3D12Heap> heap;
        auto d=*xs;
        d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Alignment=0;
        if (SUCCEEDED(hr=r->dev.As(&v3)) &&
            SUCCEEDED(hr=v3->OpenExistingHeapFromAddress(region.AllocationBase,IID_PPV_ARGS(&heap)))) {
            hr=r->dev->CreatePlacedResource(heap.Get(),offset,&d,state,clear,id,out);
            if(r->ray_tables && SUCCEEDED(hr) && *out && id==__uuidof(ID3D12Resource))r->memory.track(static_cast<ID3D12Resource*>(*out),static_cast<BYTE*>(address));
        }
    } else hr=r->textures.load(r->dev.Get(),static_cast<const BYTE*>(address),*xs,state,clear,id,out);
    if (FAILED(hr)) {
        char text[256];
        std::snprintf(text,sizeof(text),"frosty/placement: dim=%u %llux%u layers=%u mips=%u format=%u layout=%#x flags=%#x state=%#x offset=%#llx hr=%#lx\n",
            UINT(xs->Dimension),xs->Width,xs->Height,xs->DepthOrArraySize,xs->MipLevels,UINT(xs->Format),
            UINT(xs->Layout),UINT(xs->Flags),UINT(state),offset,static_cast<unsigned long>(hr));
        OutputDebugStringA(text);
    }
    return hr;
}
HRESULT WINAPI compute_pso(Root* r,const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,const void* ext,UINT flags,
                           REFIID id,void** out) {
    if (!out) return E_POINTER;
    *out=nullptr;
    if (ext || flags || !desc) return E_INVALIDARG;
    const auto& cs=desc->CS;
    if (!cs.pShaderBytecode || cs.BytecodeLength<32 || cs.BytecodeLength>64*1024*1024) return E_INVALIDARG;
    const auto* data=static_cast<const BYTE*>(cs.pShaderBytecode);
    if (std::memcmp(data,"DXBC",4)) return E_NOTIMPL;
    auto key=shader_key(cs);
    D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
    d.pRootSignature=desc->pRootSignature;
    d.CS=cs;
    ComPtr<ID3D12RootSignature> embedded;
    HRESULT hr=S_OK;
    if (!d.pRootSignature) {
        hr=create_shader_root(r->dev.Get(),0,data,cs.BytecodeLength,IID_PPV_ARGS(&embedded));
        d.pRootSignature=embedded.Get();
    }
    if (SUCCEEDED(hr)) {
        ComPtr<ID3D12PipelineState> variants;
        hr=r->prep.compute(r->dev.Get(),d,key,variants.GetAddressOf());
        if(hr==S_FALSE) hr=r->dev->CreateComputePipelineState(&d,IID_PPV_ARGS(&variants));
        if(SUCCEEDED(hr)) {
            if(id!=__uuidof(ID3D12PipelineState)) return E_NOINTERFACE;
            *out=new(std::nothrow) Pipeline(pipeline_slots.data(),variants.Get(),r,{key,0,0});
            if(!*out) return E_OUTOFMEMORY;
        }
    }
    if (FAILED(hr)) {
        char text[176];
        std::snprintf(text,sizeof(text),"frosty/pipeline: compute %016llx, %zu DXBC bytes, root %s: %#lx\n",
            static_cast<unsigned long long>(key),cs.BytecodeLength,desc->pRootSignature?"explicit":"embedded",
            static_cast<unsigned long>(hr));
        OutputDebugStringA(text);
        DWORD written; WriteFile(GetStdHandle(STD_ERROR_HANDLE),text,DWORD(std::strlen(text)),&written,nullptr);
    }
    return hr;
}
HRESULT WINAPI graphics_stream(Root* r,const D3D12_PIPELINE_STATE_STREAM_DESC* desc,REFIID id,void** out) {
    if(!out)return E_POINTER;
    *out=nullptr;
    if(!desc || !desc->pPipelineStateSubobjectStream || id!=__uuidof(ID3D12PipelineState)) return E_INVALIDARG;
    if(desc->SizeInBytes!=0x2c8) {
        char text[180];std::snprintf(text,sizeof(text),"frosty/pipeline: unsupported XS graphics stream size %#zx (first tag %u)\n",
            desc->SizeInBytes,desc->SizeInBytes>=4?*static_cast<const UINT*>(desc->pPipelineStateSubobjectStream):~0u);
        OutputDebugStringA(text);std::fputs(text,stderr);
        wchar_t dir[MAX_PATH]{},file[MAX_PATH+64]{};
        if(desc->SizeInBytes && desc->SizeInBytes<=4096 && GetEnvironmentVariableA("FROSTY_TRACE_PSO",nullptr,0) &&
           GetEnvironmentVariableW(L"FROSTY_CAPTURE_DIR",dir,MAX_PATH)<MAX_PATH && dir[0]){
            std::swprintf(file,std::size(file),L"%ls\\xs-stream-%zx.bin",dir,desc->SizeInBytes);
            if(auto fp=_wfopen(file,L"wb")){std::fwrite(desc->pPipelineStateSubobjectStream,1,desc->SizeInBytes,fp);std::fclose(fp);}
        }
        return E_NOTIMPL;
    }
    const auto* bytes=static_cast<const BYTE*>(desc->pPipelineStateSubobjectStream);
    auto get=[&]<class T>(size_t offset){T v;std::memcpy(&v,bytes+offset,sizeof(v));return v;};
    for(auto [offset,tag]:{std::pair{0x0,20}, {0x8,0},{0x18,12},{0x30,13},{0x38,14},
                          {0x40,1},{0x58,5},{0x70,2},{0x88,8},{0x1d8,21},{0x218,16},{0x220,10},
                          {0x250,15},{0x278,17},{0x288,9},{0x290,19},{0x2a8,26}}) {
        if(get.operator()<UINT>(offset)!=UINT(tag)) return E_NOTIMPL;
    }
    // No console cached PSO or Xbox-only pipeline extension goes to the
    // desktop driver. Reconstruct the standard state from the actual stream.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature=get.operator()<ID3D12RootSignature*>(0x10);
    d.InputLayout=get.operator()<D3D12_INPUT_LAYOUT_DESC>(0x20);
    d.IBStripCutValue=get.operator()<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(0x34);
    d.PrimitiveTopologyType=get.operator()<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(0x3c);
    d.VS=get.operator()<D3D12_SHADER_BYTECODE>(0x48);
    d.GS=get.operator()<D3D12_SHADER_BYTECODE>(0x60);
    d.PS=get.operator()<D3D12_SHADER_BYTECODE>(0x78);
    d.BlendState=get.operator()<D3D12_BLEND_DESC>(0x8c);
    d.DepthStencilState=get.operator()<D3D12_DEPTH_STENCIL_DESC>(0x1dc);
    d.DSVFormat=get.operator()<DXGI_FORMAT>(0x21c);
    d.RasterizerState=get.operator()<D3D12_RASTERIZER_DESC>(0x224);
    std::memcpy(d.RTVFormats,bytes+0x254,sizeof(d.RTVFormats));
    d.NumRenderTargets=get.operator()<UINT>(0x274);
    d.SampleDesc=get.operator()<DXGI_SAMPLE_DESC>(0x27c);
    d.SampleMask=get.operator()<UINT>(0x28c);
    ComPtr<ID3D12PipelineState> native;
    auto original_vs=d.VS,original_ps=d.PS;
    auto bounds=get.operator()<BOOL>(0x1dc+offsetof(D3D12_DEPTH_STENCIL_DESC1,DepthBoundsTestEnable));
    auto hr=r->prep.graphics(r->dev.Get(),d,bounds,native.GetAddressOf());
    if(FAILED(hr)) {
        char msg[192];std::snprintf(msg,sizeof(msg),"frosty/pipeline: graphics VS=%016llx (%zu), PS=%016llx (%zu), GS=%016llx, hr=%#lx\n",
             shader_key(original_vs),original_vs.BytecodeLength,shader_key(original_ps),original_ps.BytecodeLength,shader_key(d.GS),static_cast<unsigned long>(hr));
        OutputDebugStringA(msg);
        DWORD n; WriteFile(GetStdHandle(STD_ERROR_HANDLE),msg,DWORD(std::strlen(msg)),&n,nullptr);
        return hr;
    }
    auto vs_key=shader_key(original_vs),ps_key=shader_key(original_ps),gs_key=shader_key(d.GS);
    static FILE* witness=[](){wchar_t path[2048];auto n=GetEnvironmentVariableW(L"XS_DEBUG_PSO_KEYS",path,std::size(path));
        return n && n<std::size(path)?_wfopen(path,L"w"):nullptr;}();
    if(witness){
        std::fprintf(witness,"%016llx\t%016llx\t%016llx\n",vs_key,ps_key,gs_key);
        std::fflush(witness); // the bounded developer session need not exit through the guest CRT
    }
    *out=new(std::nothrow) Pipeline(pipeline_slots.data(),native.Get(),r,{vs_key,ps_key,gs_key});
    return *out?S_OK:E_OUTOFMEMORY;
}
D3D12_RESOURCE_DESC heap_resource_desc(const D3D12_RESOURCE_DESC& xs){
    auto host=desktop_desc(xs);
    // The XS transient allocator can place an MSAA surface inside a 4 MiB
    // page. Request explicit 64 KiB placement for both sizing and creation;
    // vkd3d includes any padding needed for the actual Vulkan image. Keep
    // the title's offset so overlapping heap allocations still alias.
    if(host.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D && host.SampleDesc.Count>1)
        host.Alignment=D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    return host;
}
const auto slots = [] {
    auto t = empty_table(std::make_index_sequence<192>{});
    t[0]=slot(&query); t[1]=slot(&add); t[2]=slot(&release);
    t[3]=slot(+[](Root* r,REFGUID g,UINT* n,void* d){return r->dev->GetPrivateData(g,n,d);});
    t[4]=slot(+[](Root* r,REFGUID g,UINT n,const void* d){return r->dev->SetPrivateData(g,n,d);});
    t[6]=slot(+[](Root* r,LPCWSTR n){return r->dev->SetName(n);});
    t[7]=slot(+[](Root* r){ return r->dev->GetNodeCount(); });
    t[8]=slot(&make_queue);
    t[9]=slot(+[](Root* r,D3D12_COMMAND_LIST_TYPE d,REFIID id,void** o){
        return r->dev->CreateCommandAllocator(d,id,o); });
    t[13]=slot(&features);
    t[14]=slot(&make_heap);
    t[15]=slot(+[](Root* r,D3D12_DESCRIPTOR_HEAP_TYPE t){return r->dev->GetDescriptorHandleIncrementSize(t);});
    t[16]=slot(+[](Root* r,UINT node,const void* blob,SIZE_T len,REFIID id,void** o){
        return create_shader_root(r->dev.Get(),node,blob,len,id,o); });
    t[17]=slot(+[](Root* r,const D3D12_CONSTANT_BUFFER_VIEW_DESC* d,D3D12_CPU_DESCRIPTOR_HANDLE dst){
        r->set_view(dst.ptr,nullptr);
        if(!d) {r->dev->CreateConstantBufferView(nullptr,dst);return;}
        auto copy=*d;
        if(copy.SizeInBytes && copy.SizeInBytes<=65536) copy.SizeInBytes=(copy.SizeInBytes+255u)&~255u;
        r->dev->CreateConstantBufferView(&copy,dst); });
    t[18]=slot(+[](Root* r,ID3D12Resource* res,const D3D12_SHADER_RESOURCE_VIEW_DESC* d,D3D12_CPU_DESCRIPTOR_HANDLE dst){
        auto desc=res?res->GetDesc():D3D12_RESOURCE_DESC{};
        BoundView v{};v.format=d?d->Format:desc.Format;v.dimension=d?UINT(d->ViewDimension):0;
        if(d && d->ViewDimension==D3D12_SRV_DIMENSION_BUFFER)
            buffer_view(v,res,d->Buffer.FirstElement,d->Buffer.NumElements,d->Buffer.StructureByteStride,d->Buffer.Flags&D3D12_BUFFER_SRV_FLAG_RAW);
        if(d && d->ViewDimension==D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE)v.buffer_va=d->RaytracingAccelerationStructure.Location;
        v.upload=r->textures.shader_view(res,d);
        v.image=desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        if(v.image) {
            v.width=UINT(desc.Width-1);v.height=desc.Height-1;
            if(d)switch(d->ViewDimension){
                case D3D12_SRV_DIMENSION_TEXTURE2D:v.base=d->Texture2D.MostDetailedMip;
                    v.min_lod=std::max(0.f,d->Texture2D.ResourceMinLODClamp-float(v.base));break;
                case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:v.base=d->Texture2DArray.MostDetailedMip;break;
                case D3D12_SRV_DIMENSION_TEXTURE3D:v.base=d->Texture3D.MostDetailedMip;break;
                case D3D12_SRV_DIMENSION_TEXTURECUBE:v.base=d->TextureCube.MostDetailedMip;break;
                case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:v.base=d->TextureCubeArray.MostDetailedMip;break;
                default:v.image=false;break;
            }
        }
        r->set_view(dst.ptr,&v);
        if (v.upload && FAILED(r->textures.refresh(r->dev.Get(),res))) OutputDebugStringA("frosty/placement: SRV residency update failed\n");
        r->dev->CreateShaderResourceView(res,d,dst); });
    t[19]=slot(+[](Root* r,ID3D12Resource* res,ID3D12Resource* counter,const D3D12_UNORDERED_ACCESS_VIEW_DESC* d,D3D12_CPU_DESCRIPTOR_HANDLE dst){
        BoundView v{};v.format=d?d->Format:res?res->GetDesc().Format:DXGI_FORMAT_UNKNOWN;v.dimension=d?UINT(d->ViewDimension):0;
        v.upload=r->textures.shader_view(res,nullptr);
        if(d && d->ViewDimension==D3D12_UAV_DIMENSION_BUFFER)
            buffer_view(v,res,d->Buffer.FirstElement,d->Buffer.NumElements,d->Buffer.StructureByteStride,d->Buffer.Flags&D3D12_BUFFER_UAV_FLAG_RAW);
        r->set_view(dst.ptr,&v);
        auto hr=depth_uav_view(r->dev.Get(),res,d,dst);
        if(hr==S_FALSE)r->dev->CreateUnorderedAccessView(res,counter,d,dst);
        else if(FAILED(hr)){OutputDebugStringA("frosty/depth: unsupported alias view\n");ExitProcess(ERROR_NOT_SUPPORTED);} });
    t[20]=slot(+[](Root* r,ID3D12Resource* res,const D3D12_RENDER_TARGET_VIEW_DESC* d,D3D12_CPU_DESCRIPTOR_HANDLE dst){
        r->dev->CreateRenderTargetView(res,d,dst); });
    t[21]=slot(+[](Root* r,ID3D12Resource* res,const D3D12_DEPTH_STENCIL_VIEW_DESC* d,D3D12_CPU_DESCRIPTOR_HANDLE dst){
        r->dev->CreateDepthStencilView(res,d,dst); });
    t[22]=slot(+[](Root* r,const D3D12_SAMPLER_DESC* d,D3D12_CPU_DESCRIPTOR_HANDLE dst){
        {std::lock_guard g(r->sampler_lock);if(d)r->sampler_desc[dst.ptr]=*d;else r->sampler_desc.erase(dst.ptr);}
        r->dev->CreateSampler(d,dst); });
    t[23]=slot(+[](Root* r,UINT n,const D3D12_CPU_DESCRIPTOR_HANDLE* dst,const UINT* sizes,
                   UINT m,const D3D12_CPU_DESCRIPTOR_HANDLE* src,const UINT* source_sizes,D3D12_DESCRIPTOR_HEAP_TYPE type){
        // The retail ray cache publishes one contiguous table from individual
        // source handles. Keep its lock/publication boundary and copy order,
        // but look up each destination metadata page only once.
        if(type==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && n==1 && !source_sizes && (sizes?sizes[0]:1)==m){
            auto& v=r->view_formats;UINT32 reads=0;for(UINT i=0;i<m;++i)reads|=v.mask(src[i].ptr);
            auto guard=v.guard(reads,v.mask(dst[0].ptr,m));
            if(v.scatter(dst[0].ptr,m,[&](UINT i){return src[i].ptr;}))++r->upload_revision;
        }else if(type==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || type==D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) {
            UINT32 read=0,write=0;std::unique_lock sg(r->sampler_lock,std::defer_lock);
            if(type==D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)sg.lock();
            else{
                for(UINT i=0;i<m;++i)read|=r->view_formats.mask(src[i].ptr,source_sizes?source_sizes[i]:1);
                for(UINT i=0;i<n;++i)write|=r->view_formats.mask(dst[i].ptr,sizes?sizes[i]:1);
            }
            auto guard=r->view_formats.guard(read,write);
            SIZE_T step=r->dev->GetDescriptorHandleIncrementSize(type);
            UINT si=0,sj=0;bool changed=false;
            for(UINT di=0;di<n;++di) for(UINT dj=0,dn=sizes?sizes[di]:1;dj<dn;) {
                while(si<m && sj>=(source_sizes?source_sizes[si]:1)) {++si;sj=0;}
                if(si>=m) break;
                auto s=src[si].ptr+sj*step,d=dst[di].ptr+dj*step;
                auto count=std::min(dn-dj,(source_sizes?source_sizes[si]:1)-sj);sj+=count;dj+=count;
                if(type==D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)for(UINT k=0;k<count;++k,s+=step,d+=step){
                    auto& map=r->sampler_desc;if(auto it=map.find(s);it!=map.end())map[d]=it->second;else map.erase(d);}
                else changed|=r->view_formats.copy(s,d,count);
            }
            if(changed)++r->upload_revision;
        }
        r->dev->CopyDescriptors(n,dst,sizes,m,src,source_sizes,type); });
    t[24]=slot(+[](Root* r,UINT n,D3D12_CPU_DESCRIPTOR_HANDLE dst,D3D12_CPU_DESCRIPTOR_HANDLE src,
                   D3D12_DESCRIPTOR_HEAP_TYPE type){
        if(type==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || type==D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) {
            SIZE_T step=r->dev->GetDescriptorHandleIncrementSize(type);
            if(type==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV){
                auto guard=r->view_formats.guard(r->view_formats.mask(src.ptr,n),r->view_formats.mask(dst.ptr,n));
                if(r->view_formats.copy(src.ptr,dst.ptr,n))++r->upload_revision;
            }else{std::lock_guard guard(r->sampler_lock);for(UINT i=0;i<n;++i) {
                auto s=src.ptr+i*step,d=dst.ptr+i*step;
                auto& map=r->sampler_desc;if(auto it=map.find(s);it!=map.end())map[d]=it->second;else map.erase(d);
            }}
        }
        r->dev->CopyDescriptorsSimple(n,dst,src,type); });
    t[25]=slot(+[](D3D12_RESOURCE_ALLOCATION_INFO* result,Root* r,UINT mask,UINT n,const BYTE* xs) {
        // XS aggregate-return convention: RCX=result, RDX=this; XS descs
        // have an additional trailing 8 bytes compared to desktop descs.
        *result={UINT64_MAX,0};
        if (xs && n && n<=1024) {
            try {
                std::vector<D3D12_RESOURCE_DESC> desc(n);
                for (UINT i=0;i<n;++i) {
                    std::memcpy(&desc[i],xs+64ull*i,sizeof(desc[i]));
                    desc[i]=heap_resource_desc(desc[i]);
                }
                *result=r->dev->GetResourceAllocationInfo(mask,n,desc.data());
            } catch (const std::bad_alloc&) {}
        }
        return result;
    });
    t[27]=slot(+[](Root* r,const D3D12_HEAP_PROPERTIES* props,D3D12_HEAP_FLAGS flags,
            const D3D12_RESOURCE_DESC* desc,D3D12_RESOURCE_STATES state,const D3D12_CLEAR_VALUE* clear,
            REFIID id,void** o){
        auto host=desktop_desc(*desc);
        auto initial=desktop_state(host.Dimension,state);bool hit,candidate;HRESULT hr;
        {
            play_timing::SlowCall cost(play_timing::allocations,(UINT64(props?props->Type:0)<<8)|host.Dimension,host.Width);
            cost.heap_flags=flags;cost.resource_flags=host.Flags;cost.layout=host.Layout;cost.state=initial;cost.alignment=host.Alignment;
            hr=r->upload_reserve.create(r->dev.Get(),props,flags,host,initial,clear,id,o,hit,candidate);
        }
        if(candidate)(hit?play_timing::upload_reserve_hits:play_timing::upload_reserve_misses).fetch_add(1,std::memory_order_relaxed);
        if(r->ray_tables && SUCCEEDED(hr) && o && *o && id==__uuidof(ID3D12Resource))r->memory.track(static_cast<ID3D12Resource*>(*o));
        if(SUCCEEDED(hr) && o && *o && id==__uuidof(ID3D12Resource))tag_depth_alias(static_cast<ID3D12Resource*>(*o),*desc);
        return hr; });
    t[28]=slot(+[](Root* r,const D3D12_HEAP_DESC* d,REFIID id,void** out){
        play_timing::SlowCall cost(play_timing::allocations,d?UINT64(d->Properties.Type)<<8:0,d?d->SizeInBytes:0);
        if(d){cost.heap_flags=d->Flags;cost.alignment=d->Alignment;}
        return r->dev->CreateHeap(d,id,out);});
    t[29]=slot(+[](Root* r,ID3D12Heap* heap,UINT64 offset,const D3D12_RESOURCE_DESC* desc,
                   D3D12_RESOURCE_STATES state,const D3D12_CLEAR_VALUE* clear,REFIID id,void** o){
        auto host=heap_resource_desc(*desc);
        auto hr=r->dev->CreatePlacedResource(heap,offset,&host,desktop_state(host.Dimension,state),clear,id,o);
        if(r->ray_tables && SUCCEEDED(hr) && o && *o && id==__uuidof(ID3D12Resource))r->memory.track(static_cast<ID3D12Resource*>(*o));
        if(SUCCEEDED(hr) && host.SampleDesc.Count>1 && (offset&(D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT-1))){
            static std::atomic<bool> reported{};
            if(GetEnvironmentVariableA("FROSTY_TRACE_WORK",nullptr,0) && !reported.exchange(true)){
                char msg[164];std::snprintf(msg,sizeof(msg),"frosty/placement: placed %ux MSAA %llux%u format=%u at %#llx alignment=%#llx\n",
                    host.SampleDesc.Count,host.Width,host.Height,UINT(host.Format),offset,host.Alignment);OutputDebugStringA(msg);
            }
        }
        if(SUCCEEDED(hr) && o && *o && id==__uuidof(ID3D12Resource))tag_depth_alias(static_cast<ID3D12Resource*>(*o),*desc);
        return hr; });
    t[36]=slot(+[](Root* r,UINT64 init,D3D12_FENCE_FLAGS f,REFIID id,void** o){
        return r->dev->CreateFence(init,f,id,o); });
    t[37]=slot(+[](Root* r){ return r->dev->GetDeviceRemovedReason(); });
    t[38]=slot(+[](Root* r,const D3D12_RESOURCE_DESC* desc,UINT first,UINT n,UINT64 base,
                   D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layout,UINT* rows,UINT64* strides,UINT64* bytes){
        r->dev->GetCopyableFootprints(desc,first,n,base,layout,rows,strides,bytes); });
    t[39]=slot(+[](Root* r,const D3D12_QUERY_HEAP_DESC* d,REFIID id,void** o){ return r->dev->CreateQueryHeap(d,id,o); });
    t[41]=slot(&command_signature);
    t[44]=slot(&placed_x);
    // CreateCommandQueueX's four dwords are type, flags, priority class,
    // and the Xbox compute engine selection. Use a normal host compute queue.
    t[48]=slot(+[](Root* r,const UINT* x,REFIID id,void** out) -> HRESULT {
        if (!x || x[0]!=D3D12_COMMAND_LIST_TYPE_COMPUTE) return E_INVALIDARG;
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type=D3D12_COMMAND_LIST_TYPE_COMPUTE;
        desc.Flags=static_cast<D3D12_COMMAND_QUEUE_FLAGS>(x[1]);
        return make_queue(r,&desc,id,out);
    });
    t[62]=slot(&compute_pso);
    t[73]=slot(&make_list);
    t[78]=slot(&set_interval);
    t[79]=slot(&schedule);
    t[80]=slot(&wait_origin);
    t[81]=slot(+[](Root*,UINT64,UINT,UINT* count,void*)->HRESULT {
        if(!count) return E_POINTER;
        *count=0;
        return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE; // No Xbox compositor timing samples on desktop.
    });
    t[85]=slot(+[](Root* r,UINT n,ID3D12Pageable* const* objs,const D3D12_RESIDENCY_PRIORITY* p) {
        ComPtr<ID3D12Device1> v1; auto hr=r->dev.As(&v1);
        return FAILED(hr)?hr:v1->SetResidencyPriority(n,objs,p);});
    t[86]=slot(&graphics_stream);
    t[101]=slot(+[](Root* r,const D3D12_STATE_OBJECT_DESC* desc,REFIID iid,void** out)->HRESULT{
        return r->ray_device?xs_rt_pipeline(r->ray_device.Get(),reinterpret_cast<IUnknown*>(r),desc,iid,out):E_NOINTERFACE;});
    t[102]=slot(+[](Root* r,const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* in,D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* out){
        if(!r->ray_device || !in || !out)ExitProcess(ERROR_NOT_SUPPORTED);
        auto desc=*in;
        if(r->position_fetch && desc.Type==D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL)
            desc.Flags=static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(UINT(desc.Flags)|xs_fetch_build_flag);
        r->ray_device->GetRaytracingAccelerationStructurePrebuildInfo(&desc,out);});
    t[106]=slot(+[](Root* r,const D3D12_STATE_OBJECT_DESC* desc,ID3D12StateObject* base,REFIID iid,void** out)->HRESULT{
        return r->ray_device7?xs_rt_extend(r->ray_device7.Get(),reinterpret_cast<IUnknown*>(r),desc,base,iid,out):E_NOINTERFACE;});
    // Serialized XS collections carry an RDNA library, not portable DXIL.
    t[115]=slot(+[](Root* r,const void* bytes,UINT size,ID3D12RootSignature* root,UINT flags,REFIID iid,void** out){
        auto hr=xs_rt_collection(r->ray_device.Get(),reinterpret_cast<IUnknown*>(r),bytes,size,root,flags,iid,out);
        if(r->demand_rays && SUCCEEDED(hr) && out && *out)try{r->ray_dispatch.collection(bytes,size,static_cast<ID3D12StateObject*>(*out));}
            catch(const std::exception&){static_cast<ID3D12StateObject*>(*out)->Release();*out=nullptr;return E_FAIL;}
        return hr;});
    return t;
}();
}

extern "C" HRESULT WINAPI FrostyGetVideoDevice(ID3D12Device* device,ID3D12Device** out) {
    if(!device||!out)return E_POINTER;
    auto* root=reinterpret_cast<Root*>(device);
    if(root->vtbl==slots.data())return root->dev.CopyTo(out);
    return device->QueryInterface(IID_PPV_ARGS(out)); // a native desktop MF manager
}
extern "C" HRESULT WINAPI D3D12XboxCreateDevice(IUnknown* requested_adapter, const UINT* params, REFIID id, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!params || params[0] != 0x20004) return E_INVALIDARG;
    if (id != __uuidof(ID3D12Device)) return E_NOINTERFACE;
    auto r = new(std::nothrow) Root{slots.data()};
    if (!r) return E_OUTOFMEMORY;
    ComPtr<IDXGIFactory4> factory;
    auto hr = D3D12CreateDevice(requested_adapter,D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&r->dev));
    if (SUCCEEDED(hr)) hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->EnumAdapterByLuid(r->dev->GetAdapterLuid(), IID_PPV_ARGS(&r->dxgi.adapter));
    if (FAILED(hr)) { delete r; return hr; }
    r->view_formats.increment(r->dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 caps{};
    if(SUCCEEDED(r->dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&caps,sizeof(caps))) &&
       caps.RaytracingTier>=D3D12_RAYTRACING_TIER_1_0)r->dev.As(&r->ray_device);
    if(r->ray_device && caps.RaytracingTier>=D3D12_RAYTRACING_TIER_1_1)r->dev.As(&r->ray_device7);
    if(r->ray_device7)r->position_fetch=xs_enable_position_fetch(r->dev.Get());
    if(r->ray_device && r->demand_rays && !r->dump_ray_tables && !GetEnvironmentVariableA("FROSTY_FULL_RAY_ROWS",nullptr,0))
        r->ray_rows.init(r->dev.Get());
    OutputDebugStringA("frosty/d3d12_xs: created Xbox root device with matching DXGI adapter\n");
    *out = r;
    return S_OK;
}
