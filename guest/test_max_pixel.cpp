// Dispatch the packaged tiled luminance reduction. The groups contend on a
// structured-buffer lock and publish the winning pixel, not just its value.
#define NOMINMAX
#include "recompiled_shader.hpp"
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;

namespace {
void check(HRESULT hr) {
    if(hr!=S_OK){std::fprintf(stderr,"max-pixel: HRESULT %#lx\n",static_cast<unsigned long>(hr));throw std::runtime_error("GPU operation failed");}
}
}
int main(int argc,char** argv) try {
    if(argc!=2)return 2;
    std::ifstream file(argv[1],std::ios::binary);
    std::vector<char> shader{std::istreambuf_iterator<char>(file),{}};
    if(shader.empty())return 2;
    ComPtr<ID3D12Device> d;
    check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d)));
    D3D12_DESCRIPTOR_RANGE ranges[]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_CBV,1,0,0,1},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,2}};
    D3D12_ROOT_PARAMETER par{};par.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;par.DescriptorTable={3,ranges};
    D3D12_ROOT_SIGNATURE_DESC rd{1,&par,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> sig;ComPtr<ID3D12RootSignature> root;
    check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&sig,nullptr));
    check(d->CreateRootSignature(0,sig->GetBufferPointer(),sig->GetBufferSize(),IID_PPV_ARGS(&root)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};ps.pRootSignature=root.Get();ps.CS={shader.data(),shader.size()};
    ComPtr<ID3D12PipelineState> state;
    check(make_compute_variants(d.Get(),ps,shader_key(ps.CS),state.GetAddressOf()));

    auto make=[&](D3D12_HEAP_TYPE heap,D3D12_RESOURCE_DESC desc,D3D12_RESOURCE_STATES initial) {
        D3D12_HEAP_PROPERTIES hp{};hp.Type=heap;hp.CreationNodeMask=hp.VisibleNodeMask=1;
        ComPtr<ID3D12Resource> r;
        check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,initial,nullptr,IID_PPV_ARGS(&r)));return r;
    };
    auto buffer=[&](D3D12_HEAP_TYPE heap,UINT64 bytes,D3D12_RESOURCE_STATES initial,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE) {
        return make(heap,{D3D12_RESOURCE_DIMENSION_BUFFER,0,bytes,1,1,1,DXGI_FORMAT_UNKNOWN,{1,0},D3D12_TEXTURE_LAYOUT_ROW_MAJOR,flags},initial);
    };
    constexpr UINT w=24,h=16,valid_w=19,valid_h=13,win_x=17,win_y=10;
    D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D,0,w,h,1,1,DXGI_FORMAT_R32G32B32A32_FLOAT,
                           {1,0},D3D12_TEXTURE_LAYOUT_UNKNOWN,D3D12_RESOURCE_FLAG_NONE};
    auto tex=make(D3D12_HEAP_TYPE_DEFAULT,td,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 total{};
    d->GetCopyableFootprints(&td,0,1,0,&fp,nullptr,nullptr,&total);
    auto upload=buffer(D3D12_HEAP_TYPE_UPLOAD,512+total,D3D12_RESOURCE_STATE_GENERIC_READ);
    std::array<UINT,6> seed{~0u,~0u,0,0,0x45678123,0xacc08101};
    auto dst=buffer(D3D12_HEAP_TYPE_DEFAULT,sizeof(seed),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto rb=buffer(D3D12_HEAP_TYPE_READBACK,sizeof(seed),D3D12_RESOURCE_STATE_COPY_DEST);
    BYTE* bytes{};check(upload->Map(0,nullptr,reinterpret_cast<void**>(&bytes)));
    std::memset(bytes,0,512+total);
    const UINT extent[]{valid_w,valid_h};std::memcpy(bytes,extent,sizeof(extent));std::memcpy(bytes+256,seed.data(),sizeof(seed));
    fp.Offset+=512;
    for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x){
        // The invalid edge is brighter than the winner. Only green contributes
        // luminance, so the winning IEEE word is exact without contraction.
        float green=x>=valid_w || y>=valid_h?8.f:x==win_x && y==win_y?2.f:(x+y*w+1)/512.f;
        std::memcpy(bytes+fp.Offset+y*fp.Footprint.RowPitch+16*x+4,&green,4);
    }
    upload->Unmap(0,nullptr);
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,3,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    check(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));
    auto cpu=heap->GetCPUDescriptorHandleForHeapStart();auto step=d->GetDescriptorHandleIncrementSize(hd.Type);
    d->CreateShaderResourceView(tex.Get(),nullptr,cpu);cpu.ptr+=step;
    D3D12_CONSTANT_BUFFER_VIEW_DESC cb{upload->GetGPUVirtualAddress(),256};d->CreateConstantBufferView(&cb,cpu);cpu.ptr+=step;
    D3D12_UNORDERED_ACCESS_VIEW_DESC u{};u.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;u.Buffer.StructureByteStride=4;u.Buffer.NumElements=seed.size();
    d->CreateUnorderedAccessView(dst.Get(),nullptr,&u,cpu);
    ComPtr<ID3D12CommandQueue> q;D3D12_COMMAND_QUEUE_DESC qd{};check(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)));
    ComPtr<ID3D12CommandAllocator> alloc;ComPtr<ID3D12GraphicsCommandList> list;
    check(d->CreateCommandAllocator(qd.Type,IID_PPV_ARGS(&alloc)));
    check(d->CreateCommandList(0,qd.Type,alloc.Get(),state.Get(),IID_PPV_ARGS(&list)));
    auto transition=[&](ID3D12Resource* r,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){
        D3D12_RESOURCE_BARRIER b{};b.Transition={r,0,from,to};list->ResourceBarrier(1,&b);
    };
    D3D12_TEXTURE_COPY_LOCATION src{},to{};
    src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
    to.pResource=tex.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&to,0,0,0,&src,nullptr);
    list->CopyBufferRegion(dst.Get(),0,upload.Get(),256,sizeof(seed));
    transition(tex.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(dst.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto* hp=heap.Get();list->SetDescriptorHeaps(1,&hp);list->SetComputeRootSignature(root.Get());
    list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch(w/8,h/8,1);
    transition(dst.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(rb.Get(),0,dst.Get(),0,sizeof(seed));check(list->Close());
    ID3D12CommandList* submission=list.Get();q->ExecuteCommandLists(1,&submission);
    ComPtr<ID3D12Fence> fence;check(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));check(q->Signal(fence.Get(),1));
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)return 2;
    auto hr=fence->SetEventOnCompletion(1,event);auto wait=SUCCEEDED(hr)?WaitForSingleObject(event,15000):WAIT_FAILED;
    CloseHandle(event);if(wait!=WAIT_OBJECT_0)throw std::runtime_error("compute fence timeout");
    UINT* data{};check(rb->Map(0,nullptr,reinterpret_cast<void**>(&data)));
    seed[0]=win_x;seed[1]=win_y;seed[2]=std::bit_cast<UINT>(2.f*.59f);
    bool passed=std::equal(seed.begin(),seed.end(),data);
    std::printf("Global[2633]: peak (%u,%u) %#x; lock %#x; tail %#x %#x: %s\n",
                data[0],data[1],data[2],data[3],data[4],data[5],passed?"PASS":"FAIL");
    rb->Unmap(0,nullptr);return passed?0:1;
} catch(const std::exception& e){std::fprintf(stderr,"max-pixel: %s\n",e.what());return 1;}
