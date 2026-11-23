// Run two lifts of the original indirect-material VS as vertex shaders.
// Stream output observes every export before clipping or rasterization.
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
static void ok(HRESULT hr){if(FAILED(hr)){std::fprintf(stderr,"vertex-pair HRESULT %#lx\n",(unsigned long)hr);throw std::runtime_error("GPU call");}}

int main(int argc,char** argv)try{
    if(argc!=2)return 2;
    const bool rect=std::filesystem::is_regular_file(std::filesystem::path(argv[1])/"reference.rect.dxil");
    constexpr UINT vertices=4,instances=1024,words=11,stride=4*words,grid=64,rounds=5;
    constexpr UINT dim=grid*4;
    const UINT64 bytes=rect?UINT64(dim)*dim*16:UINT64(vertices)*instances*stride,total=bytes+(rect?0:8);
    ComPtr<ID3D12Device> d;ok(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d)));
    auto buffer=[&](UINT64 n,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state){
        D3D12_HEAP_PROPERTIES h{};h.Type=type;h.CreationNodeMask=h.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC b{D3D12_RESOURCE_DIMENSION_BUFFER,0,n,1,1,1,DXGI_FORMAT_UNKNOWN,{1,0},D3D12_TEXTURE_LAYOUT_ROW_MAJOR,D3D12_RESOURCE_FLAG_NONE};
        ComPtr<ID3D12Resource> r;ok(d->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&b,state,nullptr,IID_PPV_ARGS(&r)));return r;
    };
    D3D12_ROOT_PARAMETER params[3]{};
    for(UINT i=0;i<3;++i){params[i].ParameterType=i<2?D3D12_ROOT_PARAMETER_TYPE_CBV:D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[i].Descriptor.ShaderRegister=i==1;params[i].ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;}
    D3D12_ROOT_SIGNATURE_DESC rd{3,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT|D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT};
    ComPtr<ID3DBlob> blob;ok(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,nullptr));ComPtr<ID3D12RootSignature> root;
    ok(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
    D3D12_SO_DECLARATION_ENTRY so[]{{0,"SV_Position",0,0,4,0},{0,"TEXCOORD",3,0,4,0},{0,"LIGHTMAP_ID",0,0,1,0},
        {0,"PACKED_EYE_INDEX",0,0,1,0},{0,"PACKED_VIEW_INDEX",0,0,1,0}};
    D3D12_INPUT_ELEMENT_DESC input{"ATTRIBUTE",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0};
    std::vector<char> pixel;
    if(rect){std::ifstream f("tile-ps.dxil",std::ios::binary);pixel.assign(std::istreambuf_iterator<char>(f),{});
        if(pixel.size()<32)throw std::runtime_error("missing rectangle fixture PS");}
    std::array<ComPtr<ID3D12PipelineState>,2> states;
    for(UINT i=0;i<2;++i){
        std::ifstream f(std::filesystem::path(argv[1])/(i?"candidate.dxil":"reference.dxil"),std::ios::binary);
        std::vector<char> code{std::istreambuf_iterator<char>(f),{}};if(code.size()<32)throw std::runtime_error("missing translated VS");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC ps{};ps.pRootSignature=root.Get();ps.VS={code.data(),code.size()};
        std::vector<char> gs;
        if(rect){
            std::ifstream f(std::filesystem::path(argv[1])/(i?"candidate.rect.dxil":"reference.rect.dxil"),std::ios::binary);
            gs.assign(std::istreambuf_iterator<char>(f),{});if(gs.size()<32)throw std::runtime_error("missing translated rectangle GS");
            ps.GS={gs.data(),gs.size()};
            ps.PS={pixel.data(),pixel.size()};ps.NumRenderTargets=1;ps.RTVFormats[0]=DXGI_FORMAT_R32G32B32A32_UINT;
            ps.RasterizerState.DepthClipEnable=TRUE;ps.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        }
        else ps.StreamOutput={so,UINT(std::size(so)),&stride,1,D3D12_SO_NO_RASTERIZED_STREAM};
        ps.InputLayout={&input,1};
        ps.PrimitiveTopologyType=rect?D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE:D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;ps.SampleMask=UINT_MAX;ps.SampleDesc.Count=1;
        ps.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;ps.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        ok(d->CreateGraphicsPipelineState(&ps,IID_PPV_ARGS(&states[i])));
    }
    auto data=buffer(4096,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    ComPtr<ID3D12Resource> capture;ComPtr<ID3D12DescriptorHeap> rtv_heap;D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    if(rect){D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D,0,dim,dim,1,1,DXGI_FORMAT_R32G32B32A32_UINT,{1,0},D3D12_TEXTURE_LAYOUT_UNKNOWN,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;hp.CreationNodeMask=hp.VisibleNodeMask=1;
        ok(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&capture)));
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};ok(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv_heap)));
        rtv=rtv_heap->GetCPUDescriptorHandleForHeapStart();d->CreateRenderTargetView(capture.Get(),nullptr,rtv);
    }else capture=buffer(total,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
    auto rb=buffer(2*total,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_QUERY_HEAP_DESC qh{D3D12_QUERY_HEAP_TYPE_TIMESTAMP,rounds*4,0};ComPtr<ID3D12QueryHeap> stamps;ok(d->CreateQueryHeap(&qh,IID_PPV_ARGS(&stamps)));
    auto clocks=buffer(8*qh.Count,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_COMMAND_QUEUE_DESC qd{};ComPtr<ID3D12CommandQueue> q;ok(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)));UINT64 hz{};ok(q->GetTimestampFrequency(&hz));if(!hz)return 1;
    ComPtr<ID3D12Fence> fence;ok(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));UINT seq=0;
    for(UINT n:{0u,4u,64u,128u}){
        // Two material rows. Cover empty words, both edge bits, dense
        // words, different ranks within one wave, and an exhausted scan.
        std::vector<UINT> tiles;UINT* u{};ok(data->Map(0,nullptr,reinterpret_cast<void**>(&u)));std::fill_n(u,1024,0u);
        u[17]=std::bit_cast<UINT>(.25f);u[18]=1;u[19]=n;
        u[64+5]=u[64+6]=grid;u[64+12]=u[64+13]=std::bit_cast<UINT>(1.f);
        for(UINT i=0;i<2*n;++i){
            UINT v=i%5==0?0u:i%5==1?0x80000001u:i%5==2?0xffffffffu:(0x174d35a9u*(i+1))^(i<<6);
            u[128+i]=v;if(i>=n)for(UINT k=0;k<32;++k)if(v&(1u<<k))tiles.push_back(32*(i-n)+k);
        }
        for(UINT v=0;v<vertices;++v){auto corner=rect?(v+n)%3:v;
            u[768+2*v]=std::bit_cast<UINT>(float(corner&1));u[769+2*v]=std::bit_cast<UINT>(float(corner>>1));}
        data->Unmap(0,nullptr);
        ComPtr<ID3D12CommandAllocator> a;ok(d->CreateCommandAllocator(qd.Type,IID_PPV_ARGS(&a)));
        ComPtr<ID3D12GraphicsCommandList> l;ok(d->CreateCommandList(0,qd.Type,a.Get(),nullptr,IID_PPV_ARGS(&l)));
        l->SetGraphicsRootSignature(root.Get());auto va=data->GetGPUVirtualAddress();
        l->SetGraphicsRootConstantBufferView(0,va);l->SetGraphicsRootConstantBufferView(1,va+256);l->SetGraphicsRootShaderResourceView(2,va+512);
        D3D12_VERTEX_BUFFER_VIEW vb{va+3072,vertices*8,8};l->IASetVertexBuffers(0,1,&vb);
        l->IASetPrimitiveTopology(rect?D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
        auto transition=[&](D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){D3D12_RESOURCE_BARRIER b{};b.Transition={capture.Get(),0,from,to};l->ResourceBarrier(1,&b);};
        auto dest=rect?0:capture->GetGPUVirtualAddress();D3D12_STREAM_OUTPUT_BUFFER_VIEW view{dest,bytes,dest+bytes};
        UINT draws=rect?std::min(instances,std::max(1u,UINT(tiles.size()))):instances;
        if(rect){D3D12_RECT sc{0,0,dim,dim};D3D12_VIEWPORT vp{0,0,float(dim),float(dim),0,1};
            l->RSSetViewports(1,&vp);l->RSSetScissorRects(1,&sc);l->OMSetRenderTargets(1,&rtv,FALSE,nullptr);}
        for(UINT r=0;r<rounds;++r)for(UINT j=0;j<2;++j){
            UINT i=j^(r&1),t=4*r+2*i;
            if(rect){FLOAT clear[4]{};l->ClearRenderTargetView(rtv,clear,0,nullptr);}
            else {l->CopyBufferRegion(capture.Get(),bytes,data.Get(),4000,8);
                transition(D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_STREAM_OUT);l->SOSetTargets(0,1,&view);}
            l->SetPipelineState(states[i].Get());l->EndQuery(stamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,t);
            l->DrawInstanced(rect?3:vertices,draws,0,0);l->EndQuery(stamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,t+1);
            if(rect){
                if(r+1==rounds){transition(D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
                    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=capture.Get();
                    dst.pResource=rb.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    dst.PlacedFootprint={i*total,{DXGI_FORMAT_R32G32B32A32_UINT,dim,dim,1,16*dim}};
                    l->CopyTextureRegion(&dst,0,0,0,&src,nullptr);transition(D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);}
                continue;
            }
            l->SOSetTargets(0,0,nullptr);
            if(r+1==rounds){transition(D3D12_RESOURCE_STATE_STREAM_OUT,D3D12_RESOURCE_STATE_COPY_SOURCE);
                l->CopyBufferRegion(rb.Get(),i*total,capture.Get(),0,total);transition(D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);}
            else transition(D3D12_RESOURCE_STATE_STREAM_OUT,D3D12_RESOURCE_STATE_COPY_DEST);
        }
        l->ResolveQueryData(stamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,qh.Count,clocks.Get(),0);ok(l->Close());
        ID3D12CommandList* lists[]{l.Get()};q->ExecuteCommandLists(1,lists);ok(q->Signal(fence.Get(),++seq));ok(fence->SetEventOnCompletion(seq,nullptr));
        UINT* v{};ok(rb->Map(0,nullptr,reinterpret_cast<void**>(&v)));UINT bad=0,oracle=0,checked=0;
        if(rect){
            std::array<bool,grid*grid> covered{};for(UINT j=0;j<draws;++j)covered[n?tiles.at(j):0]=true;
            for(UINT y=0;y<dim;++y)for(UINT x=0;x<dim;++x){auto k=dim*y+x;std::array<UINT,4> expected{};
                if(covered[(y/4)*grid+x/4])expected={std::bit_cast<UINT>(.25f),0,0,k+1};
                for(UINT j=0;j<4;++j){auto a=v[4*k+j];bad+=a!=v[total/4+4*k+j];oracle+=a!=expected[j];++checked;}}
        }else{
         for(UINT i=0;i<2;++i){UINT64 fill{};std::memcpy(&fill,v+(i*total+bytes)/4,8);if(fill!=bytes)throw std::runtime_error("incomplete stream output");}
         for(UINT k=0;k<vertices*instances;++k){
            auto p=v+k*words;for(UINT j=0;j<words;++j)bad+=p[j]!=p[j+total/4];
            if(n && k/vertices>=tiles.size())continue;
            auto tile=n?tiles.at(k/vertices):0u,corner=k%vertices;
            std::array<float,words> expected{2.f*(float(tile%grid)+float(corner&1))/grid-1.f,1.f-2.f*(float(tile/grid)+float(corner>>1))/grid,.25f,1.f};
            for(UINT j=0;j<words;++j)oracle+=std::bit_cast<UINT>(expected[j])!=p[j];
            checked+=words;
         }}
        D3D12_RANGE none{};rb->Unmap(0,&none);UINT64* t{};ok(clocks->Map(0,nullptr,reinterpret_cast<void**>(&t)));
        double ms[2]{};for(UINT i=0;i<2;++i){std::array<UINT64,rounds-2> dt{};
            for(UINT r=2;r<rounds;++r)dt[r-2]=t[4*r+2*i+1]-t[4*r+2*i];
            std::sort(dt.begin(),dt.end());ms[i]=1000.*dt[dt.size()/2]/hz;}
        clocks->Unmap(0,&none);
        std::printf("%u words/row: %u/%u equal %s words, %u/%u independent tile checks, %.3f/%.3f ms\n",
            n,UINT(bytes/4)-bad,UINT(bytes/4),rect?"raster":"export",checked-oracle,checked,ms[0],ms[1]);if(bad || oracle || !checked)return 1;
    }
    return 0;
}catch(const std::exception& e){std::fprintf(stderr,"vertex-pair: %s\n",e.what());return 1;}
