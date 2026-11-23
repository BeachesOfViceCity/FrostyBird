#define NOMINMAX
#include "recompiled_shader.hpp"
#include "shader_client.hpp"
#include "play_timing.hpp"
#include "pipeline_prep.hpp"
#include "xs_ray.hpp"
#include "../src/view_fields.hpp"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <atomic>
#include <map>
#include <mutex>
using Microsoft::WRL::ComPtr;

namespace {
bool trace() {static bool enabled=GetEnvironmentVariableA("FROSTY_TRACE_PSO",nullptr,0)>0;return enabled;}
// The title gives render workers console-sized stacks. The native Vulkan
// driver's compiler and DXIL-to-SPIR-V backend run on the caller's thread.
// Give desktop compilation its own reservation rather than changing UE's
// thread ABI. A worker exit without a returned HRESULT is a failed build.
struct CompileJob {
    ID3D12Device* dev;const D3D12_GRAPHICS_PIPELINE_STATE_DESC* graphics;
    const D3D12_COMPUTE_PIPELINE_STATE_DESC* compute;ID3D12PipelineState** out;
    BOOL depth_bounds=FALSE;
    HRESULT result=E_UNEXPECTED;
    UINT64 key{},other{},start{},end{};bool measured{},variant{};
};
template<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Tag,class T> struct alignas(void*) Subobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE tag=Tag;T value{};
    Subobject(const T& v):value(v){}
};
HRESULT bounded_graphics(CompileJob& j) {
    const auto& d=*j.graphics;
    D3D12_DEPTH_STENCIL_DESC1 ds{};
    static_assert(offsetof(D3D12_DEPTH_STENCIL_DESC1,DepthBoundsTestEnable)==sizeof(d.DepthStencilState));
    std::memcpy(&ds,&d.DepthStencilState,sizeof(d.DepthStencilState));ds.DepthBoundsTestEnable=TRUE;
    D3D12_RT_FORMAT_ARRAY rts{};rts.NumRenderTargets=d.NumRenderTargets;
    std::copy_n(d.RTVFormats,8,rts.RTFormats);
    struct {
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE,ID3D12RootSignature*> root;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS,D3D12_SHADER_BYTECODE> vs;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS,D3D12_SHADER_BYTECODE> ps;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS,D3D12_SHADER_BYTECODE> gs;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT,D3D12_INPUT_LAYOUT_DESC> layout;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE,D3D12_INDEX_BUFFER_STRIP_CUT_VALUE> strip;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY,D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND,D3D12_BLEND_DESC> blend;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER,D3D12_RASTERIZER_DESC> raster;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1,D3D12_DEPTH_STENCIL_DESC1> depth;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT,DXGI_FORMAT> ds_format;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS,D3D12_RT_FORMAT_ARRAY> colors;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC,DXGI_SAMPLE_DESC> sample;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK,UINT> mask;
    } stream{{d.pRootSignature},{d.VS},{d.PS},{d.GS},{d.InputLayout},{d.IBStripCutValue},{d.PrimitiveTopologyType},
             {d.BlendState},{d.RasterizerState},{ds},{d.DSVFormat},{rts},{d.SampleDesc},{d.SampleMask}};
    D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream),&stream};
    ComPtr<ID3D12Device2> v2;auto hr=j.dev->QueryInterface(IID_PPV_ARGS(&v2));
    return FAILED(hr)?hr:v2->CreatePipelineState(&desc,IID_PPV_ARGS(j.out));
}
DWORD WINAPI desktop_compile(void* context) {
    auto& j=*static_cast<CompileJob*>(context);
    if(j.measured)j.start=play_timing::now();
    j.result=j.depth_bounds?bounded_graphics(j):j.graphics?j.dev->CreateGraphicsPipelineState(j.graphics,IID_PPV_ARGS(j.out)):
        j.dev->CreateComputePipelineState(j.compute,IID_PPV_ARGS(j.out));
    if(j.measured)j.end=play_timing::now();
    return 0;
}
HRESULT compile_on_worker(CompileJob job) {
    if(play_timing::offline_bake){desktop_compile(&job);return job.result;}
    play_timing::Scope time(play_timing::pipelines);
    job.measured=time.start!=0;
    struct Workers{
        PTP_POOL pool=CreateThreadpool(nullptr);
        TP_CALLBACK_ENVIRON env{};
        Workers(){
            InitializeThreadpoolEnvironment(&env);
            TP_POOL_STACK_INFORMATION stack{32u<<20,64u<<10};
            if(pool && !SetThreadpoolStackInformation(pool,&stack)){CloseThreadpool(pool);pool=nullptr;}
            if(pool){SetThreadpoolThreadMaximum(pool,6);SetThreadpoolCallbackPool(&env,pool);}
        }
        ~Workers(){if(pool)CloseThreadpool(pool);DestroyThreadpoolEnvironment(&env);}
    };
    static Workers workers;
    if(!workers.pool)return E_OUTOFMEMORY;
    auto task=CreateThreadpoolWork(+[](PTP_CALLBACK_INSTANCE,PVOID data,PTP_WORK){desktop_compile(data);},&job,&workers.env);
    if(!task)return HRESULT_FROM_WIN32(GetLastError());
    SubmitThreadpoolWork(task);WaitForThreadpoolWorkCallbacks(task,FALSE);CloseThreadpoolWork(task);
    if(job.measured){
        static const double hz=[](){LARGE_INTEGER f{};QueryPerformanceFrequency(&f);return double(f.QuadPart);}();
        auto ms=1000.*(play_timing::now()-time.start)/hz;
        if(ms>=100.){
            FILETIME ft{};GetSystemTimePreciseAsFileTime(&ft);UINT64 us=(((UINT64(ft.dwHighDateTime)<<32)|ft.dwLowDateTime)-116444736000000000ull)/10;
            std::fprintf(stderr,"frosty/pso-latency: %s %016llx %016llx %s total %.2f ms, driver %.2f ms, %s end_us=%llu\n",
                job.graphics?"graphics":"compute",job.key,job.other,job.variant?"bound":"base",ms,1000.*(job.end-job.start)/hz,
                play_timing::prewarming?"replay":"demand",us);
        }
    }
    return job.result;
}
constexpr GUID binding_key{0xd427f2a9,0x993e,0x46a9,{0xb5,0x34,0x9c,0x68,0xcd,0x9e,0x02,0x97}};
constexpr GUID uint_key{0xf479f750,0x6485,0x4e87,{0xa1,0x99,0xd8,0xbd,0xa0,0x85,0x55,0xad}};
constexpr GUID sint_key{0xa6cbba9e,0xca9b,0x4514,{0xbb,0x1e,0xf9,0x6d,0x48,0xcc,0x3a,0xf1}};
struct Bindings final:IUnknown{
    std::atomic<ULONG> refs{1};std::shared_ptr<RootViews> value=std::make_shared<RootViews>();
    HRESULT WINAPI QueryInterface(REFIID id,void** out)override{
        if(!out)return E_POINTER;
        *out=nullptr;if(id!=__uuidof(IUnknown))return E_NOINTERFACE;
        *out=this;AddRef();return S_OK;
    }
    ULONG WINAPI AddRef()override{return ++refs;}
    ULONG WINAPI Release()override{auto n=--refs;if(!n)delete this;return n;}
};
}
std::shared_ptr<const RootViews> root_views(ID3D12RootSignature* rs){
    ComPtr<Bindings> owner;UINT n=sizeof(void*);
    return rs && SUCCEEDED(rs->GetPrivateData(binding_key,&n,owner.GetAddressOf())) && owner?owner->value:nullptr;
}
bool static_sampler(const RootViews* m,ViewStage stage,UINT reg,D3D12_SAMPLER_DESC& s){
    if(!m)return false;
    auto it=m->samplers.find({stage,reg});if(it==m->samplers.end())return false;s=it->second;return true;
}
bool table_constant_parameter(const RootViews* m,UINT& index){
    if(!m || m->words==~0u)return false;
    index=m->words;return true;
}
static void record_view_bindings(RootViews&,const void*,SIZE_T);
HRESULT create_shader_root(ID3D12Device* dev,UINT node,const void* bytes,SIZE_T size,REFIID iid,void** out)try{
    // Preserve all original parameter indices and 1.1 descriptor flags.
    // Only compute-visible roots with free DWORDs get the trailing ABI slot.
    ComPtr<ID3D12VersionedRootSignatureDeserializer> reader;ComPtr<ID3DBlob> expanded,err;
    UINT index=~0u;
    if(iid==__uuidof(ID3D12RootSignature) && SUCCEEDED(D3D12CreateVersionedRootSignatureDeserializer(bytes,size,IID_PPV_ARGS(&reader)))){
        auto v=*reader->GetUnconvertedRootSignatureDesc();
        auto extend=[&](auto& desc){
            if(desc.Flags&D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE)return;
            using Param=std::remove_cvref_t<decltype(*desc.pParameters)>;std::vector<Param> p;
            UINT cost=0;bool tables=false;
            for(UINT j=0;j<desc.NumParameters;++j){
                const auto& par=desc.pParameters[j];p.push_back(par);
                if(par.ParameterType==D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE){
                    ++cost;tables|=par.ShaderVisibility==D3D12_SHADER_VISIBILITY_ALL;
                    for(UINT k=0;k<par.DescriptorTable.NumDescriptorRanges;++k)if(par.DescriptorTable.pDescriptorRanges[k].RegisterSpace==frosty::view::table_space)return;
                }else if(par.ParameterType==D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS){
                    if(par.Constants.RegisterSpace==frosty::view::table_space)return;
                    cost+=par.Constants.Num32BitValues;
                }else{if(par.Descriptor.RegisterSpace==frosty::view::table_space)return;cost+=2;}
            }
            if(!tables || cost>64-frosty::view::table_count)return;
            Param extra{};extra.ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            extra.Constants={0,frosty::view::table_space,frosty::view::table_count};
            p.push_back(extra);desc.pParameters=p.data();++desc.NumParameters;
            if(SUCCEEDED(D3D12SerializeVersionedRootSignature(&v,&expanded,&err)))index=desc.NumParameters-1;
        };
        if(v.Version==D3D_ROOT_SIGNATURE_VERSION_1_0)extend(v.Desc_1_0);
        else if(v.Version==D3D_ROOT_SIGNATURE_VERSION_1_1)extend(v.Desc_1_1);
    }
    auto hr=dev->CreateRootSignature(node,expanded?expanded->GetBufferPointer():bytes,expanded?expanded->GetBufferSize():size,iid,out);
    if(SUCCEEDED(hr) && out && *out && iid==__uuidof(ID3D12RootSignature)){
        auto rs=static_cast<ID3D12RootSignature*>(*out);ComPtr<Bindings> owner;
        try{owner.Attach(new Bindings);owner->value->words=index;record_view_bindings(*owner->value,bytes,size);}
        catch(const std::bad_alloc&){rs->Release();*out=nullptr;return E_OUTOFMEMORY;}
        hr=rs->SetPrivateDataInterface(binding_key,owner.Get());if(FAILED(hr)){rs->Release();*out=nullptr;return hr;}
        remember_pipeline_root(rs,node,expanded?expanded->GetBufferPointer():bytes,expanded?expanded->GetBufferSize():size);
    }
    return hr;
}catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
static void record_view_bindings(RootViews& m,const void* data,SIZE_T size) {
    ComPtr<ID3D12VersionedRootSignatureDeserializer> reader;
    if (FAILED(D3D12CreateVersionedRootSignatureDeserializer(data,size,IID_PPV_ARGS(&reader)))) return;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc{};
    if(FAILED(reader->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_0,&desc))) return;
    for(UINT i=0;i<desc->Desc_1_0.NumStaticSamplers;++i){
        auto& st=desc->Desc_1_0.pStaticSamplers[i];if(st.RegisterSpace)continue;
        D3D12_SAMPLER_DESC s{};s.Filter=st.Filter;s.AddressU=st.AddressU;s.AddressV=st.AddressV;s.AddressW=st.AddressW;
        s.MinLOD=st.MinLOD;s.MaxLOD=st.MaxLOD;s.MipLODBias=st.MipLODBias;s.MaxAnisotropy=st.MaxAnisotropy;s.ComparisonFunc=st.ComparisonFunc;
        if(st.BorderColor==D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE)std::fill_n(s.BorderColor,4,1.f);
        else if(st.BorderColor==D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK)s.BorderColor[3]=1.f;
        auto add=[&](ViewStage stg){m.samplers[{stg,st.ShaderRegister}]=s;};
        auto all=st.ShaderVisibility==D3D12_SHADER_VISIBILITY_ALL;
        if(all)add(ViewStage::compute);
        if(all||st.ShaderVisibility==D3D12_SHADER_VISIBILITY_VERTEX)add(ViewStage::vertex);
        if(all||st.ShaderVisibility==D3D12_SHADER_VISIBILITY_PIXEL)add(ViewStage::pixel);
    }
    for(UINT i=0;i<desc->Desc_1_0.NumParameters;++i) {
        const auto& p=desc->Desc_1_0.pParameters[i];
        if(p.ParameterType!=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)continue;
        UINT offset=0;
        std::vector<SrvRange> srvs;
        for(UINT j=0;j<p.DescriptorTable.NumDescriptorRanges;++j) {
            const auto& range=p.DescriptorTable.pDescriptorRanges[j];
            if(range.OffsetInDescriptorsFromTableStart!=D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND) offset=range.OffsetInDescriptorsFromTableStart;
            if(range.RangeType==D3D12_DESCRIPTOR_RANGE_TYPE_SRV || range.RangeType==D3D12_DESCRIPTOR_RANGE_TYPE_UAV)
                srvs.push_back({offset,range.NumDescriptors});
            if(range.RegisterSpace==1 && p.ShaderVisibility==D3D12_SHADER_VISIBILITY_ALL &&
               range.BaseShaderRegister<128 && (range.RangeType==D3D12_DESCRIPTOR_RANGE_TYPE_SRV || range.RangeType==D3D12_DESCRIPTOR_RANGE_TYPE_UAV)){
                for(UINT k=0;k<std::min(range.NumDescriptors,128-range.BaseShaderRegister);++k){
                    UINT reg=(range.BaseShaderRegister+k)|(range.RangeType==D3D12_DESCRIPTOR_RANGE_TYPE_UAV?0x100:0);
                    m.slots[UINT(ViewStage::ray)][RootViews::index(reg)]={i,offset+k};
                }
            }
            if(!range.RegisterSpace && range.BaseShaderRegister<128) {
                for(UINT k=0;k<std::min(range.NumDescriptors,128-range.BaseShaderRegister);++k) {
                    ViewBinding binding{i,offset+k};UINT reg=range.BaseShaderRegister+k;
                    auto add=[&](ViewStage s){m.slots[UINT(s)][RootViews::index(reg)]=binding;};
                    bool all=p.ShaderVisibility==D3D12_SHADER_VISIBILITY_ALL;
                    if(range.RangeType==D3D12_DESCRIPTOR_RANGE_TYPE_UAV || range.RangeType==D3D12_DESCRIPTOR_RANGE_TYPE_SRV) {
                        if(range.RangeType==D3D12_DESCRIPTOR_RANGE_TYPE_UAV)reg|=0x100;
                        if(all)add(ViewStage::compute);
                        if(all || p.ShaderVisibility==D3D12_SHADER_VISIBILITY_VERTEX)add(ViewStage::vertex);
                        if(all || p.ShaderVisibility==D3D12_SHADER_VISIBILITY_PIXEL)add(ViewStage::pixel);
                    }else if(range.RangeType==D3D12_DESCRIPTOR_RANGE_TYPE_CBV || range.RangeType==D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER){
                        reg|=range.RangeType==D3D12_DESCRIPTOR_RANGE_TYPE_CBV?0x200u:0x300u;
                        if(all)add(ViewStage::compute);
                        if(all||p.ShaderVisibility==D3D12_SHADER_VISIBILITY_VERTEX)add(ViewStage::vertex);
                        if(all||p.ShaderVisibility==D3D12_SHADER_VISIBILITY_PIXEL)add(ViewStage::pixel);
                    }
                }
            }
            offset+=range.NumDescriptors;
        }
        if(i<m.ranges.size())m.ranges[i]=std::move(srvs);
    }
}
bool find_view_binding(const RootViews* m,ViewStage stage,UINT reg,ViewBinding& layout) {
    if(!m || (reg&~0x37fu) || UINT(stage)>=m->slots.size())return false;
    layout=m->slots[UINT(stage)][RootViews::index(reg)];return layout.table!=~0u;
}
bool has_view_variants(ID3D12PipelineState* p) {
    UINT n=0;
    return p && SUCCEEDED(p->GetPrivateData(sint_key,&n,nullptr)) && n>0;
}
bool get_typed_views(ID3D12PipelineState* p,TypedViews& v) {
    v.clear();UINT n=0;
    if(!p || FAILED(p->GetPrivateData(sint_key,&n,nullptr)) || !n || n>512*sizeof(TypedView) || n%sizeof(TypedView))return false;
    v.resize(n/sizeof(TypedView));return SUCCEEDED(p->GetPrivateData(sint_key,&n,v.data()));
}
Numeric numeric_format(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32B32_UINT: case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT: case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
    case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R10G10B10A2_UINT: return Numeric::unsigned_int;
    case DXGI_FORMAT_R32_SINT: case DXGI_FORMAT_R32G32_SINT: case DXGI_FORMAT_R32G32B32_SINT: case DXGI_FORMAT_R32G32B32A32_SINT:
    case DXGI_FORMAT_R16_SINT: case DXGI_FORMAT_R16G16_SINT: case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R8_SINT: case DXGI_FORMAT_R8G8_SINT: case DXGI_FORMAT_R8G8B8A8_SINT: return Numeric::signed_int;
    default: return Numeric::floating;
    }
}
UINT64 shader_key(const D3D12_SHADER_BYTECODE& b) {
    if(!b.pShaderBytecode || !b.BytecodeLength || b.BytecodeLength>64*1024*1024)return 0;
    UINT64 key=0xcbf29ce484222325ull;
    for(size_t i=0;i<b.BytecodeLength;++i)key=key*0x100000001b3ull^static_cast<const BYTE*>(b.pShaderBytecode)[i];
    return key;
}
namespace {
HRESULT load_binary(UINT64 key,const wchar_t* extension,ID3DBlob** blob) {
    std::array<wchar_t,32768> dir{};
    auto n=GetEnvironmentVariableW(L"FROSTY_SHADER_PATH",dir.data(),UINT(dir.size()));
    if(!n || n>=dir.size())return S_FALSE;
    wchar_t filename[48];
    std::swprintf(filename,std::size(filename),L"%016llx.%ls",key,extension);
    auto path=std::wstring(dir.data())+L"\\"+filename;
    auto hr=D3DReadFileToBlob(path.c_str(),blob);
    if(hr==HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr==HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))return S_FALSE;
    return hr;
}
}
HRESULT load_graphics_stage(D3D12_SHADER_BYTECODE& stage,bool vs,ID3DBlob** keep) {
    if(!stage.pShaderBytecode || !stage.BytecodeLength || stage.BytecodeLength>64*1024*1024) return S_FALSE;
    std::vector<UINT> bindings;
    auto hr=request_shader(stage,vs?1:0,{},bindings,keep);
    if(hr==S_FALSE)hr=load_binary(shader_key(stage),vs?L"vs.dxil":L"ps.dxil",keep);
    if(hr!=S_OK)return hr;
    stage={(*keep)->GetBufferPointer(),(*keep)->GetBufferSize()};
    return S_OK;
}
UINT shader_compute_wire(ID3D12Device* device){
        UINT wire=5;
        if(device && !GetEnvironmentVariableA("FROSTY_FORCE_VIRTUAL_WAVES",nullptr,0)){
            D3D12_FEATURE_DATA_D3D12_OPTIONS1 caps{};
            D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_6};
            if(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1,&caps,sizeof(caps))) && caps.WaveOps &&
               SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&model,sizeof(model))) && model.HighestShaderModel>=D3D_SHADER_MODEL_6_6){
                if(caps.WaveLaneCountMin<=32 && caps.WaveLaneCountMax>=32)wire|=1u<<8;
                if(caps.WaveLaneCountMin<=64 && caps.WaveLaneCountMax>=64)wire|=2u<<8;
            }
            D3D12_FEATURE_DATA_D3D12_OPTIONS4 extra{};
            if(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4,&extra,sizeof(extra))) && extra.Native16BitShaderOpsSupported &&
               SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&model,sizeof(model))) && model.HighestShaderModel>=D3D_SHADER_MODEL_6_2)wire|=4u<<8;
            char use_ordinal[2]{};
            if((wire&(3u<<8)) && GetEnvironmentVariableA("FROSTY_NATIVE_GROUP_ORDINAL",use_ordinal,sizeof(use_ordinal))==1 && use_ordinal[0]=='1' &&
                xs_enable_intrinsic(device,xs_group_ordinal))wire|=8u<<8;
        }
        return wire;
}
namespace {
struct StageCode {
    UINT stage{},wire{};UINT64 key{};std::wstring name;
    std::vector<BYTE> original;
    std::vector<UINT> views;
    ComPtr<ID3DBlob> base;
    std::map<std::string,ComPtr<ID3DBlob>> compiled;
    D3D12_SHADER_BYTECODE source() const {return {original.data(),original.size()};}
    HRESULT init(D3D12_SHADER_BYTECODE b,UINT s,ID3D12Device* device=nullptr) {
        stage=s;name=s==0?L"ps":s==1?L"vs":s==2?L"gs":L"cs";
        wire=s==5?shader_compute_wire(device):s;
        if(!b.BytecodeLength)return S_OK;
        if(!b.pShaderBytecode || b.BytecodeLength>64*1024*1024)return E_INVALIDARG;
        auto* bytes=static_cast<const BYTE*>(b.pShaderBytecode);original.assign(bytes,bytes+b.BytecodeLength);
        key=shader_key(b);
        if(trace())std::fprintf(stderr,"frosty/compile-begin: %04lx %ls %016llx\n",GetCurrentThreadId(),name.c_str(),key);
        auto hr=request_shader(b,wire,{},views,base.GetAddressOf());
        if(trace())std::fprintf(stderr,"frosty/compile-end: %04lx %ls %016llx %#lx\n",GetCurrentThreadId(),name.c_str(),key,static_cast<unsigned long>(hr));
        if(hr!=S_FALSE)return hr;
        // An offline cache remains usable without the launcher's service.
        hr=load_binary(key,(name+L".dxil").c_str(),base.GetAddressOf());
        if(hr!=S_FALSE)return hr;
        hr=load_binary(key,(name+L".float.dxil").c_str(),base.GetAddressOf());
        if(hr==S_FALSE)return S_OK; // native desktop bytecode
        if(FAILED(hr))return hr;
        ComPtr<ID3DBlob> binding;
        hr=load_binary(key,(name+L".views").c_str(),binding.GetAddressOf());
        if(hr==S_OK) {
            if(binding->GetBufferSize()%4 || binding->GetBufferSize()>256*4)return E_FAIL;
            auto* p=static_cast<const UINT*>(binding->GetBufferPointer());
            views.assign(p,p+binding->GetBufferSize()/4);
        } else if(hr==S_FALSE && stage==0) {
            hr=load_binary(key,L"ps.binding",binding.GetAddressOf());
            if(hr!=S_OK || binding->GetBufferSize()!=4)return E_FAIL;
            views.push_back(*static_cast<const UINT*>(binding->GetBufferPointer()));
        } else if(hr==S_FALSE && (stage==1 || stage==5))views.push_back(stage==5?0x100u:0u);
        else return E_FAIL;
        return S_OK;
    }
    HRESULT code(std::span<const BYTE> types,D3D12_SHADER_BYTECODE& out) {
        out={};if(original.empty())return S_OK;
        if(types.empty() || std::all_of(types.begin(),types.end(),[](BYTE t){return t==0;})) {
            out=base?D3D12_SHADER_BYTECODE{base->GetBufferPointer(),base->GetBufferSize()}:source();return S_OK;
        }
        const std::string which(types.begin(),types.end());
        auto it=compiled.find(which);
        if(it==compiled.end()) {
            ComPtr<ID3DBlob> blob;std::vector<UINT> slots;
            auto hr=request_shader(source(),wire,types,slots,blob.GetAddressOf());
            if(hr==S_FALSE && types.size()==1)
                hr=load_binary(key,(name+(types[0]==1?L".uint.dxil":L".sint.dxil")).c_str(),blob.GetAddressOf());
            else if(hr==S_OK && slots!=views)return E_UNEXPECTED;
            if(hr!=S_OK)return FAILED(hr)?hr:E_NOTIMPL;
            it=compiled.emplace(which,std::move(blob)).first;
        }
        out={it->second->GetBufferPointer(),it->second->GetBufferSize()};return S_OK;
    }
};
// Own all non-scalar fields of the PSO description. The title can release its
// original bytecode, semantic names and root immediately after PSO creation.
struct Recipe final : IUnknown {
    std::atomic<ULONG> refs{1};
    std::mutex lock,build_lock;
    bool compute{};
    BOOL bounds{};
    ComPtr<ID3D12Device> dev;ComPtr<ID3D12RootSignature> root;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};
    D3D12_COMPUTE_PIPELINE_STATE_DESC cs_desc{};
    StageCode vs,ps,gs,cs;
    ComPtr<ID3DBlob> rect_assembly;
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;std::vector<std::string> names;
    TypedViews views;
    std::wstring hint_path;
    std::map<std::string,ComPtr<ID3D12PipelineState>> variants;
    HRESULT WINAPI QueryInterface(REFIID iid,void** out) override {
        if(!out)return E_POINTER;
        *out=nullptr;if(iid!=__uuidof(IUnknown))return E_NOINTERFACE;*out=this;AddRef();return S_OK;
    }
    ULONG WINAPI AddRef() override{return ++refs;}
    ULONG WINAPI Release() override{auto n=--refs;if(!n)delete this;return n;}
    HRESULT build(std::span<const BYTE> types,ID3D12PipelineState** out) {
        if(compute) {
            auto d=cs_desc;auto hr=cs.code(types,d.CS);if(FAILED(hr))return hr;
            if(trace())std::fprintf(stderr,"frosty/pso-begin: %04lx CS %016llx\n",GetCurrentThreadId(),cs.key);
            CompileJob job{dev.Get(),nullptr,&d,out};job.key=cs.key;job.variant=!types.empty();hr=compile_on_worker(job);
            if(trace())std::fprintf(stderr,"frosty/pso-end: %04lx CS %016llx %#lx\n",GetCurrentThreadId(),cs.key,static_cast<unsigned long>(hr));
            return hr;
        }
        auto d=graphics;
        auto hr=vs.code(types.empty()?types:types.first(vs.views.size()),d.VS);
        if(SUCCEEDED(hr))hr=ps.code(types.empty()?types:types.subspan(vs.views.size()),d.PS);
        if(SUCCEEDED(hr))hr=gs.code({},d.GS);
        if(FAILED(hr))return hr;
        if(rect_assembly)d.GS={rect_assembly->GetBufferPointer(),rect_assembly->GetBufferSize()};
        if(trace())std::fprintf(stderr,"frosty/pso-begin: %04lx graphics %016llx %016llx\n",GetCurrentThreadId(),vs.key,ps.key);
        CompileJob job{dev.Get(),&d,nullptr,out,bounds};job.key=vs.key;job.other=ps.key;job.variant=!types.empty();hr=compile_on_worker(job);
        if(trace())std::fprintf(stderr,"frosty/pso-end: %04lx graphics %016llx %016llx %#lx\n",GetCurrentThreadId(),vs.key,ps.key,static_cast<unsigned long>(hr));
        return hr;
    }
    HRESULT initial(ID3D12PipelineState** out) {
        auto add=[&](StageCode& s,ViewStage stage){for(UINT slot:s.views)views.push_back({stage,slot});};
        if(compute)add(cs,ViewStage::compute);else {add(vs,ViewStage::vertex);add(ps,ViewStage::pixel);}
        ComPtr<ID3D12PipelineState> base;auto hr=build({},base.GetAddressOf());if(FAILED(hr))return hr;
        if(!views.empty()) {
            hr=base->SetPrivateData(sint_key,UINT(views.size()*sizeof(views[0])),views.data());
            if(SUCCEEDED(hr))hr=base->SetPrivateDataInterface(uint_key,this);
        }
        if(SUCCEEDED(hr))*out=base.Detach();
        return hr;
    }
    HRESULT lookup(std::span<const BYTE> types,ID3D12PipelineState** out,bool learn) {
        if(types.size()!=views.size())return E_INVALIDARG;
        std::string sig(types.begin(),types.end());
        auto hit=[&]{std::lock_guard g(lock);auto it=variants.find(sig);
            if(it==variants.end())return false;
            it->second.CopyTo(out);return true;};
        if(hit())return S_OK;
        // A different in-flight specialization can take seconds. Do not
        // hold the lookup lock over driver compilation; only misses serialize
        // access to this recipe's mutable StageCode maps.
        std::lock_guard builder(build_lock);if(hit())return S_OK;
        ComPtr<ID3D12PipelineState> p;auto hr=build(types,p.GetAddressOf());if(FAILED(hr))return hr;
        std::wstring path;
        {std::lock_guard g(lock);variants.emplace(std::move(sig),p);if(learn)path=hint_path;}
        pipeline_view_hint(path,types);return p.CopyTo(out);
    }
};
}
void set_pipeline_hint_path(ID3D12PipelineState* p,const std::wstring& path){
    ComPtr<Recipe> recipe;UINT n=sizeof(void*);
    if(p && SUCCEEDED(p->GetPrivateData(uint_key,&n,recipe.GetAddressOf())) && recipe){
        std::lock_guard g(recipe->lock);recipe->hint_path=path;
    }
}
HRESULT select_view_variant(ID3D12PipelineState* p,std::span<const BYTE> types,ID3D12PipelineState** out,bool learn) try {
    if(std::all_of(types.begin(),types.end(),[](BYTE x){return !x;}))return p->QueryInterface(IID_PPV_ARGS(out));
    ComPtr<Recipe> recipe;UINT n=sizeof(void*);
    auto hr=p->GetPrivateData(uint_key,&n,recipe.GetAddressOf());
    return FAILED(hr)?hr:recipe->lookup(types,out,learn);
} catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
HRESULT make_compute_variants(ID3D12Device* dev,const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc,UINT64,ID3D12PipelineState** out) try {
    ComPtr<Recipe> r;r.Attach(new Recipe);r->dev=dev;r->cs_desc=desc;r->root=desc.pRootSignature;r->compute=true;
    r->cs_desc.CachedPSO={};
    auto hr=r->cs.init(desc.CS,5,dev);return FAILED(hr)?hr:r->initial(out);
} catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
HRESULT make_graphics_variants(ID3D12Device* dev,const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,ID3D12PipelineState** out,BOOL depth_bounds) try {
    ComPtr<Recipe> r;r.Attach(new Recipe);r->dev=dev;r->graphics=desc;r->root=desc.pRootSignature;r->graphics.CachedPSO={};
    r->bounds=depth_bounds;
    if(desc.StreamOutput.NumEntries || desc.StreamOutput.NumStrides || desc.HS.BytecodeLength || desc.DS.BytecodeLength)return E_NOTIMPL;
    if(desc.InputLayout.NumElements) {
        if(!desc.InputLayout.pInputElementDescs || desc.InputLayout.NumElements>32)return E_INVALIDARG;
        r->elements.assign(desc.InputLayout.pInputElementDescs,desc.InputLayout.pInputElementDescs+desc.InputLayout.NumElements);
        for(auto& e:r->elements){if(!e.SemanticName)return E_INVALIDARG;r->names.emplace_back(e.SemanticName);}
        for(size_t i=0;i<r->names.size();++i)r->elements[i].SemanticName=r->names[i].c_str();
        r->graphics.InputLayout.pInputElementDescs=r->elements.data();
    }
    auto hr=r->vs.init(desc.VS,1);if(SUCCEEDED(hr))hr=r->ps.init(desc.PS,0);if(SUCCEEDED(hr))hr=r->gs.init(desc.GS,2);
    if(SUCCEEDED(hr) && UINT(desc.PrimitiveTopologyType)==xs_rectangle_pipeline_type){
        if(desc.GS.BytecodeLength)return E_NOTIMPL;
        std::vector<UINT> slots;
        hr=request_shader(desc.VS,129,{},slots,r->rect_assembly.GetAddressOf());
        if(hr==S_FALSE)hr=load_binary(r->vs.key,L"rect.gs.dxil",r->rect_assembly.GetAddressOf());
        if(hr!=S_OK || !slots.empty())return FAILED(hr)?hr:E_NOTIMPL;
        r->graphics.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
    if(!r->gs.views.empty())return E_NOTIMPL;
    return FAILED(hr)?hr:r->initial(out);
} catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
