#define NOMINMAX
#include "pipeline_prep.hpp"
#include "recompiled_shader.hpp"
#include "xs_ray.hpp"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <bit>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
static void ok(HRESULT hr){if(FAILED(hr)){std::fprintf(stderr,"pipeline-check: HRESULT %#lx\n",(unsigned long)hr);throw std::runtime_error("GPU call");}}
static bool ray_check(ID3D12Device* device,ID3D12RootSignature* rs,const wchar_t* file){
    ComPtr<ID3D12Device5> dev;ok(device->QueryInterface(IID_PPV_ARGS(&dev)));
    std::ifstream input(std::filesystem::path(file),std::ios::binary);if(!input)throw std::runtime_error("missing ray fixture");
    RayPipelineLibrary lib{L"CheckedEntry",L"ReplayEntry",L"",{std::istreambuf_iterator<char>(input),{}}};
    std::vector<RayPipelineLibrary> libs;libs.push_back(std::move(lib));
    D3D12_ROOT_PARAMETER p{};p.ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;p.Descriptor={0,21301};
    D3D12_ROOT_SIGNATURE_DESC rd{1,&p,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE};ComPtr<ID3DBlob> local;
    ok(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&local,nullptr));
    auto get=[&](PipelinePrep& prep,UINT payload){
        ComPtr<ID3D12StateObject> so;ok(prep.rays(dev.Get(),rs,{payload,8},{1,D3D12_RAYTRACING_PIPELINE_FLAG_NONE},libs,0,
            {static_cast<const BYTE*>(local->GetBufferPointer()),local->GetBufferSize()},so.GetAddressOf()));return so;
    };
    {PipelinePrep cold;get(cold,16);}
    PipelinePrep warm;warm.start(device);warm.join_and_count_hits();get(warm,32);auto so=get(warm,16);
    if(warm.join_and_count_hits()!=1)throw std::runtime_error("expected one exact ray pipeline hit");
    ComPtr<ID3D12StateObjectProperties> props;ok(so.As(&props));auto id=props->GetShaderIdentifier(L"ReplayEntry");
    if(!id)throw std::runtime_error("missing prepared raygen export");
    auto buffer=[&](D3D12_HEAP_TYPE type,UINT64 n,D3D12_RESOURCE_STATES state,bool uav){
        D3D12_HEAP_PROPERTIES hp{};hp.Type=type;hp.CreationNodeMask=hp.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_BUFFER,0,n,1,1,1,DXGI_FORMAT_UNKNOWN,{1,0},D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
            uav?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE};
        ComPtr<ID3D12Resource> b;ok(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&b)));return b;
    };
    auto gpu=buffer(D3D12_HEAP_TYPE_DEFAULT,32,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);
    auto cpu=buffer(D3D12_HEAP_TYPE_READBACK,32,D3D12_RESOURCE_STATE_COPY_DEST,false);
    auto sbt=buffer(D3D12_HEAP_TYPE_UPLOAD,64,D3D12_RESOURCE_STATE_GENERIC_READ,false);
    BYTE* bytes{};ok(sbt->Map(0,nullptr,reinterpret_cast<void**>(&bytes)));std::memset(bytes,0,64);std::memcpy(bytes,id,32);
    auto va=gpu->GetGPUVirtualAddress()+16;std::memcpy(bytes+32,&va,8);sbt->Unmap(0,nullptr);
    D3D12_COMMAND_QUEUE_DESC qd{};ComPtr<ID3D12CommandQueue> q;ok(dev->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)));
    ComPtr<ID3D12CommandAllocator> a;ComPtr<ID3D12GraphicsCommandList4> list;
    ok(dev->CreateCommandAllocator(qd.Type,IID_PPV_ARGS(&a)));ok(dev->CreateCommandList(0,qd.Type,a.Get(),nullptr,IID_PPV_ARGS(&list)));
    list->SetComputeRootSignature(rs);list->SetComputeRootUnorderedAccessView(0,gpu->GetGPUVirtualAddress());list->SetPipelineState1(so.Get());
    D3D12_DISPATCH_RAYS_DESC dispatch{};dispatch.RayGenerationShaderRecord={sbt->GetGPUVirtualAddress(),64};dispatch.Width=4;dispatch.Height=dispatch.Depth=1;
    list->DispatchRays(&dispatch);D3D12_RESOURCE_BARRIER b{};b.Transition={gpu.Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1,&b);list->CopyResource(cpu.Get(),gpu.Get());ok(list->Close());ID3D12CommandList* lists[]{list.Get()};
    q->ExecuteCommandLists(1,lists);ComPtr<ID3D12Fence> f;ok(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f)));
    ok(q->Signal(f.Get(),1));ok(f->SetEventOnCompletion(1,nullptr));
    UINT* data{};ok(cpu->Map(0,nullptr,reinterpret_cast<void**>(&data)));UINT matches=0;
    for(UINT i=0;i<4;++i){matches+=data[i]==103+5*i;matches+=data[4+i]==271-11*i;}
    D3D12_RANGE empty{};cpu->Unmap(0,&empty);
    std::printf("ray pipeline replay: %u/8 GPU words, changed payload missed, exact pipeline consumed\n",matches);return matches==8;
}
int main(int argc,char** argv)try{
    if(argc==2 && std::string(argv[1])=="--caps"){
        ComPtr<ID3D12Device> d;ok(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d)));
        std::printf("frostybird-compute-caps=%u\n",shader_compute_wire(d.Get())>>8);return 0;
    }
    if((argc==2 || argc==3) && std::string_view(argv[1])=="--prepare"){
        unsigned first=0;
        if(argc==3){std::string_view value=argv[2];auto [last,ec]=std::from_chars(value.data(),value.data()+value.size(),first);
            if(ec!=std::errc{} || last!=value.data()+value.size())throw std::runtime_error("invalid pipeline resume offset");}
        ComPtr<ID3D12Device> d;ok(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d)));
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 rt{};ComPtr<ID3D12Device7> d7;
        if(SUCCEEDED(d.As(&d7)) && SUCCEEDED(d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&rt,sizeof(rt))) &&
            rt.RaytracingTier>=D3D12_RAYTRACING_TIER_1_1)xs_enable_position_fetch(d.Get());
        {
            PipelinePrep prep;prep.bake(d.Get(),first);
        }
        if(GetEnvironmentVariableA("FROSTY_INSTALL_PROGRESS",nullptr,0)){
            using Commit=HRESULT (WINAPI*)(ID3D12Device*);
            auto commit=std::bit_cast<Commit>(GetProcAddress(GetModuleHandleW(L"d3d12core.dll"),"VKD3DCommitPipelineCache"));
            if(!commit)throw std::runtime_error("vkd3d-proton is missing synchronous pipeline-cache support");
            ok(commit(d.Get()));
            std::fputs("frostybird/pso-cache-committed\n",stderr);
            std::fflush(stderr);
        }
        return 0;
    }
    wchar_t temp[MAX_PATH]{},path[MAX_PATH]{};
    if(!GetTempPathW(MAX_PATH,temp) || !GetTempFileNameW(temp,L"pso",0,path) || !DeleteFileW(path) ||
       !CreateDirectoryW(path,nullptr) || !SetEnvironmentVariableW(L"FROSTY_PIPELINE_RECIPES",path))return 1;
    struct Cleanup{std::filesystem::path p;~Cleanup(){std::error_code ec;std::filesystem::remove_all(p,ec);}} cleanup{path};
    ComPtr<ID3D12Device> dev;ok(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&dev)));
    D3D12_ROOT_PARAMETER root_par{};root_par.ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rd{1,&root_par,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> serial;ok(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&serial,nullptr));
    auto root=[&](){ComPtr<ID3D12RootSignature> r;ok(create_shader_root(dev.Get(),0,serial->GetBufferPointer(),serial->GetBufferSize(),IID_PPV_ARGS(&r)));return r;};
    std::array<ComPtr<ID3DBlob>,2> codes;
    for(unsigned i=0;i<2;++i){
        auto src=std::string("RWByteAddressBuffer buf:register(u0);[numthreads(1,1,1)] void main(){buf.Store(")+
            std::to_string(4*i)+","+std::to_string(100+i)+");}";
        ok(D3DCompile(src.data(),src.size(),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&codes[i],nullptr));
    }
    auto get=[&](PipelinePrep& prep,ID3D12RootSignature* rs,unsigned i){D3D12_COMPUTE_PIPELINE_STATE_DESC d{};d.pRootSignature=rs;
        d.CS={codes[i]->GetBufferPointer(),codes[i]->GetBufferSize()};ComPtr<ID3D12PipelineState> p;
        ok(prep.compute(dev.Get(),d,shader_key(d.CS),p.GetAddressOf()));return p;};
    {PipelinePrep cold;auto rs=root();for(unsigned i:{0u,1u})get(cold,rs.Get(),i);}
    PipelinePrep warm;warm.start(dev.Get());warm.join_and_count_hits();auto rs=root();
    D3D12_COMMAND_QUEUE_DESC qd{};ComPtr<ID3D12CommandQueue> q;ok(dev->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)));
    ComPtr<ID3D12CommandAllocator> alloc;ComPtr<ID3D12GraphicsCommandList> list;
    ok(dev->CreateCommandAllocator(qd.Type,IID_PPV_ARGS(&alloc)));ok(dev->CreateCommandList(0,qd.Type,alloc.Get(),nullptr,IID_PPV_ARGS(&list)));
    auto buffer=[&](bool gpu){D3D12_HEAP_PROPERTIES h{};h.Type=gpu?D3D12_HEAP_TYPE_DEFAULT:D3D12_HEAP_TYPE_READBACK;h.CreationNodeMask=h.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC b{D3D12_RESOURCE_DIMENSION_BUFFER,0,8,1,1,1,DXGI_FORMAT_UNKNOWN,{1,0},D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
            gpu?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE};ComPtr<ID3D12Resource> v;
        ok(dev->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&b,gpu?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&v)));return v;};
    auto gpu=buffer(true),cpu=buffer(false);std::array<ComPtr<ID3D12PipelineState>,2> keep;
    list->SetComputeRootSignature(rs.Get());list->SetComputeRootUnorderedAccessView(0,gpu->GetGPUVirtualAddress());
    for(unsigned i:{1u,0u}){keep[i]=get(warm,rs.Get(),i);list->SetPipelineState(keep[i].Get());list->Dispatch(1,1,1);}
    D3D12_RESOURCE_BARRIER barrier{};barrier.Transition={gpu.Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1,&barrier);list->CopyResource(cpu.Get(),gpu.Get());ok(list->Close());
    ID3D12CommandList* lists[]{list.Get()};q->ExecuteCommandLists(1,lists);ComPtr<ID3D12Fence> done;ok(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&done)));
    ok(q->Signal(done.Get(),1));ok(done->SetEventOnCompletion(1,nullptr));UINT* values{};ok(cpu->Map(0,nullptr,reinterpret_cast<void**>(&values)));
    auto hits=warm.join_and_count_hits();bool good=hits==2 && values[0]==100 && values[1]==101;
    std::printf("pipeline-check: %u/2 prepared pipelines consumed, GPU words %u and %u (expected 100 and 101)\n",hits,values[0],values[1]);
    D3D12_RANGE empty{};cpu->Unmap(0,&empty);
    wchar_t file[2048]{};auto n=GetEnvironmentVariableW(L"FROSTY_TEST_RAY_PREP",file,2048);
    if(n)good=n<2048 && ray_check(dev.Get(),rs.Get(),file) && good;
    return good?0:1;
}catch(const std::exception& e){std::fprintf(stderr,"pipeline-check: %s\n",e.what());return 1;}
