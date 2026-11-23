// Compare two translations of the same original 1024-point image convolution.
// Two source textures, the original eight constants, and an ordinary float4
// UAV. Queries time dispatches only; the CPU readback is solely a test oracle.
#define NOMINMAX
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
static void ok(HRESULT hr){if(FAILED(hr)){std::fprintf(stderr,"kernel-pair HRESULT %#lx\n",(unsigned long)hr);throw std::runtime_error("GPU call");}}
int main(int argc,char** argv)try{
    if(argc!=2)return 2;
    constexpr UINT w=1024,lines=96,rounds=8,words=w*w*4,marker=0x4ade15a7;
    constexpr UINT64 bytes=4ull*words;
    ComPtr<ID3D12Device> d;ok(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d)));
    auto resource=[&](D3D12_HEAP_TYPE type,D3D12_RESOURCE_DESC desc,D3D12_RESOURCE_STATES st){
        D3D12_HEAP_PROPERTIES hp{};hp.Type=type;hp.CreationNodeMask=hp.VisibleNodeMask=1;
        ComPtr<ID3D12Resource> r;ok(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,st,nullptr,IID_PPV_ARGS(&r)));return r;
    };
    auto buf=[&](D3D12_HEAP_TYPE t,UINT64 n,D3D12_RESOURCE_STATES s){
        return resource(t,{D3D12_RESOURCE_DIMENSION_BUFFER,0,n,1,1,1,DXGI_FORMAT_UNKNOWN,{1,0},D3D12_TEXTURE_LAYOUT_ROW_MAJOR,D3D12_RESOURCE_FLAG_NONE},s);
    };
    D3D12_DESCRIPTOR_RANGE ranges[]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,2}};
    D3D12_ROOT_PARAMETER params[2]{};params[0].DescriptorTable={2,ranges};params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[1].Constants={0,0,8};
    D3D12_ROOT_SIGNATURE_DESC rd{2,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> blob;
    ok(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,nullptr));ComPtr<ID3D12RootSignature> root;
    ok(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
    std::array<ComPtr<ID3D12PipelineState>,2> states;
    for(UINT i=0;i<2;++i){std::ifstream f(std::filesystem::path(argv[1])/(i?"candidate.dxil":"reference.dxil"),std::ios::binary);
        std::vector<char> code{std::istreambuf_iterator<char>(f),{}};if(code.size()<32)throw std::runtime_error("missing translated shader");
        D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};ps.pRootSignature=root.Get();ps.CS={code.data(),code.size()};ok(d->CreateComputePipelineState(&ps,IID_PPV_ARGS(&states[i])));}
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,6,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    ComPtr<ID3D12DescriptorHeap> heap;ok(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));auto inc=d->GetDescriptorHandleIncrementSize(hd.Type);
    auto cpu=[&](UINT i){auto h=heap->GetCPUDescriptorHandleForHeapStart();h.ptr+=i*inc;return h;};
    D3D12_COMMAND_QUEUE_DESC qd{};ComPtr<ID3D12CommandQueue> q;ok(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)));UINT64 hz{};ok(q->GetTimestampFrequency(&hz));if(!hz)return 1;
    ComPtr<ID3D12Fence> fence;ok(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));UINT64 seq=0;
    D3D12_RESOURCE_DESC tex{D3D12_RESOURCE_DIMENSION_TEXTURE2D,0,w,w,1,1,DXGI_FORMAT_R32G32B32A32_FLOAT,{1,0},D3D12_TEXTURE_LAYOUT_UNKNOWN,D3D12_RESOURCE_FLAG_NONE};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;UINT64 size;d->GetCopyableFootprints(&tex,0,1,0,&fp,nullptr,nullptr,&size);
    if(size!=bytes || fp.Offset || fp.Footprint.RowPitch!=w*16)return 1;
    std::array<ComPtr<ID3D12Resource>,2> src,dst;
    for(auto& r:src)r=resource(D3D12_HEAP_TYPE_DEFAULT,tex,D3D12_RESOURCE_STATE_COPY_DEST);
    tex.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    for(UINT i=0;i<2;++i){
        dst[i]=resource(D3D12_HEAP_TYPE_DEFAULT,tex,D3D12_RESOURCE_STATE_COPY_DEST);
        for(UINT j=0;j<2;++j)d->CreateShaderResourceView(src[j].Get(),nullptr,cpu(3*i+j));
        d->CreateUnorderedAccessView(dst[i].Get(),nullptr,nullptr,cpu(3*i+2));}
    auto upload=buf(D3D12_HEAP_TYPE_UPLOAD,3*bytes,D3D12_RESOURCE_STATE_GENERIC_READ),read=buf(D3D12_HEAP_TYPE_READBACK,2*bytes,D3D12_RESOURCE_STATE_COPY_DEST);
    UINT* u{};ok(upload->Map(0,nullptr,reinterpret_cast<void**>(&u)));
    for(UINT j=0;j<2;++j)for(UINT i=0;i<words;++i){
        UINT h=i*1664525u+257u*j+1013904223u;h^=h>>11;
        u[j*words+i]=std::bit_cast<UINT>(float(int(h&1023)-511)/256.f);}
    std::fill_n(u+2*words,words,marker);upload->Unmap(0,nullptr);
    D3D12_QUERY_HEAP_DESC qh{D3D12_QUERY_HEAP_TYPE_TIMESTAMP,4*rounds,0};ComPtr<ID3D12QueryHeap> queries;ok(d->CreateQueryHeap(&qh,IID_PPV_ARGS(&queries)));
    auto times=buf(D3D12_HEAP_TYPE_READBACK,8*qh.Count,D3D12_RESOURCE_STATE_COPY_DEST);UINT cases=0;
    for(UINT flags:{0u,1u,2u,9u}){
        ComPtr<ID3D12CommandAllocator> a;ok(d->CreateCommandAllocator(qd.Type,IID_PPV_ARGS(&a)));
        ComPtr<ID3D12GraphicsCommandList> l;ok(d->CreateCommandList(0,qd.Type,a.Get(),nullptr,IID_PPV_ARGS(&l)));
        auto transition=[&](ID3D12Resource* r,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){D3D12_RESOURCE_BARRIER b{};
            b.Transition={r,0,from,to};l->ResourceBarrier(1,&b);};
        auto loc=[](ID3D12Resource* r){D3D12_TEXTURE_COPY_LOCATION loc{};loc.pResource=r;return loc;};
        auto linear=[&](ID3D12Resource* r,UINT i){auto b=loc(r);b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint=fp;b.PlacedFootprint.Offset=i*bytes;return b;};
        for(UINT i=0;i<2;++i){
            if(!cases){auto to=loc(src[i].Get()),from=linear(upload.Get(),i);l->CopyTextureRegion(&to,0,0,0,&from,nullptr);
                transition(src[i].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);}
            if(cases)transition(dst[i].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
            auto to=loc(dst[i].Get()),from=linear(upload.Get(),2);l->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            transition(dst[i].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        ID3D12DescriptorHeap* heaps[]{heap.Get()};l->SetDescriptorHeaps(1,heaps);l->SetComputeRootSignature(root.Get());
        UINT constants[]{3,7,w-11,w-9,w-5,w-3,flags,0};l->SetComputeRoot32BitConstants(1,8,constants,0);
        for(UINT r=0;r<rounds;++r)for(UINT j=0;j<2;++j){
            UINT i=j^(r&1),t=4*r+2*i;auto views=heap->GetGPUDescriptorHandleForHeapStart();views.ptr+=3*i*inc;
            l->SetComputeRootDescriptorTable(0,views);l->SetPipelineState(states[i].Get());
            D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;b.UAV.pResource=dst[i].Get();l->ResourceBarrier(1,&b);
            l->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,t);l->Dispatch(1,1,lines);l->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,t+1);}
        l->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,qh.Count,times.Get(),0);
        for(UINT i=0;i<2;++i){transition(dst[i].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
            auto from=loc(dst[i].Get()),to=linear(read.Get(),i);l->CopyTextureRegion(&to,0,0,0,&from,nullptr);}
        ok(l->Close());ID3D12CommandList* lists[]{l.Get()};q->ExecuteCommandLists(1,lists);ok(q->Signal(fence.Get(),++seq));ok(fence->SetEventOnCompletion(seq,nullptr));
        UINT* p{};ok(read->Map(0,nullptr,reinterpret_cast<void**>(&p)));UINT bad=0,changed=0,nonzero=0;
        for(UINT i=0;i<words;++i){bad+=p[i]!=p[i+words];changed+=p[i]!=marker;nonzero+=p[i]!=marker && (p[i]&0x7fffffffu)!=0u;}
        D3D12_RANGE empty{};read->Unmap(0,&empty);UINT64* t{};ok(times->Map(0,nullptr,reinterpret_cast<void**>(&t)));
        std::array<double,2> ms{};for(UINT i=0;i<2;++i){std::array<UINT64,rounds-2> ns;
            for(UINT r=2;r<rounds;++r)ns[r-2]=t[4*r+2*i+1]-t[4*r+2*i];
            std::sort(ns.begin(),ns.end());ms[i]=500.*(ns[ns.size()/2-1]+ns[ns.size()/2])/hz;}times->Unmap(0,&empty);
        std::printf("mode %u: %u changed words, %u nonzero, %u mismatches, reference/candidate %.3f/%.3f ms\n",flags,changed,nonzero,bad,ms[0],ms[1]);
        if(bad || changed<w*lines || nonzero<lines)return 1;
        ++cases;
    }
    return 0;
}catch(const std::exception& e){std::fprintf(stderr,"kernel-pair: %s\n",e.what());return 1;}
