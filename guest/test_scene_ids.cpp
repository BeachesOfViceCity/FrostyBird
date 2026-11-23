// GPUSceneSetInstancePrimitiveIdCS: check a whole guest wave's shared-memory
// prefix computation, the small-list and full-list paths, and untouched data.
#define NOMINMAX
#include "recompiled_shader.hpp"
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace {
void ok(HRESULT hr) {if(hr!=S_OK){std::fprintf(stderr,"GPUScene HRESULT %#lx\n",static_cast<unsigned long>(hr));throw std::runtime_error("device or shader failure");}}
using Entry=std::array<UINT,4>;
}
int main(int argc,char** argv) try {
    if(argc!=2)return 2;
    std::ifstream input(argv[1],std::ios::binary);
    std::vector<char> shader{std::istreambuf_iterator<char>(input),{}};
    if(shader.empty())return 2;
    ComPtr<ID3D12Device> dev;
    ok(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&dev)));
    D3D12_DESCRIPTOR_RANGE ranges[]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_CBV,1,0,0,2},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,3}};
    D3D12_ROOT_PARAMETER par{};par.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    par.DescriptorTable={3,ranges};
    D3D12_ROOT_SIGNATURE_DESC rd{1,&par,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob;ComPtr<ID3D12RootSignature> rs;
    ok(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,nullptr));
    ok(dev->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&rs)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};ps.pRootSignature=rs.Get();ps.CS={shader.data(),shader.size()};
    ComPtr<ID3D12PipelineState> state;
    ok(make_compute_variants(dev.Get(),ps,shader_key(ps.CS),state.GetAddressOf()));

    constexpr UINT groups=4,count=160;
    std::array<UINT,groups> batches{};
    std::vector<std::array<UINT,2>> spans;
    std::array<Entry,count> initial,expected;
    for(UINT i=0;i<count;++i) initial[i]={((i*23u+129u)&4095u)<<20|0x54321u,0x12340000u+i,~i,0x7fc12345u};
    expected=initial;
    auto add=[&](UINT start,UINT len,UINT dst,UINT id) {
        spans.push_back({(dst<<7)|len,(id<<6)|start});
        for(UINT k=0;k<len;++k) expected.at(dst+k)[0]=(initial[dst+k][0]&0xfff00000)|id;
    };
    batches[0]=0; // empty batch
    batches[1]=2;add(0,7,2,0x172);add(41,3,30,0x82);
    batches[2]=UINT(spans.size()<<7)|5;
    add(0,2,34,0x110);add(6,4,40,0x241);add(20,3,50,0x351);add(33,3,54,0x461);add(57,5,62,0x571);
    batches[3]=UINT(spans.size()<<7)|64;
    for(UINT i=0;i<64;++i)add(i,1,80+i,0x38000u+i);

    auto buf=[&](D3D12_HEAP_TYPE heap,UINT bytes,D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES start) {
        D3D12_HEAP_PROPERTIES hp{};hp.Type=heap;hp.CreationNodeMask=hp.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_BUFFER,0,bytes,1,1,1,DXGI_FORMAT_UNKNOWN,{1,0},D3D12_TEXTURE_LAYOUT_ROW_MAJOR,flags};
        ComPtr<ID3D12Resource> r;ok(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,start,nullptr,IID_PPV_ARGS(&r)));return r;
    };
    auto upload=[&](const void* src,UINT size) {
        auto r=buf(D3D12_HEAP_TYPE_UPLOAD,size,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_GENERIC_READ);
        void* p;ok(r->Map(0,nullptr,&p));std::memcpy(p,src,size);r->Unmap(0,nullptr);return r;
    };
    std::array<UINT,64> cb{};cb[4]=groups;
    auto c=upload(cb.data(),sizeof(cb)),b=upload(batches.data(),sizeof(batches));
    auto s=upload(spans.data(),UINT(8*spans.size())),seed=upload(initial.data(),sizeof(initial));
    auto target=buf(D3D12_HEAP_TYPE_DEFAULT,sizeof(initial),D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
    auto read=buf(D3D12_HEAP_TYPE_READBACK,sizeof(initial),D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,4,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    ok(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));
    auto cpu=heap->GetCPUDescriptorHandleForHeapStart();auto step=dev->GetDescriptorHandleIncrementSize(hd.Type);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.StructureByteStride=4;srv.Buffer.NumElements=groups;dev->CreateShaderResourceView(b.Get(),&srv,cpu);cpu.ptr+=step;
    srv.Buffer.StructureByteStride=8;srv.Buffer.NumElements=UINT(spans.size());dev->CreateShaderResourceView(s.Get(),&srv,cpu);cpu.ptr+=step;
    D3D12_CONSTANT_BUFFER_VIEW_DESC cv{c->GetGPUVirtualAddress(),sizeof(cb)};dev->CreateConstantBufferView(&cv,cpu);cpu.ptr+=step;
    D3D12_UNORDERED_ACCESS_VIEW_DESC u{};u.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;u.Buffer.StructureByteStride=16;u.Buffer.NumElements=count;
    dev->CreateUnorderedAccessView(target.Get(),nullptr,&u,cpu);
    ComPtr<ID3D12CommandQueue> q;D3D12_COMMAND_QUEUE_DESC qd{};ok(dev->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)));
    ComPtr<ID3D12CommandAllocator> alloc;ComPtr<ID3D12GraphicsCommandList> list;
    ok(dev->CreateCommandAllocator(qd.Type,IID_PPV_ARGS(&alloc)));
    ok(dev->CreateCommandList(0,qd.Type,alloc.Get(),state.Get(),IID_PPV_ARGS(&list)));
    auto transition=[&](D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){D3D12_RESOURCE_BARRIER bar{};
        bar.Transition={target.Get(),0,from,to};list->ResourceBarrier(1,&bar);};
    list->CopyBufferRegion(target.Get(),0,seed.Get(),0,sizeof(initial));
    transition(D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto* h=heap.Get();list->SetDescriptorHeaps(1,&h);list->SetComputeRootSignature(rs.Get());
    list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch(groups+1,1,1); // extra group takes the out-of-range branch
    transition(D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(read.Get(),0,target.Get(),0,sizeof(initial));ok(list->Close());
    ID3D12CommandList* submit=list.Get();q->ExecuteCommandLists(1,&submit);
    ComPtr<ID3D12Fence> fence;ok(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));ok(q->Signal(fence.Get(),1));
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)return 2;
    auto hr=fence->SetEventOnCompletion(1,event);auto wait=SUCCEEDED(hr)?WaitForSingleObject(event,15000):WAIT_FAILED;
    CloseHandle(event);if(wait!=WAIT_OBJECT_0)throw std::runtime_error("compute fence timeout");
    void* map{};ok(read->Map(0,nullptr,&map));unsigned errors=0;
    for(UINT i=0;i<count;++i)for(UINT j=0;j<4;++j) {
        auto value=static_cast<UINT*>(map)[4*i+j];
        if(value!=expected[i][j]) {
            if(errors++<6)std::fprintf(stderr,"entry[%u].%u got %#x expected %#x\n",i,j,value,expected[i][j]);
        }
    }
    read->Unmap(0,nullptr);
    std::printf("GPUSceneSetInstancePrimitiveIdCS: %u/%u words correct\n",4*count-errors,4*count);return errors?1:0;
} catch(const std::exception& e){std::fprintf(stderr,"frosty-scene-ids: %s\n",e.what());return 1;}
