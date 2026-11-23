// Headless end-to-end ABI and shader test. Submit the original DXBC bytes
// through d3d12_xs, draw a quad, and check the interpolated UVs on readback.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include "recompiled_shader.hpp"
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <dxgi1_4.h>
#include <mfapi.h>
#include <mfobjects.h>
#include <wrl/client.h>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
void ok(const char* op, HRESULT hr) {
    if (FAILED(hr)) {
        std::fprintf(stderr, "%s: HRESULT 0x%08lx\n", op, static_cast<unsigned long>(hr));
        throw std::runtime_error(op);
    }
}
#define CHECK(expr) ok(#expr, (expr))
#include "test_cube_view.inl"

ComPtr<ID3DBlob> test_shader(const char* src,const char* profile) {
    ComPtr<ID3DBlob> blob, error;
    auto hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, "main", profile,
                         0, 0, &blob, &error);
    if (FAILED(hr) && error) std::fputs(static_cast<char*>(error->GetBufferPointer()), stderr);
    CHECK(hr);
    return blob;
}

void test_mf_reset(ID3D12Device* dev) {
    using Reset = HRESULT (WINAPI*)(IMFDXGIDeviceManager*, IUnknown*, UINT);
    auto reset = std::bit_cast<Reset>(GetProcAddress(GetModuleHandleW(L"mfplat.dll"), "MFResetDXGIDeviceManagerX"));
    if (!reset) throw std::runtime_error("MFResetDXGIDeviceManagerX missing: use the Wine fork");
    UINT token{};
    ComPtr<IMFDXGIDeviceManager> mgr;
    CHECK(MFCreateDXGIDeviceManager(&token, &mgr));
    if (reset(mgr.Get(), dev, token ^ 1u) != E_INVALIDARG)
        throw std::runtime_error("MFResetDXGIDeviceManagerX accepted an invalid token");
    CHECK(reset(mgr.Get(), dev, token));
    HANDLE handle{};
    CHECK(mgr->OpenDeviceHandle(&handle));
    ComPtr<ID3D12Device> service;
    CHECK(mgr->GetVideoService(handle, IID_PPV_ARGS(&service)));
    CHECK(mgr->CloseDeviceHandle(handle));
    if (service.Get() != dev) throw std::runtime_error("MF device identity mismatch");
    std::puts("MFResetDXGIDeviceManagerX: verified token and D3D12 device round-trip");
    using Wrap = HRESULT(WINAPI*)(REFIID,IUnknown*,UINT,BOOL,IMFMediaBuffer**);
    auto wrap=std::bit_cast<Wrap>(GetProcAddress(GetModuleHandleW(L"mfplat.dll"),"MFCreateDXGISurfaceBufferX"));
    if(!wrap)throw std::runtime_error("MFCreateDXGISurfaceBufferX missing: rebuild mfplat");
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;hp.CreationNodeMask=hp.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D,0,16,16,2,1,DXGI_FORMAT_R8G8B8A8_UNORM,
                             {1,0},D3D12_TEXTURE_LAYOUT_UNKNOWN,D3D12_RESOURCE_FLAG_NONE};
    ComPtr<ID3D12Resource> tex,actual;
    CHECK(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&tex)));
    ComPtr<IMFMediaBuffer> buffer;CHECK(wrap(__uuidof(ID3D12Resource),tex.Get(),1,TRUE,&buffer));
    ComPtr<IMFDXGIBuffer> dxgi;CHECK(buffer.As(&dxgi));CHECK(dxgi->GetResource(IID_PPV_ARGS(&actual)));
    UINT plane=0;CHECK(dxgi->GetSubresourceIndex(&plane));
    if(plane!=1 || actual.Get()!=tex.Get())throw std::runtime_error("MF surface/subresource identity mismatch");
    std::puts("MFCreateDXGISurfaceBufferX: verified original D3D12 surface and subresource");
}

void test_dxc_utils(const std::vector<char>& vs) {
    auto dll = LoadLibraryW(L"dxcompiler_xs.dll");
    if (!dll) throw std::runtime_error("cannot load dxcompiler_xs.dll");
    struct Guard { HMODULE dll; ~Guard() { FreeLibrary(dll); } } guard{dll};
    auto create = std::bit_cast<DxcCreateInstanceProc>(GetProcAddress(dll, "DxcCreateInstance"));
    if (!create) throw std::runtime_error("missing DxcCreateInstance");
    ComPtr<IDxcUtils> utils;
    CHECK(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils)));
    DxcBuffer buffer{vs.data(), vs.size(), 0};
    void* part{};
    UINT32 size{};
    CHECK(utils->GetDxilContainerPart(&buffer, MAKEFOURCC('I','S','G','1'), &part, &size));
    if (!part || size < 8) throw std::runtime_error("missing vertex input signature");
    std::puts("DxcUtils: read the retail shader's input signature via dxcompiler_xs.dll");
}
} // namespace

int main(int argc, char** argv) try {
    bool test_mf = false, test_dxc = false, rectangle=false;
    const char* retail_ps=nullptr;const char* retail_gs=nullptr;const char* fixture_ps=nullptr;bool cube_ps=false,interp_ps=false,zero_coverage=false;
    if (argc < 2) throw std::runtime_error("usage: frosty-draw.exe <retail CopyVS.dxbc> [--test-mf] [--test-dxc]");
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--test-mf") == 0) test_mf = true;
        else if (std::strcmp(argv[i], "--test-dxc") == 0) test_dxc = true;
        else if (std::strcmp(argv[i],"--rectlist")==0) rectangle=true;
        else if (std::strcmp(argv[i],"--pixel")==0 && i+1<argc) retail_ps=argv[++i];
        else if (std::strcmp(argv[i],"--wave-pixel")==0 && i+1<argc) fixture_ps=argv[++i];
        else if (std::strcmp(argv[i],"--cube-pixel")==0 && i+1<argc){fixture_ps=argv[++i];cube_ps=true;}
        else if (std::strcmp(argv[i],"--interp-pixel")==0 && i+1<argc){fixture_ps=argv[++i];interp_ps=true;}
        else if (std::strcmp(argv[i],"--zero-coverage")==0 && i+1<argc){fixture_ps=argv[++i];zero_coverage=true;}
        else if (std::strcmp(argv[i],"--geometry")==0 && i+1<argc) retail_gs=argv[++i];
        else throw std::runtime_error("unknown draw option");
    }
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) throw std::runtime_error("cannot open shader container");
    std::vector<char> vs{std::istreambuf_iterator<char>(input), {}};
    if (test_dxc) test_dxc_utils(vs);
    ComPtr<ID3D12Device> dev;
    // Use the data export from the XS DLL, as the game does.
    HMODULE xs = GetModuleHandleW(L"d3d12_xs.dll");
    auto iid = std::bit_cast<const GUID*>(GetProcAddress(xs, "IID_ID3D12Device"));
    if (!iid || !IsEqualGUID(*iid, __uuidof(ID3D12Device))) throw std::runtime_error("bad XS GUID export");
    CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, *iid,
                            reinterpret_cast<void**>(dev.GetAddressOf())));
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    CHECK(factory->EnumAdapterByLuid(dev->GetAdapterLuid(), IID_PPV_ARGS(&adapter)));
    DXGI_ADAPTER_DESC1 adapter_desc{};
    CHECK(adapter->GetDesc1(&adapter_desc));
    std::printf("D3D12 adapter: %ls [%04x:%04x]\n", adapter_desc.Description,
                adapter_desc.VendorId, adapter_desc.DeviceId);
    if (test_mf) test_mf_reset(dev.Get());

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    D3D12_ROOT_PARAMETER cube_param{};D3D12_DESCRIPTOR_RANGE cube_srv{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};
    D3D12_STATIC_SAMPLER_DESC cube_sampler{};
    if(cube_ps||interp_ps){
        cube_param.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;cube_param.DescriptorTable={1,&cube_srv};cube_param.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        cube_sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;cube_sampler.AddressU=cube_sampler.AddressV=cube_sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        cube_sampler.MaxAnisotropy=1;cube_sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_NEVER;cube_sampler.MaxLOD=D3D12_FLOAT32_MAX;cube_sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        rsd.NumParameters=1;rsd.pParameters=&cube_param;rsd.NumStaticSamplers=1;rsd.pStaticSamplers=&cube_sampler;
    }
    ComPtr<ID3DBlob> rs_bytes;
    CHECK(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_bytes, nullptr));
    ComPtr<ID3D12RootSignature> rs;
    CHECK(dev->CreateRootSignature(0, rs_bytes->GetBufferPointer(), rs_bytes->GetBufferSize(), IID_PPV_ARGS(&rs)));
    ComPtr<ID3DBlob> ref_vs,ps;
    constexpr auto fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = rs.Get();
    p.VS = {vs.data(), vs.size()};
    ComPtr<ID3DBlob> recompiled;
    std::vector<char> gs_bytes,wave_bytes,fixture_vertex;
    if(fixture_ps){
        if(zero_coverage){std::ifstream vsfile("coverage-vs.dxil",std::ios::binary);
            fixture_vertex.assign(std::istreambuf_iterator<char>(vsfile),{});if(fixture_vertex.empty())throw std::runtime_error("missing coverage fixture VS");
            p.VS={fixture_vertex.data(),fixture_vertex.size()};}
        else if(interp_ps){
            ref_vs=test_shader("struct V{float2 uv:TEXCOORD0;float4 p:SV_Position;};V main(uint i:SV_VertexID){V v;"
                               "v.p=float4(i==1?3:-1,i==2?3:-1,0,1);v.uv=float2(.25,.25);return v;}","vs_5_0");
            p.VS={ref_vs->GetBufferPointer(),ref_vs->GetBufferSize()};
        }else{ref_vs=test_shader("float4 main(uint id:SV_VertexID):SV_Position{return float4(id==1?3:-1,id==2?3:-1,0,1);}","vs_5_0");
            p.VS={ref_vs->GetBufferPointer(),ref_vs->GetBufferSize()};}
        std::ifstream file(fixture_ps,std::ios::binary);
        wave_bytes.assign(std::istreambuf_iterator<char>(file),{});if(wave_bytes.empty())throw std::runtime_error("missing translated pixel kernel");
        p.PS={wave_bytes.data(),wave_bytes.size()};
    }else if(retail_gs) {
        ref_vs=test_shader(
            "struct Out {float2 uv:TEXCOORD0;float4 pos:SV_Position;nointerpolation uint slice:TEXCOORD1;};"
            "Out main(uint id:SV_VertexID,uint inst:SV_InstanceID){Out o;o.uv=float2(id&1,id>>1);"
            "o.pos=float4(o.uv.x*2-1,1-2*o.uv.y,0,1);o.slice=inst;return o;}","vs_5_0");
        p.VS={ref_vs->GetBufferPointer(),ref_vs->GetBufferSize()};
        std::ifstream file(retail_gs,std::ios::binary);
        gs_bytes.assign(std::istreambuf_iterator<char>(file),{});
        if(gs_bytes.empty())throw std::runtime_error("missing retail geometry shader");
        p.GS={gs_bytes.data(),gs_bytes.size()};
        ps=test_shader("float4 main(float2 uv:TEXCOORD0,float4 pos:SV_Position,nointerpolation uint slice:SV_RenderTargetArrayIndex):SV_Target"
            "{return float4(uv,slice!=0 ? 0.75 : 0.25,1);}","ps_5_0");
        p.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    } else if(retail_ps) {
        // Deliberately exercise the 32-bit export epilog. None of the blue
        // payloads is exactly representable as FP16; a half-precision pass
        // would look right in an 8-bit screenshot and fail this readback.
        ref_vs=test_shader(
            "struct Out {float4 value:TEXCOORD0;float4 pos:SV_Position;};"
            "Out main(uint id:SV_VertexID) {Out o;float2 uv=float2(id&1,id>>1);"
            "o.pos=float4(uv.x*2-1,1-2*uv.y,0,1);o.value=float4(uv,0.1234567,1);return o;}","vs_5_0");
        p.VS={ref_vs->GetBufferPointer(),ref_vs->GetBufferSize()};
        std::ifstream file(retail_ps,std::ios::binary);
        std::vector<char> bytes{std::istreambuf_iterator<char>(file),{}};
        p.PS={bytes.data(),bytes.size()};
        if(load_graphics_stage(p.PS,false,ps.GetAddressOf())!=S_OK) throw std::runtime_error("missing recompiled pixel shader");
    } else {
        if(!rectangle && load_graphics_stage(p.VS,true,recompiled.GetAddressOf())!=S_OK)
            throw std::runtime_error("missing recompiled vertex shader");
        ps=test_shader("float4 main(float4 p:SV_Position, float2 uv:TEXCOORD0):SV_Target {"
            "return float4(uv,p.x>0 ? 0.25 : 0,1);}","ps_5_0");
        p.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    }
    p.BlendState.RenderTarget[0] = {FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO,
        D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable = TRUE;
    p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    p.SampleMask = UINT_MAX;
    p.SampleDesc.Count = 1;
    p.NumRenderTargets = 1;
    p.RTVFormats[0] = retail_ps||fixture_ps?DXGI_FORMAT_R32G32B32A32_FLOAT:fmt;
    p.PrimitiveTopologyType = rectangle?static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(xs_rectangle_pipeline_type):D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    ComPtr<ID3D12PipelineState> state;
    if(retail_gs || rectangle)CHECK(make_graphics_variants(dev.Get(),p,state.GetAddressOf()));
    else CHECK(dev->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&state)));

    constexpr UINT width = 32, height = 32;
    const UINT slices=retail_gs?2:1;
    D3D12_RESOURCE_DESC tex_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, UINT16(slices), 1,
        p.RTVFormats[0], {1,0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    D3D12_HEAP_PROPERTIES props{};
    props.Type = D3D12_HEAP_TYPE_DEFAULT;
    props.CreationNodeMask = props.VisibleNodeMask = 1;
    ComPtr<ID3D12Resource> tex, readback;
    CHECK(dev->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &tex_desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&tex)));
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,2> layouts{};
    UINT64 total{};
    dev->GetCopyableFootprints(&tex_desc,0,slices,0,layouts.data(),nullptr,nullptr,&total);
    D3D12_RESOURCE_DESC buf_desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, total, 1, 1, 1,
        DXGI_FORMAT_UNKNOWN, {1,0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    props.Type = D3D12_HEAP_TYPE_READBACK;
    CHECK(dev->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &buf_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    CHECK(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(tex.Get(), nullptr, rtv);

    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC qd{};
    CHECK(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> alloc;
    CHECK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
    ComPtr<ID3D12GraphicsCommandList> list;
    CHECK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), state.Get(), IID_PPV_ARGS(&list)));
    D3D12_VIEWPORT viewport{0,0,float(width),float(height),0,1};
    D3D12_RECT scissor{0,0,width,height};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    constexpr float clear[4]{1,0,1,0};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list->SetGraphicsRootSignature(rs.Get());
    CubeFixture cube;if(cube_ps||interp_ps)cube.bind(dev.Get(),list.Get(),interp_ps);
    list->IASetPrimitiveTopology(rectangle||fixture_ps?D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    list->DrawInstanced(rectangle||fixture_ps?3:4,slices,fixture_ps?0:5,0); // exercise D3D's zero-based SV_VertexID
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {tex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.pResource = tex.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    for(UINT layer=0;layer<slices;++layer) {
        src.SubresourceIndex=layer;dst.PlacedFootprint=layouts[layer];
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    }
    CHECK(list->Close());
    ID3D12CommandList* lists[]{list.Get()};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    CHECK(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    CHECK(queue->Signal(fence.Get(), 1));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) throw std::runtime_error("cannot create fence event");
    auto hr = fence->SetEventOnCompletion(1, event);
    auto wait = SUCCEEDED(hr) ? WaitForSingleObject(event, 20000) : WAIT_FAILED;
    CloseHandle(event);
    CHECK(hr);
    if (wait != WAIT_OBJECT_0) throw std::runtime_error("GPU readback did not complete");

    void* mapping{};
    D3D12_RANGE range{0, SIZE_T(total)};
    CHECK(readback->Map(0, &range, &mapping));
    unsigned errors = 0;
    for(unsigned layer=0;layer<slices;++layer) for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
        const auto& layout=layouts[layer];
        if(retail_ps || fixture_ps) {
            const auto* row=reinterpret_cast<const float*>(static_cast<const BYTE*>(mapping)+layout.Offset+y*layout.Footprint.RowPitch);
            auto c=row+4*x;
            const auto index=x&3;
            bool enabled=((x<16?x:y)&1)==0;
            // 16 texels * |.75-.25| = 8: mip 3, or mip 4 at bias 1.
            bool good=interp_ps?(c[0]==(enabled?3000.f:-1.f) && c[1]==(enabled?4000.f:-2.f) &&
                                 c[2]==(enabled?.25f:.75f) && c[3]==((x&3)==1?(x==1?2034.f:2035.f):-3.f)):
                zero_coverage?std::equal(c,c+4,clear):cube_ps?(c[0]==cube.expected(x,y,1) && c[1]==cube.expected(x,y,2) &&
                               c[2]==((x&5)?-1.f:cube.expected(x,y,2)) && c[3]==cube.expected(x,y,0)):
                fixture_ps?(c[0]==9 && c[1]==17 && c[2]==float(index<2?100+index:17) && c[3]==(index==0?109:index==1?118:26)):
                (c[0]==(x+0.5f)/width && c[1]==(y+0.5f)/height && c[2]==0.1234567f && c[3]==1);
            if(!good) {
                if(!errors) std::fprintf(stderr,"first PS mismatch at %u,%u: %g %g %.9g %g\n",x,y,c[0],c[1],c[2],c[3]);
                ++errors;
            }
            continue;
        }
        auto* rgba = static_cast<std::uint8_t*>(mapping) + layout.Offset + y * layout.Footprint.RowPitch + 4*x;
        const auto expect = [](unsigned i, unsigned dimension) {
            return int(std::lround((i + .5) * 255. / dimension));
        };
        if (std::abs(rgba[0] - expect(x, width)) > 1 || std::abs(rgba[1] - expect(y, height)) > 1 ||
            rgba[2] != (layer?191:64) || rgba[3] != 255) {
            if (!errors) std::fprintf(stderr, "first mismatch at %u,%u: %u %u %u %u\n", x,y,rgba[0],rgba[1],rgba[2],rgba[3]);
            ++errors;
        }
    }
    D3D12_RANGE no_write{0,0};
    readback->Unmap(0, &no_write);
    std::printf("%s: %u/%u pixels match expected values\n",interp_ps?"Masked interpolation: retained coordinates, WQM subsets and mips 2/3/4":zero_coverage?"Retail null coverage retains the clear":cube_ps?"Cube array faces, layers, gradients and biased LOD":fixture_ps?"Masked elections and relative VGPR reads/writes":rectangle?"XS RECTLIST / CopyVS":retail_gs?"WriteToSliceMainGS":retail_ps?"Frosty PixelMain (RGBA32F)":"CopyVS",
        width*height*slices-errors,width*height*slices);
    return errors ? 1 : 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "frosty-draw: %s\n", e.what());
    return 1;
}
