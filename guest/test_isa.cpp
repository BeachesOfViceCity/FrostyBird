// Execute host-lifted ISA conformance kernels via the same DXIL/Vulkan backend.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include "xs_ray.hpp"
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
namespace {
void ok(HRESULT hr){if(FAILED(hr)){std::fprintf(stderr,"isa-check HRESULT %#lx\n",static_cast<unsigned long>(hr));throw std::runtime_error("GPU operation");}}
std::vector<char> bytes(fs::path p){std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("missing "+p.string());return {std::istreambuf_iterator<char>(f),{}};}
struct GatherImage {
    ComPtr<ID3D12Resource> tex,upload;ComPtr<ID3D12DescriptorHeap> heap;
    void bind(ID3D12Device* dev,ID3D12GraphicsCommandList* cl,const fs::path& path){
        if(!fs::is_regular_file(path))return;
        auto data=bytes(path);std::array<UINT,3> header{};if(data.size()<12)throw std::runtime_error("short gather fixture");
        std::memcpy(header.data(),data.data(),12);auto [w,h,mips]=header;
        if(!w||!h||w>16384||h>16384||!mips||mips>15)throw std::runtime_error("invalid gather fixture");
        D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D,0,w,h,1,UINT16(mips),DXGI_FORMAT_R32_FLOAT,{1,0},D3D12_TEXTURE_LAYOUT_UNKNOWN,D3D12_RESOURCE_FLAG_NONE};
        D3D12_HEAP_PROPERTIES hp{};hp.CreationNodeMask=hp.VisibleNodeMask=1;hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        ok(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&tex)));
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(mips);UINT64 size{};
        dev->GetCopyableFootprints(&desc,0,mips,0,layouts.data(),nullptr,nullptr,&size);
        auto bd=D3D12_RESOURCE_DESC{D3D12_RESOURCE_DIMENSION_BUFFER,0,size,1,1,1,DXGI_FORMAT_UNKNOWN,{1,0},D3D12_TEXTURE_LAYOUT_ROW_MAJOR,D3D12_RESOURCE_FLAG_NONE};hp.Type=D3D12_HEAP_TYPE_UPLOAD;
        ok(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)));
        BYTE* ptr{};ok(upload->Map(0,nullptr,(void**)&ptr));size_t at=12;
        for(UINT i=0;i<mips;++i){auto fp=layouts[i];auto& f=fp.Footprint;
            for(UINT y=0;y<f.Height;++y){if(4ull*f.Width>data.size()-at)throw std::runtime_error("truncated gather mip");
                std::memcpy(ptr+fp.Offset+y*f.RowPitch,data.data()+at,4*f.Width);at+=4*f.Width;}
        }
        if(at!=data.size())throw std::runtime_error("extra gather mip data");
        upload->Unmap(0,nullptr);
        for(UINT i=0;i<mips;++i){D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            from.PlacedFootprint=layouts[i];to.pResource=tex.Get();to.SubresourceIndex=i;cl->CopyTextureRegion(&to,0,0,0,&from,nullptr);}
        D3D12_RESOURCE_BARRIER b{};b.Transition={tex.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};cl->ResourceBarrier(1,&b);
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
        ok(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));dev->CreateShaderResourceView(tex.Get(),nullptr,heap->GetCPUDescriptorHandleForHeapStart());
        auto* handle=heap.Get();cl->SetDescriptorHeaps(1,&handle);cl->SetComputeRootDescriptorTable(2,heap->GetGPUDescriptorHandleForHeapStart());
    }
};
}
int main(int argc,char** argv) try {
    if(argc!=2)return 2;
    ComPtr<ID3D12Device> d;ok(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d)));
    if(GetEnvironmentVariableA("FROSTY_TEST_GROUP_ORDINAL",nullptr,0) && !xs_enable_intrinsic(d.Get(),xs_group_ordinal))
        throw std::runtime_error("backend lacks native subgroup ordinal");
    D3D12_DESCRIPTOR_RANGE ir{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,1,0,0};
    D3D12_DESCRIPTOR_RANGE sr{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};
    D3D12_ROOT_PARAMETER par[3]{};par[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
    par[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;par[1].DescriptorTable={1,&ir};
    par[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;par[2].DescriptorTable={1,&sr};
    D3D12_ROOT_SIGNATURE_DESC rd{3,par,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> sig;ok(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&sig,nullptr));
    ComPtr<ID3D12RootSignature> rs;ok(d->CreateRootSignature(0,sig->GetBufferPointer(),sig->GetBufferSize(),IID_PPV_ARGS(&rs)));
    ComPtr<ID3D12CommandQueue> q;D3D12_COMMAND_QUEUE_DESC qd{};ok(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)));
    UINT cases=0,errors=0;
    for(auto& ent:fs::directory_iterator(argv[1])){
        if(ent.path().extension()!=".ref")continue;
        auto ref=bytes(ent.path()),code=bytes(fs::path(ent.path()).replace_extension(".dxil"));
        std::array<UINT,3> header{};if(ref.size()<12)throw std::runtime_error("truncated reference");
        std::memcpy(header.data(),ref.data(),12);auto [groups,threads,stride]=header;
        if(!groups||!threads||!stride||stride%4||UINT64(groups)*threads*stride!=ref.size()-12)throw std::runtime_error("invalid reference size");
        auto n=ref.size()-12;
        auto buffer=[&](D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES s,D3D12_RESOURCE_FLAGS flags,UINT64 size=0){
            D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER,0,size?size:n,1,1,1,DXGI_FORMAT_UNKNOWN,{1,0},D3D12_TEXTURE_LAYOUT_ROW_MAJOR,flags};
            D3D12_HEAP_PROPERTIES hp{};hp.Type=heap;hp.CreationNodeMask=hp.VisibleNodeMask=1;
            ComPtr<ID3D12Resource> r;ok(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,s,nullptr,IID_PPV_ARGS(&r)));return r;};
        auto gpu=buffer(D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto seed=buffer(D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_RESOURCE_FLAG_NONE);
        auto cpu=buffer(D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
        void* data{};ok(seed->Map(0,nullptr,&data));std::memset(data,0xa7,n);seed->Unmap(0,nullptr);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=rs.Get();pd.CS={code.data(),code.size()};
        ComPtr<ID3D12PipelineState> ps;
        bool compile_only=GetEnvironmentVariableA("FROSTY_TEST_COMPILE_ONLY",nullptr,0)!=0;
        if(compile_only)std::fprintf(stderr,"isa-check: creating %s PSO\n",ent.path().stem().string().c_str());
        ok(d->CreateComputePipelineState(&pd,IID_PPV_ARGS(&ps)));
        if(compile_only){++cases;continue;}
        ComPtr<ID3D12CommandAllocator> alloc;ComPtr<ID3D12GraphicsCommandList> cl;ok(d->CreateCommandAllocator(qd.Type,IID_PPV_ARGS(&alloc)));
        ok(d->CreateCommandList(0,qd.Type,alloc.Get(),ps.Get(),IID_PPV_ARGS(&cl)));
        auto barrier=[&](D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){D3D12_RESOURCE_BARRIER b{};
            b.Transition={gpu.Get(),0,from,to};cl->ResourceBarrier(1,&b);};
        cl->CopyResource(gpu.Get(),seed.Get());barrier(D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl->SetComputeRootSignature(rs.Get());cl->SetComputeRootUnorderedAccessView(0,gpu->GetGPUVirtualAddress());
        auto sidecar=fs::path(ent.path()).replace_extension(".img64");
        std::vector<char> pixels;UINT width=0,height=0;
        ComPtr<ID3D12Resource> img,img_seed,img_out;ComPtr<ID3D12DescriptorHeap> heap;
        D3D12_TEXTURE_COPY_LOCATION tex{},buf{};D3D12_RESOURCE_BARRIER ib{};
        if(fs::is_regular_file(sidecar)){
            pixels=bytes(sidecar);if(pixels.size()<8)throw std::runtime_error("truncated image fixture");
            std::memcpy(&width,pixels.data(),4);std::memcpy(&height,pixels.data()+4,4);
            if(!width||!height||width>16384||height>16384||8+16ull*width*height!=pixels.size())throw std::runtime_error("invalid image fixture");
            D3D12_FEATURE_DATA_D3D12_OPTIONS9 feature{};
            ok(d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9,&feature,sizeof(feature)));
            if(!feature.AtomicInt64OnTypedResourceSupported)throw std::runtime_error("adapter lacks 64-bit typed image atomics");
            D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D,0,width,height,1,1,DXGI_FORMAT_R32G32_UINT,
                {1,0},D3D12_TEXTURE_LAYOUT_UNKNOWN,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
            D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;hp.CreationNodeMask=hp.VisibleNodeMask=1;
            ok(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&img)));
            UINT64 span{};buf.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            d->GetCopyableFootprints(&desc,0,1,0,&buf.PlacedFootprint,nullptr,nullptr,&span);
            img_seed=buffer(D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_RESOURCE_FLAG_NONE,span);
            img_out=buffer(D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE,span);
            ok(img_seed->Map(0,nullptr,&data));for(UINT y=0;y<height;++y)std::memcpy(static_cast<BYTE*>(data)+y*buf.PlacedFootprint.Footprint.RowPitch,pixels.data()+8+8ull*y*width,width*8);
            img_seed->Unmap(0,nullptr);tex.pResource=img.Get();buf.pResource=img_seed.Get();cl->CopyTextureRegion(&tex,0,0,0,&buf,nullptr);
            ib.Transition={img.Get(),0,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};cl->ResourceBarrier(1,&ib);
            D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
            ok(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));d->CreateUnorderedAccessView(img.Get(),nullptr,nullptr,heap->GetCPUDescriptorHandleForHeapStart());
            auto* h=heap.Get();cl->SetDescriptorHeaps(1,&h);cl->SetComputeRootDescriptorTable(1,heap->GetGPUDescriptorHandleForHeapStart());
        }
         GatherImage gather;gather.bind(d.Get(),cl.Get(),fs::path(ent.path()).replace_extension(".gather"));
         ComPtr<ID3D12Resource> input;ComPtr<ID3D12DescriptorHeap> input_heap;
         auto srv=fs::path(ent.path()).replace_extension(".srv");
         if(fs::is_regular_file(srv)){
             auto raw=bytes(srv);if(raw.empty() || raw.size()%4 || raw.size()>1u<<20)throw std::runtime_error("invalid buffer fixture");
             input=buffer(D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_RESOURCE_FLAG_NONE,raw.size());
             ok(input->Map(0,nullptr,&data));std::memcpy(data,raw.data(),raw.size());input->Unmap(0,nullptr);
             D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
             ok(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&input_heap)));
             D3D12_SHADER_RESOURCE_VIEW_DESC sd{};sd.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;sd.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
             sd.Buffer.NumElements=UINT(raw.size()/4);sd.Buffer.StructureByteStride=4;
             d->CreateShaderResourceView(input.Get(),&sd,input_heap->GetCPUDescriptorHandleForHeapStart());
             auto* hh=input_heap.Get();cl->SetDescriptorHeaps(1,&hh);cl->SetComputeRootDescriptorTable(2,input_heap->GetGPUDescriptorHandleForHeapStart());
         }
        cl->Dispatch(groups,1,1);
        if(img){
            ib.Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;ib.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
            cl->ResourceBarrier(1,&ib);buf.pResource=img_out.Get();cl->CopyTextureRegion(&buf,0,0,0,&tex,nullptr);
        }
        barrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);cl->CopyResource(cpu.Get(),gpu.Get());
        ok(cl->Close());ID3D12CommandList* cmd=cl.Get();q->ExecuteCommandLists(1,&cmd);
        ComPtr<ID3D12Fence> f;ok(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f)));ok(q->Signal(f.Get(),1));
        HANDLE ev=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!ev)throw std::runtime_error("fence event");
        auto hr=f->SetEventOnCompletion(1,ev);auto wait=SUCCEEDED(hr)?WaitForSingleObject(ev,20000):WAIT_FAILED;CloseHandle(ev);
        if(wait!=WAIT_OBJECT_0)throw std::runtime_error("conformance dispatch timeout");
        ok(cpu->Map(0,nullptr,&data));UINT bad=0;
        for(size_t j=0;j<n/4;++j){UINT want;std::memcpy(&want,ref.data()+12+4*j,4);auto got=static_cast<const UINT*>(data)[j];
            if(want!=got && bad++<4)std::fprintf(stderr,"%s lane %zu word %zu: %08x != %08x\n",ent.path().stem().string().c_str(),j/(stride/4),j%(stride/4),got,want);}
        cpu->Unmap(0,nullptr);++cases;errors+=bad;
        std::printf("%s: %zu/%zu words\n",ent.path().stem().string().c_str(),n/4-bad,n/4);
        if(img){
            ok(img_out->Map(0,nullptr,&data));UINT mismatched=0;
            for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){
                UINT64 got,want;std::memcpy(&got,static_cast<BYTE*>(data)+y*buf.PlacedFootprint.Footprint.RowPitch+8*x,8);
                std::memcpy(&want,pixels.data()+8+8ull*(width*height+y*width+x),8);
                if(got!=want && mismatched++<3)std::fprintf(stderr,"texel (%u,%u): %016llx != %016llx\n",x,y,got,want);
            }
            img_out->Unmap(0,nullptr);errors+=mismatched;std::printf("R32G32_UINT: %u/%u 64-bit texels\n",width*height-mismatched,width*height);
        }
    }
    std::printf("ISA conformance: %u kernels, %u incorrect words\n",cases,errors);
    return !cases||errors?1:0;
}catch(const std::exception& e){std::fprintf(stderr,"isa-check: %s\n",e.what());return 1;}
