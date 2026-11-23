// Hold one GPU reader behind a fence, change the placed texture's backing,
// then submit a second queue. Both CPU submissions must return before Signal.
// Verify each queue's generation on the GPU; read back only the two counts.
#define NOMINMAX
#include "texture_upload.hpp"
#include "depth_alias.hpp"
#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
namespace {
void ok(HRESULT hr){if(FAILED(hr)){std::fprintf(stderr,"upload-check HRESULT %#lx\n",(unsigned long)hr);throw std::runtime_error("GPU call failed");}}
#include "test_depth_planes.inl"
constexpr UINT width=64,height=32;
struct AddressComputer{
    struct Vtable{ULONG(WINAPI* add)(AddressComputer*);ULONG(WINAPI* release)(AddressComputer*);void* unused[4];
        UINT64(WINAPI* offset)(AddressComputer*,UINT,UINT,UINT64,UINT,UINT,UINT,void*);};
    const Vtable* v;
    ULONG AddRef(){return v->add(this);}ULONG Release(){return v->release(this);}
};
struct Commands{
    ComPtr<ID3D12CommandAllocator> alloc;ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Resource> result,cpu;
};
bool check_storage_formats(ID3D12Device* d,ID3D12CommandQueue* q,XboxTextures& upload){
    struct Shape{DXGI_FORMAT fmt;UINT bytes,block,layers;bool volume;};
    constexpr Shape cases[]{{DXGI_FORMAT_R8_UINT,1,1,3,false},{DXGI_FORMAT_R16_UINT,2,1,2,false},
        {DXGI_FORMAT_BC1_UNORM,8,4,2,false},{DXGI_FORMAT_BC7_UNORM,16,4,3,false},{DXGI_FORMAT_R32_UINT,4,1,5,true}};
    for(auto [format,elem,block,layers,volume]:cases){
        D3D12_RESOURCE_DESC xs{volume?D3D12_RESOURCE_DIMENSION_TEXTURE3D:D3D12_RESOURCE_DIMENSION_TEXTURE2D,0,
            block==4?68u:65u,block==4?36u:33u,UINT16(layers),3,format,{1,0},D3D12_TEXTURE_LAYOUT_UNKNOWN,D3D12_RESOURCE_FLAG_NONE};
        ComPtr<XgComputer> computer;ok(xg_computer(xs,computer.GetAddressOf()));SIZE_T size=computer->v->size(computer.Get());
        auto mem=static_cast<BYTE*>(VirtualAlloc(nullptr,size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        if(!mem)throw std::bad_alloc();
        struct Cleanup{BYTE* m;~Cleanup(){VirtualFree(m,0,MEM_RELEASE);}} cleanup{mem};
        UINT n=xs.MipLevels*(volume?1:layers);std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(n);std::vector<UINT> rows(n);std::vector<UINT64> widths(n);
        UINT64 span{};d->GetCopyableFootprints(&xs,0,n,0,fp.data(),rows.data(),widths.data(),&span);
        auto pattern=[](UINT i,UINT x,UINT y,UINT z,UINT b){return BYTE(23*i+13*x+91*y+103*z+7*b+19);};
        for(UINT i=0;i<n;++i)for(UINT z=0;z<fp[i].Footprint.Depth;++z)for(UINT y=0;y<rows[i];++y)
            for(UINT x=0;x<widths[i]/elem;++x){
                auto at=computer->v->offset(computer.Get(),0,i%xs.MipLevels,x,y,volume?z:i/xs.MipLevels,0,nullptr);
                if(at>size || elem>size-at)throw std::runtime_error("oracle offset outside XG allocation");
                for(UINT j=0;j<elem;++j)mem[at+j]=pattern(i,x,y,z,j);
            }
        ComPtr<ID3D12Resource> image,cpu;ok(upload.load(d,mem,xs,D3D12_RESOURCE_STATE_COPY_SOURCE,nullptr,IID_PPV_ARGS(&image)));
        D3D12_HEAP_PROPERTIES h{};h.Type=D3D12_HEAP_TYPE_READBACK;h.CreationNodeMask=h.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC b{D3D12_RESOURCE_DIMENSION_BUFFER,0,span,1,1,1,DXGI_FORMAT_UNKNOWN,{1,0},D3D12_TEXTURE_LAYOUT_ROW_MAJOR,D3D12_RESOURCE_FLAG_NONE};
        ok(d->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&b,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&cpu)));
        ComPtr<ID3D12CommandAllocator> a;ComPtr<ID3D12GraphicsCommandList> l;ok(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a)));
        ok(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,a.Get(),nullptr,IID_PPV_ARGS(&l)));
        for(UINT i=0;i<n;++i){D3D12_TEXTURE_COPY_LOCATION to{},from{};to.pResource=cpu.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            to.PlacedFootprint=fp[i];from.pResource=image.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;from.SubresourceIndex=i;
            l->CopyTextureRegion(&to,0,0,0,&from,nullptr);}
        ok(l->Close());TextureReads reads;reads.add(upload.access(image.Get()));const TextureReads* uses[]{&reads};UploadConsumer c;
        ID3D12CommandList* lists[]{l.Get()};ok(upload.submit(d,q,c,uses,1,lists));ComPtr<ID3D12Fence> done;
        ok(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&done)));ok(q->Signal(done.Get(),1));ok(done->SetEventOnCompletion(1,nullptr));
        BYTE* p{};ok(cpu->Map(0,nullptr,reinterpret_cast<void**>(&p)));UINT matches=0,total=0;
        for(UINT i=0;i<n;++i)for(UINT z=0;z<fp[i].Footprint.Depth;++z)for(UINT y=0;y<rows[i];++y)
            for(UINT x=0;x<widths[i]/elem;++x)for(UINT j=0;j<elem;++j){
                matches+=p[fp[i].Offset+(z*rows[i]+y)*fp[i].Footprint.RowPitch+x*elem+j]==pattern(i,x,y,z,j);++total;}
        D3D12_RANGE empty{};cpu->Unmap(0,&empty);std::printf("GPU detile format %u, %u subresources: %u/%u bytes\n",UINT(format),n,matches,total);
        if(matches!=total)return false;
    }
    return true;
}
}
int main(int argc,char** argv)try{
    if(!LoadLibraryW(L"xg_xs.dll"))throw std::runtime_error("missing packaged address computer");
    ComPtr<ID3D12Device> d;ok(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d)));
    if(argc==2 && !std::strcmp(argv[1],"--prepare-layouts"))return prepare_known_tile_layouts(d.Get());
    if(argc!=1)return 2;
    XboxTextures upload;std::array<UploadConsumer,2> consumers;
    std::array<ComPtr<ID3D12CommandQueue>,2> q;D3D12_COMMAND_QUEUE_DESC qd{};
    for(auto& v:q)ok(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&v)));
    ComPtr<ID3D12Fence> gate,done;ok(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));
    ok(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&done)));
    constexpr UINT capacity=1<<20;
    auto* data=static_cast<UINT*>(VirtualAlloc(nullptr,capacity,MEM_RESERVE,PAGE_READWRITE));
    if(!data)throw std::bad_alloc();
    D3D12_RESOURCE_DESC tex{D3D12_RESOURCE_DIMENSION_TEXTURE2D,0,width,height,1,2,DXGI_FORMAT_R32_UINT,{1,0},
                             D3D12_TEXTURE_LAYOUT_UNKNOWN,D3D12_RESOURCE_FLAG_NONE};
    using Create=HRESULT(WINAPI*)(const D3D12_RESOURCE_DESC*,AddressComputer**);
    auto create=std::bit_cast<Create>(GetProcAddress(GetModuleHandleW(L"xg_xs.dll"),"XGCreateTextureComputer"));
    ComPtr<AddressComputer> layout;if(!create)throw std::runtime_error("missing XG texture layout");
    ok(create(&tex,layout.GetAddressOf()));
    auto fill=[&](UINT gen){
        std::fill_n(data,capacity/4,0xa7a7a7a7u);
        for(UINT mip=0;mip<2;++mip)for(UINT y=0;y<(height>>mip);++y)for(UINT x=0;x<(width>>mip);++x){
            auto off=layout->v->offset(layout.Get(),0,mip,x,y,0,0,nullptr);
            if(off>capacity-4 || off%4)throw std::runtime_error("XG wrote outside fixture allocation");
            data[off/4]=gen+13*x+273*y+65536*mip;
        }
    };
    ComPtr<ID3D12Resource> image;
    ok(upload.load(d.Get(),reinterpret_cast<BYTE*>(data),tex,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&image)));
    D3D12_SHADER_RESOURCE_VIEW_DESC top{};top.Format=tex.Format;top.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    top.Texture2D.MipLevels=1;auto first=upload.shader_view(image.Get(),&top);
    if(!first)throw std::runtime_error("missing sparse view");
    for(UINT i=0;i<3;++i){ok(upload.refresh(d.Get(),image.Get(),first.get()));if(first->ready)throw std::runtime_error("absent mip marked ready");}
    if(VirtualAlloc(data,capacity,MEM_COMMIT,PAGE_READWRITE)!=data)throw std::bad_alloc();
    fill(17);
    ok(upload.refresh(d.Get(),image.Get(),first.get()));
    if(!first->ready)throw std::runtime_error("newly resident mip was skipped");
    ok(upload.refresh(d.Get(),image.Get()));
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};
    D3D12_ROOT_PARAMETER par[3]{};par[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;par[0].DescriptorTable={1,&range};
    par[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;par[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;par[2].Constants={0,0,1};
    D3D12_ROOT_SIGNATURE_DESC desc{3,par,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> sig,cs;ok(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&sig,nullptr));
    ComPtr<ID3D12RootSignature> rs;ok(d->CreateRootSignature(0,sig->GetBufferPointer(),sig->GetBufferSize(),IID_PPV_ARGS(&rs)));
    const char shader[]="Texture2D<uint> tex:register(t0);RWByteAddressBuffer outbuf:register(u0);"
        "cbuffer C:register(b0){uint expected;} [numthreads(1,1,1)] void main(){uint n=0;"
        "[loop] for(int y=0;y<32;++y)[loop] for(int x=0;x<64;++x)n+=uint(tex.Load(int3(x,y,0))==uint(17+13*x+273*y));"
        "[loop] for(int v=0;v<16;++v)[loop] for(int u=0;u<32;++u)n+=uint(tex.Load(int3(u,v,1))==expected+65536+13*u+273*v);outbuf.Store(0,n);}";
    ok(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&cs,nullptr));
    D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};ps.pRootSignature=rs.Get();ps.CS={cs->GetBufferPointer(),cs->GetBufferSize()};
    ComPtr<ID3D12PipelineState> state;ok(d->CreateComputePipelineState(&ps,IID_PPV_ARGS(&state)));
    ComPtr<ID3D12DescriptorHeap> heap;D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    ok(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));d->CreateShaderResourceView(image.Get(),nullptr,heap->GetCPUDescriptorHandleForHeapStart());
    std::array<Commands,2> commands;
    auto buffer=[&](D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES initial,D3D12_RESOURCE_FLAGS flags,ID3D12Resource** out){
        D3D12_RESOURCE_DESC b{D3D12_RESOURCE_DIMENSION_BUFFER,0,4,1,1,1,DXGI_FORMAT_UNKNOWN,{1,0},D3D12_TEXTURE_LAYOUT_ROW_MAJOR,flags};
        D3D12_HEAP_PROPERTIES h{};h.Type=heap;h.CreationNodeMask=h.VisibleNodeMask=1;
        ok(d->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&b,initial,nullptr,IID_PPV_ARGS(out)));
    };
    struct GateGuard{ID3D12Fence* f;~GateGuard(){if(f)f->Signal(1);}} release{gate.Get()};
    ok(q[0]->Wait(gate.Get(),1));
    for(UINT i=0;i<2;++i){
        if(i){fill(29);
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=tex.Format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D.MostDetailedMip=1;view.Texture2D.MipLevels=1;auto srv=upload.shader_view(image.Get(),&view);
            if(!srv)throw std::runtime_error("missing first-use refresh");
            ok(upload.refresh(d.Get(),image.Get(),srv.get()));}
        auto& c=commands[i];
        buffer(D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,c.result.GetAddressOf());
        buffer(D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE,c.cpu.GetAddressOf());
        ok(d->CreateCommandAllocator(qd.Type,IID_PPV_ARGS(&c.alloc)));
        ok(d->CreateCommandList(0,qd.Type,c.alloc.Get(),state.Get(),IID_PPV_ARGS(&c.list)));
        auto* h=heap.Get();c.list->SetDescriptorHeaps(1,&h);c.list->SetComputeRootSignature(rs.Get());
        c.list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());c.list->SetComputeRootUnorderedAccessView(1,c.result->GetGPUVirtualAddress());
        c.list->SetComputeRoot32BitConstant(2,i?29:17,0);c.list->Dispatch(1,1,1);
        D3D12_RESOURCE_BARRIER b{};b.Transition={c.result.Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};
        c.list->ResourceBarrier(1,&b);c.list->CopyBufferRegion(c.cpu.Get(),0,c.result.Get(),0,4);ok(c.list->Close());
        TextureReads reads;reads.add(upload.access(image.Get()));const TextureReads* u[]{&reads};
        ID3D12CommandList* lists[]{c.list.Get()};ok(upload.submit(d.Get(),q[i].Get(),consumers[i],u,1,lists));
    }
    // No CPU path can have waited for the read/refresh pair to finish yet.
    VirtualFree(data,0,MEM_RELEASE);ok(gate->Signal(1));release.f=nullptr;ok(q[1]->Signal(done.Get(),1));
    ok(done->SetEventOnCompletion(1,nullptr));
    for(UINT i=0;i<2;++i){
        UINT* value{};ok(commands[i].cpu->Map(0,nullptr,reinterpret_cast<void**>(&value)));UINT n=*value;
        commands[i].cpu->Unmap(0,nullptr);
        constexpr UINT expected=width*height+(width/2)*(height/2);
        std::printf("queue %u: %u/%u patterned texels matched, mip 0 generation 17, mip 1 generation %u\n",i,n,expected,i?29:17);
        if(n!=expected)return 1;
    }
    if(GetEnvironmentVariableA("FROSTY_TEST_GPU_DETILE",nullptr,0) && !check_storage_formats(d.Get(),q[0].Get(),upload))return 1;
    if(GetEnvironmentVariableA("FROSTY_TEST_PLANE_PATH",nullptr,0) && !check_depth_planes(d.Get(),q[0].Get()))return 1;
    return 0;
}catch(const std::exception& e){std::fprintf(stderr,"frosty-upload-check: %s\n",e.what());return 1;}
