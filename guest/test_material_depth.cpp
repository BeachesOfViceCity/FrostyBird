// Draw a lifted, UAV-writing retail material against a partially occluding
// depth surface. The two programs differ only in the original ordering bit.
#define NOMINMAX
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <source_location>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
using R=ComPtr<ID3D12Resource>;
static void ok(HRESULT hr,std::source_location where=std::source_location::current()){if(FAILED(hr)){
    std::fprintf(stderr,"depth-check line %u HRESULT %#lx\n",unsigned(where.line()),(unsigned long)hr);throw std::runtime_error("GPU call");}}
int main(int argc,char** argv)try{
    if(argc!=2)return 2;
    constexpr UINT w=256,h=128,n=w*h,rounds=5,colors=4;
    ComPtr<ID3D12Device> d;ok(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&d)));
    auto make=[&](D3D12_HEAP_TYPE heap,D3D12_RESOURCE_DESC desc,D3D12_RESOURCE_STATES st){
        D3D12_HEAP_PROPERTIES p{};p.Type=heap;p.CreationNodeMask=p.VisibleNodeMask=1;
        R r;ok(d->CreateCommittedResource(&p,D3D12_HEAP_FLAG_NONE,&desc,st,nullptr,IID_PPV_ARGS(&r)));return r;};
    auto buf=[&](D3D12_HEAP_TYPE heap,UINT64 bytes,D3D12_RESOURCE_STATES st,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){
        return make(heap,{D3D12_RESOURCE_DIMENSION_BUFFER,0,bytes,1,1,1,DXGI_FORMAT_UNKNOWN,{1,0},D3D12_TEXTURE_LAYOUT_ROW_MAJOR,flags},st);};
    auto heap=[&](D3D12_DESCRIPTOR_HEAP_TYPE type,UINT count,bool visible=false){
        ComPtr<ID3D12DescriptorHeap> s;D3D12_DESCRIPTOR_HEAP_DESC hd{type,count,visible?D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE:D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
        ok(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&s)));return s;};
    auto cb=buf(D3D12_HEAP_TYPE_UPLOAD,65536,D3D12_RESOURCE_STATE_GENERIC_READ);void* mapped{};
    ok(cb->Map(0,nullptr,&mapped));std::memset(mapped,0,65536);cb->Unmap(0,nullptr);
    auto desc=heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,66,true);
    auto dh=[&](UINT i){auto h=desc->GetCPUDescriptorHandleForHeapStart();h.ptr+=i*d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);return h;};
    for(UINT i=0;i<61;++i){
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};v.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
        v.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;v.Texture2D.MipLevels=1;
        if(!i){v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE3D;v.Texture3D.MipLevels=1;}
        if(i==1||i==2||i==7||i==8){
            v.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;v.Buffer={};v.Buffer.NumElements=1;
            if(i<3){v.Format=DXGI_FORMAT_UNKNOWN;v.Buffer.StructureByteStride=16;}
            else{v.Format=DXGI_FORMAT_R32_TYPELESS;v.Buffer.Flags=D3D12_BUFFER_SRV_FLAG_RAW;}}
        d->CreateShaderResourceView(nullptr,&v,dh(i));
    }
    for(UINT i=0;i<4;++i){D3D12_CONSTANT_BUFFER_VIEW_DESC v{cb->GetGPUVirtualAddress(),65536};d->CreateConstantBufferView(&v,dh(61+i));}
    D3D12_UNORDERED_ACCESS_VIEW_DESC v{};v.Format=DXGI_FORMAT_R32_FLOAT;v.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;v.Buffer.NumElements=1;
    d->CreateUnorderedAccessView(nullptr,nullptr,&v,dh(65));
    D3D12_DESCRIPTOR_RANGE ranges[]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,61,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_CBV,4,0,0,61},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,65}};
    D3D12_ROOT_PARAMETER params[2]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable={3,ranges};
    params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;params[1].Descriptor.ShaderRegister=1;
    D3D12_STATIC_SAMPLER_DESC samp[6]{};for(UINT i=0;i<6;++i){auto& s=samp[i];s.ShaderRegister=i;
        s.AddressU=s.AddressV=s.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;s.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;
        s.MaxAnisotropy=1;s.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;s.MaxLOD=D3D12_FLOAT32_MAX;}
    D3D12_ROOT_SIGNATURE_DESC rd{2,params,6,samp,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> b;ok(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&b,nullptr));
    ComPtr<ID3D12RootSignature> root;ok(d->CreateRootSignature(0,b->GetBufferPointer(),b->GetBufferSize(),IID_PPV_ARGS(&root)));
    auto bytes=[&](const char* name){std::ifstream f(std::filesystem::path(argv[1])/name,std::ios::binary);
        if(!f)throw std::runtime_error("missing material fixture");
        return std::vector<char>(std::istreambuf_iterator<char>(f),{});};
    auto vs=bytes("triangle.dxil");std::array<ComPtr<ID3D12PipelineState>,2> states;
    for(UINT i=0;i<2;++i){auto ps=bytes(i?"early.dxil":"late.dxil");D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
        p.pRootSignature=root.Get();p.VS={vs.data(),vs.size()};p.PS={ps.data(),ps.size()};p.SampleMask=UINT_MAX;p.SampleDesc.Count=1;
        p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;
        p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;p.RasterizerState.DepthClipEnable=TRUE;p.DepthStencilState.DepthEnable=TRUE;
        p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_EQUAL;p.DSVFormat=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;p.NumRenderTargets=colors;
        for(UINT j=0;j<colors;++j){p.RTVFormats[j]=DXGI_FORMAT_R32G32B32A32_FLOAT;p.BlendState.RenderTarget[j].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;}
        ok(d->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&states[i])));
    }
    auto witness=buf(D3D12_HEAP_TYPE_DEFAULT,4*n,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto seed=buf(D3D12_HEAP_TYPE_UPLOAD,4*n,D3D12_RESOURCE_STATE_GENERIC_READ);ok(seed->Map(0,nullptr,&mapped));std::memset(mapped,0xa5,4*n);seed->Unmap(0,nullptr);
    constexpr UINT capture=4*n+colors*16*n;auto read=buf(D3D12_HEAP_TYPE_READBACK,2*capture,D3D12_RESOURCE_STATE_COPY_DEST);
    auto targets=heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV,colors),depths=heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV,1);
    auto rt=targets->GetCPUDescriptorHandleForHeapStart(),ds=depths->GetCPUDescriptorHandleForHeapStart();
    D3D12_RESOURCE_DESC tex{D3D12_RESOURCE_DIMENSION_TEXTURE2D,0,w,h,1,1,DXGI_FORMAT_D32_FLOAT_S8X24_UINT,{1,0},D3D12_TEXTURE_LAYOUT_UNKNOWN,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
    auto z=make(D3D12_HEAP_TYPE_DEFAULT,tex,D3D12_RESOURCE_STATE_DEPTH_WRITE);d->CreateDepthStencilView(z.Get(),nullptr,ds);
    std::array<R,colors> color;std::array<D3D12_CPU_DESCRIPTOR_HANDLE,colors> ht{};
    tex.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;tex.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    for(UINT i=0;i<colors;++i){color[i]=make(D3D12_HEAP_TYPE_DEFAULT,tex,D3D12_RESOURCE_STATE_RENDER_TARGET);
        ht[i]=rt;ht[i].ptr+=i*d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);d->CreateRenderTargetView(color[i].Get(),nullptr,ht[i]);}
    D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP,4*rounds,0};ComPtr<ID3D12QueryHeap> stamps;ok(d->CreateQueryHeap(&qd,IID_PPV_ARGS(&stamps)));
    auto clock=buf(D3D12_HEAP_TYPE_READBACK,8*qd.Count,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_COMMAND_QUEUE_DESC cq{};ComPtr<ID3D12CommandQueue> q;ok(d->CreateCommandQueue(&cq,IID_PPV_ARGS(&q)));
    UINT64 hz{},seq{};ok(q->GetTimestampFrequency(&hz));ComPtr<ID3D12Fence> fence;ok(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    for(UINT visible:{w,w/8,0u}){
        ComPtr<ID3D12CommandAllocator> a;ok(d->CreateCommandAllocator(cq.Type,IID_PPV_ARGS(&a)));ComPtr<ID3D12GraphicsCommandList> l;
        ok(d->CreateCommandList(0,cq.Type,a.Get(),nullptr,IID_PPV_ARGS(&l)));
        auto move=[&](ID3D12Resource* obj,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){D3D12_RESOURCE_BARRIER t{};
            t.Transition={obj,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};l->ResourceBarrier(1,&t);};
        D3D12_RECT sc{0,0,w,h},occluded{LONG(visible),0,w,h};D3D12_VIEWPORT vp{0,0,float(w),float(h),0,1};
        l->RSSetViewports(1,&vp);l->RSSetScissorRects(1,&sc);l->ClearDepthStencilView(ds,D3D12_CLEAR_FLAG_DEPTH,.5f,0,0,nullptr);
        if(visible<w)l->ClearDepthStencilView(ds,D3D12_CLEAR_FLAG_DEPTH,0,0,1,&occluded);
        l->SetGraphicsRootSignature(root.Get());ID3D12DescriptorHeap* heaps[]{desc.Get()};l->SetDescriptorHeaps(1,heaps);
        l->SetGraphicsRootDescriptorTable(0,desc->GetGPUDescriptorHandleForHeapStart());l->SetGraphicsRootUnorderedAccessView(1,witness->GetGPUVirtualAddress());
        l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);l->OMSetRenderTargets(colors,ht.data(),FALSE,&ds);
        for(UINT j=0;j<rounds;++j)for(UINT k=0;k<2;++k){auto i=k^(j&1),t=4*j+2*i;
            l->CopyBufferRegion(witness.Get(),0,seed.Get(),0,4*n);move(witness.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const float clear[]{-1,-2,-3,-4};for(auto view:ht)l->ClearRenderTargetView(view,clear,0,nullptr);
            l->SetPipelineState(states[i].Get());l->EndQuery(stamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,t);l->DrawInstanced(3,1,0,0);l->EndQuery(stamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,t+1);
            move(witness.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
            if(j==rounds-1){
                l->CopyBufferRegion(read.Get(),i*capture,witness.Get(),0,4*n);
                for(UINT c=0;c<colors;++c){move(color[c].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
                    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=color[c].Get();dst.pResource=read.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    dst.PlacedFootprint={i*capture+4*n+c*16*n,{tex.Format,w,h,1,w*16}};
                    l->CopyTextureRegion(&dst,0,0,0,&src,nullptr);move(color[c].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);}
            }
            move(witness.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        }
        l->ResolveQueryData(stamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,qd.Count,clock.Get(),0);ok(l->Close());
        ID3D12CommandList* lists[]{l.Get()};q->ExecuteCommandLists(1,lists);ok(q->Signal(fence.Get(),++seq));ok(fence->SetEventOnCompletion(seq,nullptr));
        ok(read->Map(0,nullptr,&mapped));auto* u=static_cast<UINT*>(mapped);UINT wrong=0,bad_mrt=0;
        for(UINT i=0;i<n;++i){auto expected=0x2145df33u+i;wrong+=u[i]!=expected;wrong+=u[capture/4+i]!=(i%w<visible?expected:0xa5a5a5a5u);}
        for(UINT i=n;i<capture/4;++i)bad_mrt+=u[i]!=u[i+capture/4];
        D3D12_RANGE none{};read->Unmap(0,&none);ok(clock->Map(0,nullptr,&mapped));auto* ticks=static_cast<UINT64*>(mapped);double ms[2];
        for(UINT i=0;i<2;++i){std::array<UINT64,rounds-2> dt;for(UINT j=2;j<rounds;++j){auto t=4*j+2*i;dt[j-2]=ticks[t+1]-ticks[t];}
            std::sort(dt.begin(),dt.end());ms[i]=1000.*dt[dt.size()/2]/hz;}clock->Unmap(0,&none);
        std::printf("%u/%u passing pixels: %u/%u UAV checks, %u/%u identical MRT words, late/early %.3f/%.3f ms\n",visible*h,n,2*n-wrong,2*n,4*colors*n-bad_mrt,4*colors*n,ms[0],ms[1]);
        if(wrong||bad_mrt)return 3;
    }
    return 0;
}catch(const std::exception& e){std::fprintf(stderr,"material-depth: %s\n",e.what());return 1;}
