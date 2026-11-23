// Exercise the retail XS device/list slots, including batched BLAS, TLAS,
// descriptor-table AS SRV, and hardware ray-query hit/miss readback.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include "xs_ray.hpp"
#include <wrl/client.h>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>
extern "C" HRESULT WINAPI D3D12XboxCreateDevice(IUnknown*,const UINT*,REFIID,void**);
bool check_shadow(ID3D12Resource*,const std::filesystem::path&);
using Microsoft::WRL::ComPtr;
namespace {
struct XS : IUnknown {};
template<class Fn> Fn fn(XS* x,UINT i){return std::bit_cast<Fn>((*reinterpret_cast<void***>(x))[i]);}
void ok(HRESULT hr){if(FAILED(hr)){std::fprintf(stderr,"hwrt-check HRESULT %#lx\n",(unsigned long)hr);throw std::runtime_error("XS device call");}}
std::vector<char> read(std::filesystem::path p){std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("missing fixture");return {std::istreambuf_iterator<char>(f),{}};}
struct Hit{UINT instance,primitive;float t,u,v;};
struct Record{UINT ray[4];Hit expected;};
static_assert(sizeof(Hit)==20 && sizeof(Record)==36);
}
int main(int argc,char** argv)try{
    if(argc!=2)return 2;
    auto dir=std::filesystem::path(argv[1]);auto data=read(dir/"cube-rays.bin"),code=read(dir/"probe.dxil");
    std::ifstream manifest(dir/"occlusion.libs");std::string rgfile,rgname,msfile,msname,chfile,chname;
    if(!(manifest>>rgfile>>rgname>>msfile>>msname))throw std::runtime_error("missing retail ray libraries");
    bool intersection=bool(manifest>>chfile>>chname);UINT result_stride=intersection?20:4;
    UINT h[3]{};if(data.size()<sizeof(h))return 2;std::memcpy(h,data.data(),sizeof(h));auto [nv,ni,nr]=h;
    if(!nv || !ni || ni%3 || !nr || nr>65535 || data.size()!=12+12ull*nv+4ull*ni+sizeof(Record)*nr)return 2;
    std::vector<Record> records(nr);std::memcpy(records.data(),data.data()+12+12ull*nv+4ull*ni,sizeof(Record)*nr);
    std::vector<char> pos_ref;if(std::filesystem::exists(dir/"positions.ref"))pos_ref=read(dir/"positions.ref");
    if(!pos_ref.empty() && pos_ref.size()!=36ull*nr)throw std::runtime_error("invalid vertex oracle");
    UINT query_stride=pos_ref.empty()?sizeof(Hit):96;
    ComPtr<XS> root,dev,q,cl,l4,l6,heap,pso;
    const std::array<UINT,32> params{0x20004};
    ok(D3D12XboxCreateDevice(nullptr,params.data(),__uuidof(ID3D12Device),(void**)root.GetAddressOf()));
    ok(root->QueryInterface(__uuidof(ID3D12Device5),(void**)dev.GetAddressOf()));
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 caps{};
    ok(fn<HRESULT(*)(XS*,D3D12_FEATURE,void*,UINT)>(root.Get(),13)(root.Get(),D3D12_FEATURE_D3D12_OPTIONS5,&caps,sizeof(caps)));
    if(caps.RaytracingTier<D3D12_RAYTRACING_TIER_1_1)throw std::runtime_error("inline tracing requires tier 1.1");
    D3D12_COMMAND_QUEUE_DESC qd{};ok(fn<HRESULT(*)(XS*,const D3D12_COMMAND_QUEUE_DESC*,REFIID,void**)>(root.Get(),8)(root.Get(),&qd,__uuidof(ID3D12CommandQueue),(void**)q.GetAddressOf()));
    ComPtr<ID3D12CommandAllocator> alloc;
    ok(fn<HRESULT(*)(XS*,D3D12_COMMAND_LIST_TYPE,REFIID,void**)>(root.Get(),9)(root.Get(),qd.Type,IID_PPV_ARGS(&alloc)));
    const UINT ld[]{0,0,1,0};
    ok(fn<HRESULT(*)(XS*,const UINT*,ID3D12CommandAllocator*,ID3D12PipelineState*,REFIID,void**)>(root.Get(),73)(root.Get(),ld,alloc.Get(),nullptr,__uuidof(ID3D12GraphicsCommandList),(void**)cl.GetAddressOf()));
    ok(cl->QueryInterface(__uuidof(ID3D12GraphicsCommandList4),(void**)l4.GetAddressOf()));
    ok(cl->QueryInterface(xs_list6,(void**)l6.GetAddressOf()));
    auto buffer=[&](D3D12_HEAP_TYPE type,UINT64 size,D3D12_RESOURCE_STATES state,bool uav=false){
        D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_BUFFER,0,size,1,1,1,DXGI_FORMAT_UNKNOWN,{1,0},D3D12_TEXTURE_LAYOUT_ROW_MAJOR,uav?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE};
        D3D12_HEAP_PROPERTIES hp{};hp.Type=type;hp.CreationNodeMask=hp.VisibleNodeMask=1;ComPtr<ID3D12Resource> res;
        ok(fn<HRESULT(*)(XS*,const D3D12_HEAP_PROPERTIES*,D3D12_HEAP_FLAGS,const D3D12_RESOURCE_DESC*,D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,REFIID,void**)>(root.Get(),27)(root.Get(),&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&res)));return res;};
    auto upload=[&](const void* src,UINT64 n){auto b=buffer(D3D12_HEAP_TYPE_UPLOAD,n,D3D12_RESOURCE_STATE_GENERIC_READ);void* p{};ok(b->Map(0,nullptr,&p));std::memcpy(p,src,n);b->Unmap(0,nullptr);return b;};
    auto verts=upload(data.data()+12,12ull*nv),idx=upload(data.data()+12+12ull*nv,4ull*ni);
    auto barrier=[&](D3D12_RESOURCE_BARRIER b){fn<void(*)(XS*,UINT,const D3D12_RESOURCE_BARRIER*)>(cl.Get(),26)(cl.Get(),1,&b);};
    std::vector<ComPtr<ID3D12Resource>> keep;
    ComPtr<ID3D12Resource> compression_read;
    constexpr UINT state_w=9,state_h=3,state_pitch=256;
    auto texel=[](UINT x,UINT y){return 0xff123400u^(0x010301u*x)^(0x071100u*y);};
    if(std::filesystem::is_regular_file(dir/"shadow.libs")){
        D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D,0,state_w,state_h,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},
            D3D12_TEXTURE_LAYOUT_UNKNOWN,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;hp.CreationNodeMask=hp.VisibleNodeMask=1;ComPtr<ID3D12Resource> image;
        ok(fn<HRESULT(*)(XS*,const D3D12_HEAP_PROPERTIES*,D3D12_HEAP_FLAGS,const D3D12_RESOURCE_DESC*,D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,REFIID,void**)>(root.Get(),27)(
            root.Get(),&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&image)));
        std::array<UINT,state_h*state_pitch/4> rgba{};for(UINT y=0;y<state_h;++y)for(UINT x=0;x<state_w;++x)rgba[y*state_pitch/4+x]=texel(x,y);
        auto src=upload(rgba.data(),sizeof(rgba));compression_read=buffer(D3D12_HEAP_TYPE_READBACK,sizeof(rgba),D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION fp{},rt{};fp.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;fp.PlacedFootprint={0,{desc.Format,state_w,state_h,1,state_pitch}};
        fp.pResource=src.Get();rt.pResource=image.Get();
        auto copy=fn<void(*)(XS*,const D3D12_TEXTURE_COPY_LOCATION*,UINT,UINT,UINT,const D3D12_TEXTURE_COPY_LOCATION*,const D3D12_BOX*)>(cl.Get(),16);
        copy(cl.Get(),&rt,0,0,0,&fp,nullptr);
        auto srv=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        auto xs=static_cast<D3D12_RESOURCE_STATES>(UINT(srv)|0x12000000u);
        D3D12_RESOURCE_BARRIER b{};b.Transition={image.Get(),0,D3D12_RESOURCE_STATE_COPY_DEST,xs};barrier(b);
        b.Transition.StateBefore=xs;b.Transition.StateAfter=srv;barrier(b); // only the compression hint changes
        b.Transition.StateBefore=srv;b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;barrier(b);
        fp.pResource=compression_read.Get();copy(cl.Get(),&fp,0,0,0,&rt,nullptr);keep.insert(keep.end(),{image,src});
    }
    auto build=[&](D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in,bool batch){
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        fn<void(*)(XS*,const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS*,D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO*)>(dev.Get(),102)(dev.Get(),&in,&info);
        if(!info.ScratchDataSizeInBytes || !info.ResultDataMaxSizeInBytes)throw std::runtime_error("empty prebuild");
        auto as=buffer(D3D12_HEAP_TYPE_DEFAULT,info.ResultDataMaxSizeInBytes,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,true);
        auto scratch=buffer(D3D12_HEAP_TYPE_DEFAULT,info.ScratchDataSizeInBytes,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);
        XsBatchBuild b{};b.build={as->GetGPUVirtualAddress(),in,0,scratch->GetGPUVirtualAddress()};
        if(batch)fn<void(*)(XS*,UINT,const XsBatchBuild*)>(l6.Get(),131)(l6.Get(),1,&b);
        else fn<void(*)(XS*,const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC*,UINT,const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC*)>(l4.Get(),117)(l4.Get(),&b.build,0,nullptr);
        D3D12_RESOURCE_BARRIER bar{};bar.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;bar.UAV.pResource=as.Get();barrier(bar);keep.push_back(scratch);
        std::printf("%s: result %llu bytes, scratch %llu\n",batch?"BLAS":"TLAS",info.ResultDataMaxSizeInBytes,info.ScratchDataSizeInBytes);return as;};
    D3D12_RAYTRACING_GEOMETRY_DESC geom{};geom.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;geom.Flags=D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geom.Triangles={0,DXGI_FORMAT_R32_UINT,DXGI_FORMAT_R32G32B32_FLOAT,ni,nv,idx->GetGPUVirtualAddress(),{verts->GetGPUVirtualAddress(),12}};
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};in.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;in.NumDescs=1;in.pGeometryDescs=&geom;
    auto blas=build(in,true);
    D3D12_RAYTRACING_INSTANCE_DESC instances[2]{};
    for(UINT i=0;i<2;++i){auto& v=instances[i];v.Transform[0][0]=v.Transform[1][1]=v.Transform[2][2]=1;
        v.Transform[0][3]=float(i*200);v.Transform[2][3]=float(i*80);v.InstanceID=i?41:17;v.InstanceMask=1u<<i;v.AccelerationStructure=blas->GetGPUVirtualAddress();}
    if(intersection)std::swap(instances[0],instances[1]);
    auto instance_data=upload(instances,sizeof(instances));in.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;in.NumDescs=2;in.InstanceDescs=instance_data->GetGPUVirtualAddress();
    auto tlas=build(in,false);
    auto output=buffer(D3D12_HEAP_TYPE_DEFAULT,nr*query_stride,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);
    auto readback=buffer(D3D12_HEAP_TYPE_READBACK,nr*query_stride,D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<UINT> rays;for(auto& rec:records)rays.insert(rays.end(),rec.ray,rec.ray+4);
    auto ray_data=upload(rays.data(),rays.size()*4);
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,3,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    ok(fn<HRESULT(*)(XS*,const D3D12_DESCRIPTOR_HEAP_DESC*,REFIID,void**)>(root.Get(),14)(root.Get(),&hd,__uuidof(ID3D12DescriptorHeap),(void**)heap.GetAddressOf()));
    auto stride=fn<UINT(*)(XS*,D3D12_DESCRIPTOR_HEAP_TYPE)>(root.Get(),15)(root.Get(),hd.Type);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{fn<SIZE_T(*)(XS*)>(heap.Get(),9)(heap.Get())};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{fn<UINT64(*)(XS*)>(heap.Get(),10)(heap.Get())};
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension=D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;srv.RaytracingAccelerationStructure.Location=tlas->GetGPUVirtualAddress();
    auto srv_call=fn<void(*)(XS*,ID3D12Resource*,const D3D12_SHADER_RESOURCE_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE)>(root.Get(),18);
    srv_call(root.Get(),nullptr,&srv,cpu);cpu.ptr+=stride;srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;srv.Format=DXGI_FORMAT_R32_TYPELESS;
    srv.Buffer={0,UINT(rays.size()),0,D3D12_BUFFER_SRV_FLAG_RAW};srv_call(root.Get(),ray_data.Get(),&srv,cpu);cpu.ptr+=stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R32_TYPELESS;uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements=nr*query_stride/4;uav.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;
    fn<void(*)(XS*,ID3D12Resource*,ID3D12Resource*,const D3D12_UNORDERED_ACCESS_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE)>(root.Get(),19)(root.Get(),output.Get(),nullptr,&uav,cpu);
    D3D12_DESCRIPTOR_RANGE ranges[]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,2}};
    D3D12_ROOT_PARAMETER param{};param.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;param.DescriptorTable={2,ranges};
    D3D12_ROOT_SIGNATURE_DESC rd{1,&param,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> blob;
    ok(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,nullptr));ComPtr<ID3D12RootSignature> rs;
    ok(fn<HRESULT(*)(XS*,UINT,const void*,SIZE_T,REFIID,void**)>(root.Get(),16)(root.Get(),0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&rs)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.CS={code.data(),code.size()};pd.pRootSignature=rs.Get();
    ok(fn<HRESULT(*)(XS*,const D3D12_COMPUTE_PIPELINE_STATE_DESC*,const void*,UINT,REFIID,void**)>(root.Get(),62)(root.Get(),&pd,nullptr,0,__uuidof(ID3D12PipelineState),(void**)pso.GetAddressOf()));
    auto* heap_ptr=heap.Get();fn<void(*)(XS*,UINT,XS* const*)>(cl.Get(),32)(cl.Get(),1,&heap_ptr);
    fn<void(*)(XS*,XS*)>(cl.Get(),25)(cl.Get(),pso.Get());fn<void(*)(XS*,ID3D12RootSignature*)>(cl.Get(),33)(cl.Get(),rs.Get());
    fn<void(*)(XS*,UINT,D3D12_GPU_DESCRIPTOR_HANDLE)>(cl.Get(),35)(cl.Get(),0,gpu);
    fn<void(*)(XS*,UINT,UINT,UINT)>(cl.Get(),14)(cl.Get(),nr,1,1);
    D3D12_RESOURCE_BARRIER trans{};trans.Transition={output.Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};barrier(trans);
    fn<void(*)(XS*,ID3D12Resource*,UINT64,ID3D12Resource*,UINT64,UINT64)>(cl.Get(),15)(cl.Get(),readback.Get(),0,output.Get(),0,nr*query_stride);
    // Dispatch the actual title raygen/continuation and miss shader on the
    // same AS, using their serialized register space and structured view ABI.
    auto expected=read(dir/"occlusion.ref");
    if(expected.size()!=result_stride*nr)throw std::runtime_error("bad retail ray oracle");
    std::vector<float> inputs;
    for(auto& rec:records)inputs.insert(inputs.end(),{std::bit_cast<float>(rec.ray[0]),std::bit_cast<float>(rec.ray[1]),-160.f,0,0,0,1,std::bit_cast<float>(rec.ray[2])});
    auto ray_input=upload(inputs.data(),inputs.size()*4);
    auto occ=buffer(D3D12_HEAP_TYPE_DEFAULT,result_stride*nr,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);
    auto occ_read=buffer(D3D12_HEAP_TYPE_READBACK,result_stride*nr,D3D12_RESOURCE_STATE_COPY_DEST);
    ComPtr<XS> rh;ok(fn<HRESULT(*)(XS*,const D3D12_DESCRIPTOR_HEAP_DESC*,REFIID,void**)>(root.Get(),14)(root.Get(),&hd,__uuidof(ID3D12DescriptorHeap),(void**)rh.GetAddressOf()));
    cpu={fn<SIZE_T(*)(XS*)>(rh.Get(),9)(rh.Get())};gpu={fn<UINT64(*)(XS*)>(rh.Get(),10)(rh.Get())};
    srv={};srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension=D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;srv.RaytracingAccelerationStructure.Location=tlas->GetGPUVirtualAddress();
    srv_call(root.Get(),nullptr,&srv,cpu);cpu.ptr+=stride;srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
    srv.Buffer={0,nr,32,D3D12_BUFFER_SRV_FLAG_NONE};srv_call(root.Get(),ray_input.Get(),&srv,cpu);cpu.ptr+=stride;
    uav={};uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;uav.Buffer.NumElements=nr;uav.Buffer.StructureByteStride=result_stride;
    fn<void(*)(XS*,ID3D12Resource*,ID3D12Resource*,const D3D12_UNORDERED_ACCESS_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE)>(root.Get(),19)(root.Get(),occ.Get(),nullptr,&uav,cpu);
    ranges[0].RegisterSpace=ranges[1].RegisterSpace=1;
    if(intersection)ranges[1].BaseShaderRegister=1;
    blob.Reset();ok(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,nullptr));ComPtr<ID3D12RootSignature> rtroot;
    ok(fn<HRESULT(*)(XS*,UINT,const void*,SIZE_T,REFIID,void**)>(root.Get(),16)(root.Get(),0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&rtroot)));
    ComPtr<XS> dx8;ok(root->QueryInterface(xs_serialized_device,(void**)dx8.GetAddressOf()));
    ComPtr<ID3D12StateObject> collections[3];
    for(UINT i=0;i<(intersection?3u:2u);++i){
        const auto& file=i==0?rgfile:i==1?msfile:chfile;auto stem=file.substr(0,file.find('.'));auto xs=read(dir/(stem+".xsrt"));
        ok(fn<HRESULT(*)(XS*,const void*,UINT,ID3D12RootSignature*,UINT,REFIID,void**)>(dx8.Get(),115)(
            dx8.Get(),xs.data(),UINT(xs.size()),rtroot.Get(),0,IID_PPV_ARGS(&collections[i])));
    }
    D3D12_EXISTING_COLLECTION_DESC libs[]{{collections[0].Get(),0,nullptr},{collections[1].Get(),0,nullptr},{collections[2].Get(),0,nullptr}};
    D3D12_RAYTRACING_SHADER_CONFIG config{24,8};D3D12_RAYTRACING_PIPELINE_CONFIG depth{1};
    D3D12_GLOBAL_ROOT_SIGNATURE global{rtroot.Get()};D3D12_HIT_GROUP_DESC empty_hit{};empty_hit.HitGroupExport=L"OpaqueTriangles";
    D3D12_STATE_SUBOBJECT subs[]{{D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION,libs},{D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION,libs+1},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG,&config},{D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG,&depth},
        {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,&global},{D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,&empty_hit}};
    if(intersection)subs[5]={D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION,libs+2};
    D3D12_STATE_OBJECT_DESC so{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,UINT(std::size(subs)),subs};ComPtr<ID3D12StateObject> pipeline;
    auto create_state=fn<HRESULT(*)(XS*,const D3D12_STATE_OBJECT_DESC*,REFIID,void**)>(dev.Get(),101);
    if(!pos_ref.empty()){
        D3D12_STATE_OBJECT_CONFIG flags{D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS};
        D3D12_STATE_SUBOBJECT conf[]{subs[2],subs[3],subs[4],{D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG,&flags}};
        D3D12_STATE_OBJECT_DESC seed_desc{so.Type,UINT(std::size(conf)),conf};ComPtr<ID3D12StateObject> seed;
        ok(create_state(dev.Get(),&seed_desc,IID_PPV_ARGS(&seed)));
        D3D12_STATE_SUBOBJECT add[]{subs[0],subs[1],subs[5]};
        D3D12_STATE_OBJECT_DESC added{so.Type,UINT(std::size(add)),add};
        ComPtr<XS> rev7;ok(root->QueryInterface(__uuidof(ID3D12Device7),(void**)rev7.GetAddressOf()));
        ok(fn<HRESULT(*)(XS*,const D3D12_STATE_OBJECT_DESC*,ID3D12StateObject*,REFIID,void**)>(rev7.Get(),106)(
            rev7.Get(),&added,seed.Get(),IID_PPV_ARGS(&pipeline)));
        std::puts("XS AddToStateObject: linked retail collections into the configured empty pipeline");
    }else ok(create_state(dev.Get(),&so,IID_PPV_ARGS(&pipeline)));
    ComPtr<ID3D12StateObjectProperties> props;ok(pipeline.As(&props));std::array<BYTE,192> sbt{};
    UINT slot=0;for(const auto& name:{std::wstring(rgname.begin(),rgname.end()),std::wstring(msname.begin(),msname.end()),intersection?std::wstring(chname.begin(),chname.end()):std::wstring(L"OpaqueTriangles")}){
        auto* id=props->GetShaderIdentifier(name.c_str());if(!id)throw std::runtime_error("missing native shader identifier");
        std::memcpy(sbt.data()+64*slot++,id,D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);}
    auto table=upload(sbt.data(),sbt.size());auto base=table->GetGPUVirtualAddress();
    D3D12_DISPATCH_RAYS_DESC dispatch{{base,32},{base+64,32,32},{base+128,32,32},{},nr,1,1};
    heap_ptr=rh.Get();fn<void(*)(XS*,UINT,XS* const*)>(cl.Get(),32)(cl.Get(),1,&heap_ptr);
    fn<void(*)(XS*,ID3D12RootSignature*)>(cl.Get(),33)(cl.Get(),rtroot.Get());
    fn<void(*)(XS*,UINT,D3D12_GPU_DESCRIPTOR_HANDLE)>(cl.Get(),35)(cl.Get(),0,gpu);
    fn<void(*)(XS*,ID3D12StateObject*)>(l4.Get(),120)(l4.Get(),pipeline.Get());
    fn<void(*)(XS*,const D3D12_DISPATCH_RAYS_DESC*)>(l4.Get(),121)(l4.Get(),&dispatch);
    trans.Transition.pResource=occ.Get();barrier(trans);
    fn<void(*)(XS*,ID3D12Resource*,UINT64,ID3D12Resource*,UINT64,UINT64)>(cl.Get(),15)(cl.Get(),occ_read.Get(),0,occ.Get(),0,nr*result_stride);
    constexpr UINT gw=7,gh=5;
    ComPtr<ID3D12Resource> guard_read;ComPtr<XS> guard_heap;ComPtr<ID3D12RootSignature> guard_root;
    ComPtr<ID3D12StateObject> guard_state;
    if(std::filesystem::exists(dir/"launch-guard.dxil")){
        auto code=read(dir/"launch-guard.dxil");
        std::array<UINT,64> cb0{};std::array<UINT,576> cb1{};
        cb0[12]=3;cb0[13]=6;cb0[14]=5;cb0[15]=8; // inclusive light viewport
        cb0[16]=UINT(-1);cb0[17]=2;              // signed dispatch offset
        cb1[0x850/4]=std::bit_cast<UINT>(2.5f);cb1[0x854/4]=std::bit_cast<UINT>(3.25f);
        auto c0=upload(cb0.data(),sizeof(cb0)),c1=upload(cb1.data(),sizeof(cb1));
        auto target=buffer(D3D12_HEAP_TYPE_DEFAULT,gw*gh*4,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);
        guard_read=buffer(D3D12_HEAP_TYPE_READBACK,gw*gh*4,D3D12_RESOURCE_STATE_COPY_DEST);
        ok(fn<HRESULT(*)(XS*,const D3D12_DESCRIPTOR_HEAP_DESC*,REFIID,void**)>(root.Get(),14)(root.Get(),&hd,__uuidof(ID3D12DescriptorHeap),(void**)guard_heap.GetAddressOf()));
        cpu={fn<SIZE_T(*)(XS*)>(guard_heap.Get(),9)(guard_heap.Get())};gpu={fn<UINT64(*)(XS*)>(guard_heap.Get(),10)(guard_heap.Get())};
        D3D12_CONSTANT_BUFFER_VIEW_DESC cv{c0->GetGPUVirtualAddress(),sizeof(cb0)};
        auto cbv=fn<void(*)(XS*,const D3D12_CONSTANT_BUFFER_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE)>(root.Get(),17);
        cbv(root.Get(),&cv,cpu);cpu.ptr+=stride;cv={c1->GetGPUVirtualAddress(),sizeof(cb1)};cbv(root.Get(),&cv,cpu);cpu.ptr+=stride;
        uav={};uav.Format=DXGI_FORMAT_R32_TYPELESS;uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;uav.Buffer.NumElements=gw*gh;uav.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;
        fn<void(*)(XS*,ID3D12Resource*,ID3D12Resource*,const D3D12_UNORDERED_ACCESS_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE)>(root.Get(),19)(root.Get(),target.Get(),nullptr,&uav,cpu);
        ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_CBV,2,0,1,0};ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,31,2};
        blob.Reset();ok(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,nullptr));
        ok(fn<HRESULT(*)(XS*,UINT,const void*,SIZE_T,REFIID,void**)>(root.Get(),16)(root.Get(),0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&guard_root)));
        D3D12_DXIL_LIBRARY_DESC lib{{code.data(),code.size()},0,nullptr};D3D12_GLOBAL_ROOT_SIGNATURE gr{guard_root.Get()};
        D3D12_STATE_SUBOBJECT gs[]{{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,&lib},subs[2],subs[3],{D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,&gr}};
        D3D12_STATE_OBJECT_DESC gd{so.Type,UINT(std::size(gs)),gs};ok(create_state(dev.Get(),&gd,IID_PPV_ARGS(&guard_state)));
        ComPtr<ID3D12StateObjectProperties> gp;ok(guard_state.As(&gp));auto id=gp->GetShaderIdentifier(L"MeasureLaunchGuard");
        if(!id)throw std::runtime_error("missing measurement raygen");
        auto record=upload(id,D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        heap_ptr=guard_heap.Get();fn<void(*)(XS*,UINT,XS* const*)>(cl.Get(),32)(cl.Get(),1,&heap_ptr);
        fn<void(*)(XS*,ID3D12RootSignature*)>(cl.Get(),33)(cl.Get(),guard_root.Get());
        fn<void(*)(XS*,UINT,D3D12_GPU_DESCRIPTOR_HANDLE)>(cl.Get(),35)(cl.Get(),0,gpu);
        fn<void(*)(XS*,ID3D12StateObject*)>(l4.Get(),120)(l4.Get(),guard_state.Get());
        D3D12_DISPATCH_RAYS_DESC probe{{record->GetGPUVirtualAddress(),32},{},{},{},gw,gh,1};
        fn<void(*)(XS*,const D3D12_DISPATCH_RAYS_DESC*)>(l4.Get(),121)(l4.Get(),&probe);
        trans.Transition.pResource=target.Get();barrier(trans);
        fn<void(*)(XS*,ID3D12Resource*,UINT64,ID3D12Resource*,UINT64,UINT64)>(cl.Get(),15)(cl.Get(),guard_read.Get(),0,target.Get(),0,gw*gh*4);
        keep.insert(keep.end(),{c0,c1,target,record});
    }
    ok(fn<HRESULT(*)(XS*)>(cl.Get(),9)(cl.Get()));auto* cmd=cl.Get();fn<void(*)(XS*,UINT,XS* const*)>(q.Get(),10)(q.Get(),1,&cmd);
    ComPtr<ID3D12Fence> fence;ok(fn<HRESULT(*)(XS*,UINT64,D3D12_FENCE_FLAGS,REFIID,void**)>(root.Get(),36)(root.Get(),0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    ok(fn<HRESULT(*)(XS*,ID3D12Fence*,UINT64)>(q.Get(),14)(q.Get(),fence.Get(),1));ok(fence->SetEventOnCompletion(1,nullptr));
    BYTE* got{};ok(readback->Map(0,nullptr,(void**)&got));UINT correct=0,pos_ok=0,walker_ok=0;
    for(UINT i=0;i<nr;++i){Hit a{};std::memcpy(&a,got+i*query_stride,sizeof(a));const auto& e=records[i].expected;
        bool match=a.instance==e.instance && a.primitive==e.primitive && std::abs(a.t-e.t)<.001f && std::abs(a.u-e.u)<.0001f && std::abs(a.v-e.v)<.0001f;
        if(!pos_ref.empty())for(UINT k=0;k<9;++k){float v,ref;std::memcpy(&v,got+i*query_stride+sizeof(a)+4*k,4);
            std::memcpy(&ref,pos_ref.data()+i*36+4*k,4);bool same=std::abs(v-ref)<.0001f;pos_ok+=same;match&=same;}
        if(!pos_ref.empty()){
            bool hit=e.instance!=UINT_MAX;UINT instance=(e.instance==41);
            std::array<UINT,10> values,ref{UINT(hit),hit?std::bit_cast<UINT>(e.t):records[i].ray[2],
                hit?e.instance:0u,hit?(instance^UINT(intersection)):0u,UINT(hit),0,3,0,0,
                std::bit_cast<UINT>(instance?200.f:0.f)};
            std::memcpy(values.data(),got+i*query_stride+56,sizeof(values));
            for(UINT k=0;k<values.size();++k){bool same=k==1?
                std::abs(std::bit_cast<float>(values[k])-std::bit_cast<float>(ref[k]))<.001f:values[k]==ref[k];
                walker_ok+=same;match&=same;}
        }
        std::printf("ray %u: instance %u primitive %u t %.4f uv %.5f %.5f %s\n",i,a.instance,a.primitive,a.t,a.u,a.v,match?"OK":"FAIL");correct+=match;}
    D3D12_RANGE empty{};readback->Unmap(0,&empty);
    if(!pos_ref.empty())std::printf("KHR hardware triangle position fetch: %u/%u coordinates matched\n",pos_ok,9*nr);
    if(!pos_ref.empty())std::printf("Lumen inline walker helpers: %u/%u hit, distance, instance, mask, range and budget fields matched\n",walker_ok,10*nr);
    std::printf("XS DXR tier %u: %u vertices, %u triangles, 2 instances, %u/%u rays matched\n",UINT(caps.RaytracingTier),nv,ni/3,correct,nr);
    UINT* oc{};ok(occ_read->Map(0,nullptr,(void**)&oc));UINT oc_ok=0;
    for(UINT i=0;i<nr;++i){bool match=true;
        for(UINT j=0;j<result_stride/4;++j){UINT ref;std::memcpy(&ref,expected.data()+i*result_stride+4*j,4);
            auto value=oc[i*(result_stride/4)+j];match&=intersection && (j==0||j>=3)?
                std::abs(std::bit_cast<float>(ref)-std::bit_cast<float>(value))<.0001f:ref==value;}
        oc_ok+=match;std::printf("retail ray %u: %u words %s\n",i,result_stride/4,match?"OK":"FAIL");}
    occ_read->Unmap(0,&empty);
    std::printf("Retail %s + %s%s%s: %u/%u %s results matched\n",rgname.c_str(),msname.c_str(),intersection?" + ":"",
        chname.c_str(),oc_ok,nr,intersection?"intersection":"occlusion");
    if(guard_read){
        UINT* v{};ok(guard_read->Map(0,nullptr,(void**)&v));UINT matches=0,inside=0;
        for(UINT y=0;y<gh;++y)for(UINT x=0;x<gw;++x){auto actual=v[y*gw+x];inside+=(actual!=0);
            matches+=actual==UINT(x>=2 && x<=4 && y>=1 && y<=3);}
        guard_read->Unmap(0,&empty);std::printf("Retail shadow raygen launch guard: %u/%u lanes matched; %u inside, %u outside\n",matches,gw*gh,inside,gw*gh-inside);
        if(matches!=gw*gh)return 1;
    }
    if(compression_read){UINT* rgba{};ok(compression_read->Map(0,nullptr,(void**)&rgba));UINT n=0;
        for(UINT y=0;y<state_h;++y)for(UINT x=0;x<state_w;++x)n+=rgba[y*state_pitch/4+x]==texel(x,y);
        compression_read->Unmap(0,&empty);std::printf("XS 0x120000c0 SRV transition: %u/%u RGBA texels retained\n",n,state_w*state_h);
        if(n!=state_w*state_h)return 1;}
    return correct==nr && oc_ok==nr && check_shadow(blas.Get(),dir)?0:1;
}catch(const std::exception& e){std::fprintf(stderr,"hwrt-check: %s\n",e.what());return 1;}
