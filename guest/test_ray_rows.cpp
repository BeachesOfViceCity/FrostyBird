// A GPU producer updates the same instance buffer twice. Only the phase
// constant is rebound after interop: loss of the original PSO, root or
// UAV binding corrupts the second snapshot. Include inactive instances,
// nonopaque rows, full 24-bit SBT bases and a partial final workgroup.
#define NOMINMAX
#include "ray_rows.hpp"
#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
static void ok(HRESULT hr){if(FAILED(hr)){std::fprintf(stderr,"row check HRESULT %#lx\n",(unsigned long)hr);throw std::runtime_error("GPU call");}}
int main()try{
    constexpr UINT count=128*67+21,offset=256;
    ComPtr<ID3D12Device> d;ok(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&d)));
    RayRowPacker pack;if(!pack.init(d.Get())){std::fputs("Vulkan buffer-address row packer unavailable\n",stderr);return 77;}
    constexpr char kernel[]=R"(
RWByteAddressBuffer rows:register(u0);cbuffer Config:register(b0){uint count,phase,offset;}
[numthreads(64,1,1)] void main(uint i:SV_DispatchThreadID){
    if(i>=count)return;
    uint id=i%251u,mask=((i/128u)%9==1 || i%41u==0u)?0u:(1u<<(i%8u));
    uint base=(i/128u)%3u==0u?0xffffffu:(i/7u*1009u+phase*43u)&0xffffffu;
    uint flags=(i/128u)%3u==0u?4u:((i/17u+phase)&15u);
    rows.Store2(offset+i*64u+48u,uint2(id|(mask<<24u),base|(flags<<24u)));
})";
    D3D12_ROOT_PARAMETER params[2]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[1].Constants={0,0,3};
    D3D12_ROOT_SIGNATURE_DESC rd{2,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> root_blob,code,error;
    ok(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&root_blob,nullptr));
    auto hr=D3DCompile(kernel,sizeof(kernel)-1,nullptr,nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
    if(FAILED(hr) && error)std::fputs(static_cast<const char*>(error->GetBufferPointer()),stderr);
    ok(hr);
    ComPtr<ID3D12RootSignature> root;ok(d->CreateRootSignature(0,root_blob->GetBufferPointer(),root_blob->GetBufferSize(),IID_PPV_ARGS(&root)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};ps.pRootSignature=root.Get();ps.CS={code->GetBufferPointer(),code->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pipeline;ok(d->CreateComputePipelineState(&ps,IID_PPV_ARGS(&pipeline)));
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_BUFFER,0,offset+64*count,1,1,1,DXGI_FORMAT_UNKNOWN,{1,0},D3D12_TEXTURE_LAYOUT_ROW_MAJOR,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;hp.CreationNodeMask=hp.VisibleNodeMask=1;
    ComPtr<ID3D12Resource> src;ok(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&src)));
    for(auto type:{D3D12_COMMAND_LIST_TYPE_DIRECT,D3D12_COMMAND_LIST_TYPE_COMPUTE}){
        D3D12_COMMAND_QUEUE_DESC qd{};qd.Type=type;ComPtr<ID3D12CommandQueue> q;ok(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)));
        ComPtr<ID3D12CommandAllocator> a;ok(d->CreateCommandAllocator(type,IID_PPV_ARGS(&a)));
        ComPtr<ID3D12GraphicsCommandList> l;ok(d->CreateCommandList(0,type,a.Get(),pipeline.Get(),IID_PPV_ARGS(&l)));
        l->SetComputeRootSignature(root.Get());l->SetComputeRootUnorderedAccessView(0,src->GetGPUVirtualAddress());
        UINT initial[]{count,0,offset};l->SetComputeRoot32BitConstants(1,3,initial,0);
        std::array<std::shared_ptr<RayRowBuffers>,2> slots;
        if(pack.record(d.Get(),l.Get(),src.Get(),offset,count+1))return 2;
        for(UINT phase=0;phase<2;++phase){
            l->SetComputeRoot32BitConstant(1,phase,1);l->Dispatch((count+63)/64,1,1);
            D3D12_RESOURCE_BARRIER b{};b.Transition={src.Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
            l->ResourceBarrier(1,&b);slots[phase]=pack.record(d.Get(),l.Get(),src.Get(),offset,count);if(!slots[phase])return 3;
            std::swap(b.Transition.StateBefore,b.Transition.StateAfter);l->ResourceBarrier(1,&b);
        }
        if(slots[0]==slots[1])return 4;
        ok(l->Close());ID3D12CommandList* lists[]{l.Get()};q->ExecuteCommandLists(1,lists);
        ComPtr<ID3D12Fence> f;ok(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f)));ok(q->Signal(f.Get(),1));ok(f->SetEventOnCompletion(1,nullptr));
        for(UINT phase=0;phase<2;++phase){
            auto n=RayRowPacker::bytes(count);D3D12_RANGE range{0,SIZE_T(n)};UINT* p{};ok(slots[phase]->readback->Map(0,&range,reinterpret_cast<void**>(&p)));
            std::vector<UINT> got,expected;
            bool valid=RayRowPacker::visit({p,SIZE_T(n)/4},count,[&](UINT base,bool opaque){got.push_back(base|(!opaque?0x1000000u:0u));});
            D3D12_RANGE none{};slots[phase]->readback->Unmap(0,&none);
            UINT last=~0u;for(UINT i=0;i<count;++i){
                UINT group=i/128;if(!(i%128))last=~0u;
                UINT base=group%3==0?0xffffffu:(i/7*1009u+phase*43u)&0xffffffu;
                UINT flags=group%3==0?4u:(i/17+phase)&15u;
                UINT t=group%9==1 || !(i%41)?~0u:base|((flags&12)==4?0u:0x1000000u);
                if(t!=~0u && t!=last)expected.push_back(t);
                last=t;
            }
            std::sort(got.begin(),got.end());std::sort(expected.begin(),expected.end());
            if(!valid || got!=expected){std::fprintf(stderr,"q%u revision %u: got %zu expected %zu tokens\n",UINT(type),phase,got.size(),expected.size());return 5;}
            std::printf("q%u revision %u: %u rows, %zu exact run tokens with opacity, %llu fenced bytes (was %u)\n",UINT(type),phase,count,got.size(),n,count*64);
        }
    }
    return 0;
}catch(const std::exception& e){std::fprintf(stderr,"ray-rows-check: %s\n",e.what());return 1;}
