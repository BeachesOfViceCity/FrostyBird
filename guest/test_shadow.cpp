// Execute the whole recompiled light-space shadow raygen, including its real
// UAV stores and return continuations. The inputs here are diagnostic; this
// program is not part of the title runtime.
#define NOMINMAX
#include <d3d12.h>
#include "ray_packet.hpp"
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using Floats=std::array<float,4>;
void check(HRESULT hr){if(FAILED(hr)){std::fprintf(stderr,"shadow GPU HRESULT %#lx\n",(unsigned long)hr);throw std::runtime_error("shadow GPU call");}}
std::vector<char> file(const std::filesystem::path& p){
    std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("missing shadow library");
    return {std::istreambuf_iterator<char>(f),{}};
}
#include "test_city_shadow.inl"
}
bool check_shadow(ID3D12Resource* blas,const std::filesystem::path& dir){
    std::ifstream manifest(dir/"shadow.libs");if(!manifest)return true;
    ComPtr<ID3D12Device5> dev;check(blas->GetDevice(IID_PPV_ARGS(&dev)));
    ComPtr<ID3D12CommandQueue> q;D3D12_COMMAND_QUEUE_DESC qd{};check(dev->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)));
    ComPtr<ID3D12CommandAllocator> alloc;check(dev->CreateCommandAllocator(qd.Type,IID_PPV_ARGS(&alloc)));
    ComPtr<ID3D12GraphicsCommandList4> l;check(dev->CreateCommandList(0,qd.Type,alloc.Get(),nullptr,IID_PPV_ARGS(&l)));
    std::vector<ComPtr<ID3D12Resource>> keep;
    auto resource=[&](D3D12_HEAP_TYPE heap,const D3D12_RESOURCE_DESC& d,D3D12_RESOURCE_STATES state){
        D3D12_HEAP_PROPERTIES h{};h.Type=heap;h.CreationNodeMask=h.VisibleNodeMask=1;
        ComPtr<ID3D12Resource> r;check(dev->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)));
        keep.push_back(r);return r;
    };
    auto buf=[&](D3D12_HEAP_TYPE heap,UINT64 n,D3D12_RESOURCE_STATES state,bool uav=false){
        return resource(heap,{D3D12_RESOURCE_DIMENSION_BUFFER,0,n,1,1,1,DXGI_FORMAT_UNKNOWN,{1,0},D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
            uav?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE},state);
    };
    auto upload=[&](const void* data,UINT n){
        auto r=buf(D3D12_HEAP_TYPE_UPLOAD,n,D3D12_RESOURCE_STATE_GENERIC_READ);void* p{};check(r->Map(0,nullptr,&p));
        std::memcpy(p,data,n);r->Unmap(0,nullptr);return r;
    };
    auto transition=[&](ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
        D3D12_RESOURCE_BARRIER bar{};bar.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};l->ResourceBarrier(1,&bar);
    };
    // The shipped raygen traces mask 8. Give one real indexed cube that
    // mask; neighboring lit receiver pixels lie well outside its silhouette.
    D3D12_RAYTRACING_INSTANCE_DESC inst{};inst.Transform[0][0]=inst.Transform[1][1]=inst.Transform[2][2]=1;
    inst.InstanceMask=8;inst.AccelerationStructure=blas->GetGPUVirtualAddress();auto idata=upload(&inst,sizeof(inst));
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};in.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    in.NumDescs=1;in.InstanceDescs=idata->GetGPUVirtualAddress();D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pre{};
    dev->GetRaytracingAccelerationStructurePrebuildInfo(&in,&pre);
    auto as=buf(D3D12_HEAP_TYPE_DEFAULT,pre.ResultDataMaxSizeInBytes,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,true);
    auto scratch=buf(D3D12_HEAP_TYPE_DEFAULT,pre.ScratchDataSizeInBytes,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{as->GetGPUVirtualAddress(),in,0,scratch->GetGPUVirtualAddress()};
    l->BuildRaytracingAccelerationStructure(&build,0,nullptr);D3D12_RESOURCE_BARRIER u{};u.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;u.UAV.pResource=as.Get();l->ResourceBarrier(1,&u);
    D3D12_DESCRIPTOR_RANGE ranges[]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,12,0,1,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,1,12},
        {D3D12_DESCRIPTOR_RANGE_TYPE_CBV,3,0,1,14}};
    D3D12_ROOT_PARAMETER par{};par.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;par.DescriptorTable={3,ranges};
    D3D12_ROOT_SIGNATURE_DESC rd{1,&par,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> blob;
    check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,nullptr));ComPtr<ID3D12RootSignature> root;
    check(dev->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,17,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    ComPtr<ID3D12DescriptorHeap> heap;check(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));auto step=dev->GetDescriptorHandleIncrementSize(hd.Type);
    auto cpu=[&](UINT i){return D3D12_CPU_DESCRIPTOR_HANDLE{heap->GetCPUDescriptorHandleForHeapStart().ptr+SIZE_T(i)*step};};
    if(std::filesystem::is_regular_file(dir/"city-shadow.libs"))
        return compare_city_shadow(dir,dev.Get(),q.Get(),l.Get(),root.Get(),heap.Get(),as.Get(),buf,resource,upload,transition,cpu);
    constexpr UINT W=7,H=5;std::array<ComPtr<ID3D12Resource>,2> targets;
    for(UINT i=0;i<14;++i){
        bool output=i>=12,ints=i==5||i==9||i==11;
        auto fmt=ints?DXGI_FORMAT_R32G32B32A32_UINT:DXGI_FORMAT_R32G32B32A32_FLOAT;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if(i==0||i==1){
            sv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;sv.Format=i?DXGI_FORMAT_UNKNOWN:DXGI_FORMAT_R32_FLOAT;sv.Buffer.NumElements=1;sv.Buffer.StructureByteStride=i?32:0;
            std::array<UINT,8> zeros{};auto r=upload(zeros.data(),sizeof(zeros));dev->CreateShaderResourceView(r.Get(),&sv,cpu(i));continue;
        }
        if(i==10){
            sv.ViewDimension=D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;sv.RaytracingAccelerationStructure.Location=as->GetGPUVirtualAddress();
            dev->CreateShaderResourceView(nullptr,&sv,cpu(i));continue;
        }
        D3D12_RESOURCE_DESC d{i==2?D3D12_RESOURCE_DIMENSION_TEXTURE3D:D3D12_RESOURCE_DIMENSION_TEXTURE2D,0,W,H,1,1,fmt,{1,0},
            D3D12_TEXTURE_LAYOUT_UNKNOWN,output?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE};
        auto tex=resource(D3D12_HEAP_TYPE_DEFAULT,d,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        UINT64 bytes{};dev->GetCopyableFootprints(&d,0,1,0,&src.PlacedFootprint,nullptr,nullptr,&bytes);
        std::vector<BYTE> data(bytes);Floats texel{};
        if(i==3 || i==4)texel[0]=1;                // translucent/opaque depth
        if(i==6)texel={.5f,.5f,1,0};
        if(i==11)texel[0]=std::bit_cast<float>(1u); // translucent receiver lighting channels
        if(output)texel.fill(93.f);
        for(UINT y=0;y<H;++y)for(UINT x=0;x<W;++x)std::memcpy(data.data()+y*src.PlacedFootprint.Footprint.RowPitch+x*16,&texel,16);
        src.pResource=upload(data.data(),UINT(bytes)).Get();dst.pResource=tex.Get();
        l->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        transition(tex.Get(),D3D12_RESOURCE_STATE_COPY_DEST,output?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if(output){
            D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};uv.Format=fmt;uv.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
            dev->CreateUnorderedAccessView(tex.Get(),nullptr,&uv,cpu(i));targets[i-12]=tex;
        }else{
            sv.Format=fmt;sv.ViewDimension=i==2?D3D12_SRV_DIMENSION_TEXTURE3D:D3D12_SRV_DIMENSION_TEXTURE2D;
            if(i==2)sv.Texture3D.MipLevels=1;else sv.Texture2D.MipLevels=1;
            dev->CreateShaderResourceView(tex.Get(),&sv,cpu(i));
        }
    }
    std::array<UINT,128> b0{};std::array<float,640> b1{};std::array<float,64> b2{};
    b0[0x28/4]=1;b0[12]=1;b0[13]=1;b0[14]=5;b0[15]=3;
    b0[0x70/4]=std::bit_cast<UINT>(7.f);b0[0x74/4]=std::bit_cast<UINT>(11.f);b0[0x78/4]=std::bit_cast<UINT>(600.f);
    // Screen center maps to x=7, neighboring centers are 150 units apart.
    // The light is at (7,11,600), all receivers at z=-160. At the cube's
    // far face an adjacent ray is still >100 units from its center.
    auto* m=b1.data()+0x2c0/4;m[0]=150;m[12]=7-3.5f*150;m[13]=11;m[14]=-160;m[15]=1;
    b1[0x448/4]=-161;
    auto cb=[&](UINT i,const auto& data){auto r=upload(data.data(),sizeof(data));D3D12_CONSTANT_BUFFER_VIEW_DESC cv{r->GetGPUVirtualAddress(),sizeof(data)};dev->CreateConstantBufferView(&cv,cpu(i));};
    cb(14,b0);cb(15,b1);cb(16,b2);
    std::array<std::vector<char>,3> data;std::array<std::wstring,3> names;D3D12_DXIL_LIBRARY_DESC libs[3]{};
    D3D12_STATE_SUBOBJECT subs[7]{};
    for(UINT n=0;n<3;++n){std::string path,name;if(!(manifest>>path>>name))throw std::runtime_error("incomplete shadow manifest");
        data[n]=file(dir/path);names[n].assign(name.begin(),name.end());libs[n].DXILLibrary={data[n].data(),data[n].size()};
        subs[n]={D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,&libs[n]};}
    D3D12_RAYTRACING_SHADER_CONFIG config{24,8};D3D12_RAYTRACING_PIPELINE_CONFIG depth{1};D3D12_GLOBAL_ROOT_SIGNATURE gr{root.Get()};
    D3D12_HIT_GROUP_DESC hit{};hit.HitGroupExport=L"MeasuredOpaqueGroup";hit.ClosestHitShaderImport=names[2].c_str();
    subs[3]={D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG,&config};subs[4]={D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG,&depth};
    subs[5]={D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,&gr};subs[6]={D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,&hit};
    D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,7,subs};ComPtr<ID3D12StateObject> so;
    check(dev->CreateStateObject(&desc,IID_PPV_ARGS(&so)));ComPtr<ID3D12StateObjectProperties> props;check(so.As(&props));
    std::array<BYTE,256> records{};UINT slot=0;for(auto name:{names[0].c_str(),names[1].c_str(),hit.HitGroupExport,hit.HitGroupExport}){
        auto id=props->GetShaderIdentifier(name);if(!id)throw std::runtime_error("missing shadow shader identifier");std::memcpy(records.data()+64*slot++,id,32);
    }
    auto sb=upload(records.data(),sizeof(records));auto base=sb->GetGPUVirtualAddress();
    D3D12_DISPATCH_RAYS_DESC rays{{base,32},{base+64,32,32},{base+128,128,64},{},W,H,1};
    auto hp=heap.Get();l->SetDescriptorHeaps(1,&hp);l->SetComputeRootSignature(root.Get());l->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
    l->SetPipelineState1(so.Get());l->DispatchRays(&rays);
    std::array<ComPtr<ID3D12Resource>,2> readbacks;UINT pitch{};
    for(UINT i=0;i<2;++i){
        auto d=targets[i]->GetDesc();UINT64 bytes{};D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=targets[i].Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        transition(src.pResource,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        dev->GetCopyableFootprints(&d,0,1,0,&dst.PlacedFootprint,nullptr,nullptr,&bytes);pitch=dst.PlacedFootprint.Footprint.RowPitch;
        readbacks[i]=buf(D3D12_HEAP_TYPE_READBACK,bytes,D3D12_RESOURCE_STATE_COPY_DEST);dst.pResource=readbacks[i].Get();l->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    }
    // Calibration: a deliberately incompatible SBT layout must report the
    // reached TraceRay operands through the raygen's local root descriptor.
    // This second dispatch is isolated and cannot contribute to the images
    // already copied above.
    ComPtr<ID3D12StateObject> mismatch;ComPtr<ID3D12RootSignature> status_root;ComPtr<ID3D12Resource> error_read;
    if(std::filesystem::is_regular_file(dir/"shadow-mismatch.dxil")){
        auto code=file(dir/"shadow-mismatch.dxil");libs[0].DXILLibrary={code.data(),code.size()};
        D3D12_ROOT_PARAMETER rp{};rp.ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;rp.Descriptor={0,21301};
        D3D12_ROOT_SIGNATURE_DESC local{1,&rp,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE};blob.Reset();
        check(D3D12SerializeRootSignature(&local,D3D_ROOT_SIGNATURE_VERSION_1,&blob,nullptr));check(dev->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&status_root)));
        std::array<D3D12_STATE_SUBOBJECT,9> sub{};std::copy_n(subs,7,sub.begin());
        D3D12_LOCAL_ROOT_SIGNATURE lr{status_root.Get()};sub[7]={D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE,&lr};
        auto rg_name=names[0].c_str();D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION assoc{&sub[7],1,&rg_name};
        sub[8]={D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,&assoc};D3D12_STATE_OBJECT_DESC pipe{desc.Type,9,sub.data()};
        check(dev->CreateStateObject(&pipe,IID_PPV_ARGS(&mismatch)));ComPtr<ID3D12StateObjectProperties> ids;check(mismatch.As(&ids));
        UINT k=0;for(auto name:{names[0].c_str(),names[1].c_str(),hit.HitGroupExport,hit.HitGroupExport}){
            auto id=ids->GetShaderIdentifier(name);if(!id)throw std::runtime_error("missing calibrated identifier");std::memcpy(records.data()+64*k++,id,32);}
        std::array<UINT,4> zeros{};auto zero=upload(zeros.data(),16);
        auto status=buf(D3D12_HEAP_TYPE_DEFAULT,16,D3D12_RESOURCE_STATE_COPY_DEST,true);auto va=status->GetGPUVirtualAddress();
        std::memcpy(records.data()+32,&va,8);auto sbt=upload(records.data(),sizeof(records));va=sbt->GetGPUVirtualAddress();
        l->CopyBufferRegion(status.Get(),0,zero.Get(),0,16);transition(status.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        for(auto& r:targets)transition(r.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        rays.RayGenerationShaderRecord={va,64};rays.MissShaderTable.StartAddress=va+64;rays.HitGroupTable.StartAddress=va+128;
        l->SetPipelineState1(mismatch.Get());l->DispatchRays(&rays);transition(status.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        error_read=buf(D3D12_HEAP_TYPE_READBACK,16,D3D12_RESOURCE_STATE_COPY_DEST);l->CopyBufferRegion(error_read.Get(),0,status.Get(),0,16);
    }
    check(l->Close());ID3D12CommandList* list=l.Get();q->ExecuteCommandLists(1,&list);ComPtr<ID3D12Fence> f;
    check(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f)));check(q->Signal(f.Get(),1));check(f->SetEventOnCompletion(1,nullptr));
    UINT good=0;
    for(UINT i=0;i<2;++i){BYTE* ptr{};check(readbacks[i]->Map(0,nullptr,(void**)&ptr));
        for(UINT y=0;y<H;++y)for(UINT x=0;x<W;++x){
            Floats val,expected{};std::memcpy(&val,ptr+y*pitch+x*16,16);
            bool inside=x>=1 && x<=5 && y>=1 && y<=3;
            if(i==1)expected.fill(inside && x!=3?1.f:0.f);
            else expected[1]=-2;
            bool ok=true;for(UINT j=0;j<4;++j)ok&=std::abs(val[j]-expected[j])<.0001f;
            if(!ok)std::printf("shadow u%u (%u,%u): %.5g %.5g %.5g %.5g expected %.5g %.5g %.5g %.5g\n",i,x,y,val[0],val[1],val[2],val[3],expected[0],expected[1],expected[2],expected[3]);
            good+=ok;
        }
        D3D12_RANGE empty{};readbacks[i]->Unmap(0,&empty);
    }
    std::printf("Whole retail shadow raygen: %u/%u RGBA texels matched (20 outside viewport; 3 occluded, 12 clear inside)\n",good,2*W*H);
    if(error_read){
        UINT* p{};check(error_read->Map(0,nullptr,(void**)&p));std::array<UINT,4> record;std::memcpy(record.data(),p,16);D3D12_RANGE none{};error_read->Unmap(0,&none);
        std::printf("GPU SBT contract mismatch calibration: status=%u packed=%#x flags=%#x payload[0]=%#x\n",record[0],record[1],record[2],record[3]);
        if(record[0]!=3 || record[1]!=0x2108 || (record[2]&2))return false;
    }
    return good==2*W*H;
}
