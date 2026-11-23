// Verify the actual lifted ClearCS at the XS entry points: typed u0 and b0 at
// different table indices, nonzero start, a partial final group and sentinels.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

extern "C" HRESULT WINAPI D3D12XboxCreateDevice(IUnknown*,const UINT*,REFIID,void**);
using Microsoft::WRL::ComPtr;
namespace {
struct XS : IUnknown {};
template<class Fn> Fn method(XS* obj, size_t index) {return std::bit_cast<Fn>((*reinterpret_cast<void***>(obj))[index]);}
void check(HRESULT hr) { if(FAILED(hr)){std::fprintf(stderr,"ClearCS HRESULT %#lx\n",static_cast<unsigned long>(hr));throw std::runtime_error("device call");} }
}
int main(int argc,char** argv) try {
    if(argc<2 || argc>4) return 2;
    bool indirect=argc==4 && std::string_view(argv[3])=="--indirect";
    if(argc==4 && !indirect)return 2;
    unsigned group=64;
    if(argc>=3) {
        std::string_view arg=argv[2];
        auto [end,ec]=std::from_chars(arg.data(),arg.data()+arg.size(),group);
        if(ec!=std::errc{} || end!=arg.data()+arg.size() || !group || group>1024) return 2;
    }
    std::ifstream in(argv[1],std::ios::binary);
    std::vector<char> bytes{std::istreambuf_iterator<char>(in),{}};
    if(bytes.empty()) throw std::runtime_error("missing retail ClearCS");
    ComPtr<XS> r,queue,list,pipeline,heap;
    const std::array<UINT,32> params{0x20004};
    check(D3D12XboxCreateDevice(nullptr,params.data(),__uuidof(ID3D12Device),reinterpret_cast<void**>(r.GetAddressOf())));
    D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_CBV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0}};
    D3D12_ROOT_PARAMETER ps[2]{};
    for(UINT i=0;i<2;++i){ps[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;ps[i].DescriptorTable={1,ranges+i};}
    D3D12_ROOT_SIGNATURE_DESC rsd{2,ps,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> sig;
    check(D3D12SerializeRootSignature(&rsd,D3D_ROOT_SIGNATURE_VERSION_1,&sig,nullptr));
    ComPtr<ID3D12RootSignature> rs;
    check(method<HRESULT(*)(XS*,UINT,const void*,SIZE_T,REFIID,void**)>(r.Get(),16)(r.Get(),0,sig->GetBufferPointer(),
          sig->GetBufferSize(),IID_PPV_ARGS(&rs)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.CS={bytes.data(),bytes.size()};pd.pRootSignature=rs.Get();
    check(method<HRESULT(*)(XS*,const D3D12_COMPUTE_PIPELINE_STATE_DESC*,const void*,UINT,REFIID,void**)>(r.Get(),62)(
        r.Get(),&pd,nullptr,0,__uuidof(ID3D12PipelineState),reinterpret_cast<void**>(pipeline.GetAddressOf())));
    D3D12_COMMAND_QUEUE_DESC qd{};
    check(method<HRESULT(*)(XS*,const D3D12_COMMAND_QUEUE_DESC*,REFIID,void**)>(r.Get(),8)(
        r.Get(),&qd,__uuidof(ID3D12CommandQueue),reinterpret_cast<void**>(queue.GetAddressOf())));
    ComPtr<ID3D12CommandAllocator> a;
    check(method<HRESULT(*)(XS*,D3D12_COMMAND_LIST_TYPE,REFIID,void**)>(r.Get(),9)(r.Get(),D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a)));
    UINT list_desc[]{0,0,1,0};
    check(method<HRESULT(*)(XS*,const UINT*,ID3D12CommandAllocator*,ID3D12PipelineState*,REFIID,void**)>(r.Get(),73)(
        r.Get(),list_desc,a.Get(),nullptr,__uuidof(ID3D12GraphicsCommandList),reinterpret_cast<void**>(list.GetAddressOf())));
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,2,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    check(method<HRESULT(*)(XS*,const D3D12_DESCRIPTOR_HEAP_DESC*,REFIID,void**)>(r.Get(),14)(
        r.Get(),&hd,__uuidof(ID3D12DescriptorHeap),reinterpret_cast<void**>(heap.GetAddressOf())));
    auto make=[&](D3D12_HEAP_TYPE t,UINT n,D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES initial){
        ComPtr<ID3D12Resource> b;
        D3D12_HEAP_PROPERTIES prop{};prop.Type=t;prop.CreationNodeMask=prop.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_BUFFER,0,n,1,1,1,DXGI_FORMAT_UNKNOWN,
            {1,0},D3D12_TEXTURE_LAYOUT_ROW_MAJOR,flags};
        check(method<HRESULT(*)(XS*,const D3D12_HEAP_PROPERTIES*,D3D12_HEAP_FLAGS,const D3D12_RESOURCE_DESC*,
              D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,REFIID,void**)>(r.Get(),27)(
              r.Get(),&prop,D3D12_HEAP_FLAG_NONE,&d,initial,nullptr,IID_PPV_ARGS(&b)));
        return b;
    };
    const UINT count=group+20,words=indirect?5:4,total=count*words*4,stop=count-5;
    auto upload=make(D3D12_HEAP_TYPE_UPLOAD,total+256,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_GENERIC_READ);
    auto gpu=make(D3D12_HEAP_TYPE_DEFAULT,total,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
    auto read=make(D3D12_HEAP_TYPE_READBACK,total*(indirect?2:1),D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
    UINT* ptr{};check(upload->Map(0,nullptr,reinterpret_cast<void**>(&ptr)));
    std::fill_n(ptr,(total+256)/4,0xc0decafe);
    constexpr std::array<UINT,4> values{0x56789abc,37,0x80000001,0xffffffff};
    std::copy(values.begin(),values.end(),ptr);ptr[4]=7;ptr[8]=stop;
    if(indirect){
        ptr[2]=stop;
        constexpr std::array<UINT,6> commands{1,1,1,2,1,1}; // count buffer permits only the first
        std::copy(commands.begin(),commands.end(),ptr+16);ptr[32]=1;
    }
    upload->Unmap(0,nullptr);
    auto cpu=method<SIZE_T(*)(XS*)>(heap.Get(),9)(heap.Get());
    auto addr=method<UINT64(*)(XS*)>(heap.Get(),10)(heap.Get());
    auto stride=method<UINT(*)(XS*,D3D12_DESCRIPTOR_HEAP_TYPE)>(r.Get(),15)(r.Get(),hd.Type);
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{upload->GetGPUVirtualAddress(),256};
    method<void(*)(XS*,const D3D12_CONSTANT_BUFFER_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE)>(r.Get(),17)(r.Get(),&cbv,{cpu});
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=indirect?DXGI_FORMAT_R32_UINT:DXGI_FORMAT_R32G32B32A32_UINT;
    uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;uav.Buffer.NumElements=indirect?count*words:count;
    method<void(*)(XS*,ID3D12Resource*,ID3D12Resource*,const D3D12_UNORDERED_ACCESS_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE)>(r.Get(),19)(
        r.Get(),gpu.Get(),nullptr,&uav,{cpu+stride});
    auto copy=method<void(*)(XS*,ID3D12Resource*,UINT64,ID3D12Resource*,UINT64,UINT64)>(list.Get(),15);
    auto transition=[&](D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){
        D3D12_RESOURCE_BARRIER b{};b.Transition={gpu.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};
        method<void(*)(XS*,UINT,const D3D12_RESOURCE_BARRIER*)>(list.Get(),26)(list.Get(),1,&b);
    };
    copy(list.Get(),gpu.Get(),0,upload.Get(),256,total);
    transition(D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto* h=heap.Get();
    method<void(*)(XS*,UINT,XS* const*)>(list.Get(),32)(list.Get(),1,&h);
    method<void(*)(XS*,XS*)>(list.Get(),25)(list.Get(),pipeline.Get());
    method<void(*)(XS*,ID3D12RootSignature*)>(list.Get(),33)(list.Get(),rs.Get());
    auto table=method<void(*)(XS*,UINT,D3D12_GPU_DESCRIPTOR_HANDLE)>(list.Get(),35);
    table(list.Get(),0,{addr});table(list.Get(),1,{addr+stride});
    ComPtr<ID3D12CommandSignature> dispatch;
    if(indirect) {
        D3D12_INDIRECT_ARGUMENT_DESC arg{};arg.Type=static_cast<D3D12_INDIRECT_ARGUMENT_TYPE>(0x69); // XS DISPATCH
        D3D12_COMMAND_SIGNATURE_DESC desc{sizeof(D3D12_DISPATCH_ARGUMENTS),1,&arg,0};
        check(method<HRESULT(*)(XS*,const D3D12_COMMAND_SIGNATURE_DESC*,ID3D12RootSignature*,REFIID,void**)>(r.Get(),41)(
            r.Get(),&desc,nullptr,IID_PPV_ARGS(&dispatch)));
        method<void(*)(XS*,ID3D12CommandSignature*,UINT,ID3D12Resource*,UINT64,ID3D12Resource*,UINT64)>(list.Get(),59)(
            list.Get(),dispatch.Get(),2,upload.Get(),64,upload.Get(),128);
        transition(D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        copy(list.Get(),read.Get(),total,gpu.Get(),0,total);
        transition(D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    method<void(*)(XS*,UINT,UINT,UINT)>(list.Get(),14)(list.Get(),2,1,1);
    transition(D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    copy(list.Get(),read.Get(),0,gpu.Get(),0,total);
    check(method<HRESULT(*)(XS*)>(list.Get(),9)(list.Get()));
    auto* l=list.Get();method<void(*)(XS*,UINT,XS* const*)>(queue.Get(),10)(queue.Get(),1,&l);
    ComPtr<ID3D12Fence> fence;
    check(method<HRESULT(*)(XS*,UINT64,D3D12_FENCE_FLAGS,REFIID,void**)>(r.Get(),36)(r.Get(),0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    check(method<HRESULT(*)(XS*,ID3D12Fence*,UINT64,UINT)>(queue.Get(),23)(queue.Get(),fence.Get(),1,0));
    check(fence->SetEventOnCompletion(1,nullptr));
    check(read->Map(0,nullptr,reinterpret_cast<void**>(&ptr)));
    unsigned errors=0,count_errors=0;
    for(UINT i=0;i<count;++i) for(UINT j=0;j<words;++j) {
        UINT expected=0xc0decafe;
        if(indirect) {if(i<stop && j==1)expected=0;}
        else if(i>=7 && i<stop)expected=values[j];
        errors+=ptr[words*i+j]!=expected;
        if(indirect)count_errors+=ptr[total/4+words*i+j]!=(i<std::min(group,stop) && j==1?0u:0xc0decafeu);
    }
    read->Unmap(0,nullptr);
    std::printf("%s: %u/%u words correct, including untouched sentinels\n",indirect?"ClearIndirectArgInstanceCountCS":"ClearCS",
        words*count-errors,words*count);
    if(indirect)std::printf("XS counted ExecuteIndirect: %u/%u words correct\n",words*count-count_errors,words*count);
    return errors||count_errors?1:0;
} catch(const std::exception& e) { std::fprintf(stderr,"frosty-clear: %s\n",e.what());return 1; }
