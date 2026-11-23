#define NOMINMAX
#include "ray_dispatch.hpp"
#include "play_timing.hpp"
#include "shader_client.hpp"
#include "../src/rt_collection.hpp"
#include "../src/view_fields.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
using Microsoft::WRL::ComPtr;
namespace {
using S=std::string;using W=std::wstring;
using Id=std::array<BYTE,32>;
S hex(UINT64 n){char s[20];std::snprintf(s,sizeof(s),"%016llx",n);return s;}
void require(bool v,const S& why){if(!v)throw frosty::Error(why);}
void log(const S& text){auto s="frosty/ray-link: "+text+"\n";OutputDebugStringA(s.c_str());std::fputs(s.c_str(),stderr);}
std::vector<BYTE> read(const std::filesystem::path& p){
    std::ifstream f(p,std::ios::binary|std::ios::ate);require(bool(f),"missing "+p.filename().string());auto n=f.tellg();
    require(n>0 && n<64*1024*1024,"invalid ray data size");std::vector<BYTE> data(static_cast<size_t>(n));
    f.seekg(0);f.read(reinterpret_cast<char*>(data.data()),n);require(bool(f),"short ray data");return data;
}
struct Shader {
    UINT64 key;int stage;
    auto operator<=>(const Shader&)const=default;
    W alias()const{auto k=hex(key);return (stage==0?L"RG_":stage==1?L"MS_":L"CH_")+W(k.begin(),k.end());}
    W group()const{auto k=hex(key);return L"HG_"+W(k.begin(),k.end());}
};
struct Record{size_t offset;Shader shader;};
}
void RayExecution::record(ID3D12GraphicsCommandList4* l){
    packet->begin(l,zero_offset);
    l->SetPipelineState1(pipeline.Get());l->DispatchRays(&desc);
    packet->finish(l);
}
bool RayExecution::retired(){
    std::array<UINT,4> result;if(FAILED(packet->result(result)))return false;
    if(!result[0])return true;
    log("shader "+hex(source)+" requires another specialization, code="+std::to_string(result[0])+" operands="+hex(result[1])+","+hex(result[2])+","+hex(result[3]));return false;
}
void RayDispatch::collection(const void* data,UINT n,ID3D12StateObject* p){
    auto entries=rt_identifiers(p);std::lock_guard guard(lock);
    for(const auto& e:entries){
        if(!e.key)continue;
        auto [a,added]=libraries.try_emplace(e.key);
        if(added)a->second.source.assign(static_cast<const BYTE*>(data),static_cast<const BYTE*>(data)+n);
        else require(a->second.source.size()==n && !std::memcmp(data,a->second.source.data(),n),"different bytes share RT source key");
        ids[e.id]={e.key,e.name};
    }
}
HRESULT RayDispatch::link(PipelinePrep& prep,ID3D12Device5* dev,ID3D12RootSignature* root,ID3D12StateObject* state,
        const D3D12_DISPATCH_RAYS_DESC& d,RayMemory& memory,const std::map<UINT,RayLiveView>& views,
        const std::function<std::shared_ptr<RaySceneLayout>(UINT64)>& scene_for,RayExecution& out)try{
    play_timing::Scope time(play_timing::ray_links);
    wchar_t path[32768]{};auto len=GetEnvironmentVariableW(L"FROSTY_RT_DEMAND_PATH",path,DWORD(std::size(path)));
    if(!len || len>=std::size(path))return S_FALSE;
    std::lock_guard guard(lock);std::filesystem::path dir(path);
    auto identify=[&](const BYTE* bytes){Id id{};std::memcpy(id.data(),bytes,id.size());auto it=ids.find(id);
        if(it==ids.end()){UINT64 v[4];std::memcpy(v,bytes,32);throw frosty::Error("unrecognized SBT identifier "+hex(v[0])+":"+hex(v[1])+":"+hex(v[2]));}
        return it->second;};
    for(auto& e:rt_identifiers(state))if(e.key && libraries.contains(e.key))ids.try_emplace(e.id,Identifier{e.key,e.name});
    std::vector<BYTE> table;require(memory.read(d.RayGenerationShaderRecord.StartAddress,d.RayGenerationShaderRecord.SizeInBytes,table) && table.size()>=32,"missing raygen table upload");
    auto rg=identify(table.data());auto prefix=(dir/hex(rg.key)).wstring();
    auto file=[&](const W& ext){return std::filesystem::path(prefix+ext);};
    D3D12_RAYTRACING_SHADER_CONFIG config{};D3D12_RAYTRACING_PIPELINE_CONFIG1 depth{};
    require(rt_settings(state,config,depth),"missing bound RT pipeline configuration");
    auto matches=[&](const Raygen& shader){
        if(shader.words*4!=config.MaxPayloadSizeInBytes)return false;
        for(auto& [reg,shape]:shader.layout){auto v=views.find(reg);
            if(v==views.end() || v->second.dimension!=shape.first || v->second.type!=shape.second)return false;}
        auto as=views.find(shader.as_reg);return as!=views.end() && as->second.dimension==D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    };
    auto& compiled_rgs=raygens[rg.key];auto use=std::ranges::find_if(compiled_rgs,matches);
    if(use==compiled_rgs.end()){
        Raygen shader;bool offline=false;std::ifstream plan(file(L".checked.plan"));
        if(plan){
            UINT nv;require(bool(plan>>shader.words>>shader.selector>>shader.field>>shader.value>>shader.as_reg>>nv) && nv<=256,"bad raygen plan");
            for(UINT i=0;i<nv;++i){UINT reg,dim;S ty;require(bool(plan>>reg>>dim>>ty) &&
                shader.layout.emplace(reg,std::pair{dim,ty}).second,"invalid raygen binding list");}
            offline=matches(shader);
        }else{
            std::ifstream shape(dir/L"dispatch.contract");
            if(!(shape>>shader.words>>shader.selector>>shader.field>>shader.value) || shader.words*4!=config.MaxPayloadSizeInBytes){
                log("no SBT specialization for raygen "+hex(rg.key));return S_FALSE;}
        }
        require(shader.words>0 && shader.words<=32 && shader.field<shader.words && shader.selector<=0xffffff &&
                shader.words*4==config.MaxPayloadSizeInBytes,"incompatible ray payload contract");
        auto& original=libraries.at(rg.key).source;
        if(offline){
            require(original==read(file(L".xsrt")),"different source raygen "+hex(rg.key));
            auto abi=read(file(L".rt.abi"));UINT payload{};if(abi.size()==8)std::memcpy(&payload,abi.data(),4);
            require(payload==shader.words*4,"different prewarmed raygen ABI");shader.code=read(file(L".checked.dxil"));
        }else{
            S layout;for(auto& [reg,v]:views)if(v.dimension){
                if(!layout.empty())layout+=",";
                layout+=std::to_string(reg)+":"+std::to_string(v.dimension)+":"+v.type;}
            shader.layout.clear();std::vector<UINT> requested;ComPtr<ID3DBlob> code;
            auto hr=request_raygen(original,shader.words,shader.selector,shader.field,shader.value,layout,requested,code.GetAddressOf());
            require(hr==S_OK && code,"cannot lift bound raygen "+hex(rg.key));
            bool has_as=false;for(auto r:requested){auto reg=r&0x17fu;auto it=views.find(reg);require(it!=views.end(),"different compiler binding set");
                if(r&frosty::view::ray_as){require(!has_as,"multiple live TLASes");has_as=true;shader.as_reg=reg;}
                else shader.layout.emplace(reg,std::pair{it->second.dimension,it->second.type});}
            require(has_as && matches(shader),"missing live raygen TLAS/type");
            auto* bytes=static_cast<const BYTE*>(code->GetBufferPointer());shader.code.assign(bytes,bytes+code->GetBufferSize());
            log("compiled bound raygen "+hex(rg.key)+" with "+std::to_string(requested.size())+" original global bindings");
        }
        compiled_rgs.push_back(std::move(shader));use=std::prev(compiled_rgs.end());
    }
    const UINT words=use->words,packed=use->selector,field=use->field,constant=use->value;
    UINT multiple=(packed>>4)&15u,add=packed&15u,miss=packed>>8;
    auto closure_key=[&](const std::set<Shader>& shaders){
        S k=hex(reinterpret_cast<UINT_PTR>(root))+":"+hex(rg.key)+":"+std::to_string(use-compiled_rgs.begin())+":"+std::to_string(config.MaxAttributeSizeInBytes)+":"+std::to_string(depth.MaxTraceRecursionDepth)+":"+std::to_string(UINT(depth.Flags));
        for(auto v:shaders)k+="/"+std::to_string(v.stage)+hex(v.key);
        return k;
    };
    auto as=views.find(use->as_reg);
    auto scene=scene_for(as->second.va);require(scene && scene->count,"no submitted instance layout for "+hex(as->second.va));
    const auto& hit=d.HitGroupTable;
    auto step=hit.StrideInBytes?hit.StrideInBytes:hit.SizeInBytes;
    require(step>=32 && hit.SizeInBytes && hit.SizeInBytes<=(64u<<20) && !(hit.SizeInBytes%step),"invalid hit table");
    std::vector<BYTE> hit_bytes;require(memory.read(hit.StartAddress,hit.SizeInBytes,hit_bytes),"missing immutable hit table");
    const auto& ms=d.MissShaderTable;auto ms_step=ms.StrideInBytes?ms.StrideInBytes:ms.SizeInBytes;
    require(ms_step>=32 && ms.SizeInBytes && ms.SizeInBytes<=(64u<<20) && !(ms.SizeInBytes%ms_step),"invalid miss table");
    std::vector<BYTE> miss_bytes;require(memory.read(ms.StartAddress,ms.SizeInBytes,miss_bytes),"missing immutable miss table");
    bool complete=hit.SizeInBytes/step<=2048;
    std::set<UINT64> all_hits;
    for(size_t pos=0;complete && pos<hit_bytes.size();pos+=step){
        Id id{};std::memcpy(id.data(),hit_bytes.data()+pos,32);if(id==Id{})continue;
        auto found=ids.find(id);if(found==ids.end() || !libraries.contains(found->second.key)){complete=false;break;}
        if(!all_hits.insert(found->second.key).second)continue;
        if(all_hits.size()>16){complete=false;break;}
        auto& lib=libraries.at(found->second.key);
        if(lib.triangle_chs_only<0){
            auto module=frosty::rt_collection({lib.source});auto& exp=module.modules.back().exports;
            auto group=std::ranges::find_if(exp,[](auto& x){return x.flags&1;});
            lib.triangle_chs_only=group!=exp.end() && std::ranges::count_if(exp,[](auto& x){return x.flags&1;})==1 &&
                !group->symbols[1].empty() && group->symbols[2].empty() && group->symbols[3].empty();
        }
        complete=lib.triangle_chs_only!=0;
    }
    if(complete){
        // Do not introduce a compilation dependency on an unused row. Only
        // reuse the complete closure if that *exact* native pipeline has
        // already been built successfully through the checked path.
        std::set<Shader> full{{rg.key,0}};for(auto key:all_hits)full.insert({key,2});
        const auto index=ms.StrideInBytes?miss:0u;
        if(index>=ms.SizeInBytes/ms_step)complete=false;
        else{Id id{};std::memcpy(id.data(),miss_bytes.data()+index*ms_step,32);
            if(id!=Id{}){auto it=ids.find(id);if(it==ids.end())complete=false;else full.insert({it->second.key,1});}}
        complete=complete && cache.contains(closure_key(full));
    }
    // With every non-null row present and no original any-hit/intersection
    // stage to omit, the native SBT is valid regardless of the set of active
    // instance contributions. Instance/ray masks and all shader-local data
    // still come from the original table. Keep the previous opacity check
    // on the *same* TLAS, without a GPU-to-recording-thread round trip.
    if(complete)require(scene->request_check(),"active TLAS contains non-opaque instance");
    else {scene->ready->wait();require(scene->all_active_opaque,"active TLAS contains non-opaque instance");}
    std::set<UINT> bases;std::array<UINT,16> first;first.fill(UINT_MAX);
    if(!complete)for(auto contribution:scene->contributions){
        auto base=contribution+add;
        if(multiple)first[base%multiple]=std::min(first[base%multiple],base);else bases.insert(base);}
    require(!d.CallableShaderTable.SizeInBytes,"unsupported callable dispatch");
    // A geometry index only adds nonnegative multiples of the original
    // multiplier. One lower bound per residue covers every possible hit
    // without walking the city's entire instance table for every SBT row.
    auto active=[&](UINT i){return complete || (multiple?i>=first[i%multiple]:bases.contains(i));};
    std::vector<Record> records{{0,{rg.key,0}}};std::set<Shader> shaders{{rg.key,0}};out.desc=d;out.source=rg.key;
    auto append=[&](D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE& t,int stage){
        require(t.SizeInBytes && t.SizeInBytes<=(64u<<20),"missing SBT range");
        auto stride=t.StrideInBytes?t.StrideInBytes:t.SizeInBytes;require(stride>=32 && !(t.SizeInBytes%stride),"invalid SBT stride");
        std::vector<BYTE> bytes;
        bytes=stage==2?std::move(hit_bytes):std::move(miss_bytes);
        size_t base=(table.size()+63)&~size_t(63);table.resize(base);table.insert(table.end(),bytes.begin(),bytes.end());
        for(size_t i=0;i<bytes.size()/stride;++i){auto pos=base+i*stride;
            bool used=stage==1?(t.StrideInBytes?i==miss:i==0):(t.StrideInBytes?active(UINT(i)):true);
            Id id{};std::memcpy(id.data(),table.data()+pos,32);if(id==Id{})continue;
            if(used){auto e=identify(id.data());Shader shader{e.key,stage};records.push_back({pos,shader});shaders.insert(shader);}
            else std::memset(table.data()+pos,0,32);
        }
        t.StartAddress=base;
    };
    append(out.desc.MissShaderTable,1);append(out.desc.HitGroupTable,2);
    auto cache_key=closure_key(shaders);
    auto cached=cache.find(cache_key);
    if(cached!=cache.end()){
        out.pipeline=cached->second.pipeline;
        if(complete && !cached->second.reported_complete){cached->second.reported_complete=true;log("reused complete CHS-only table for "+hex(rg.key)+"; opacity checked on TLAS retirement");}
    }
    else{
        std::vector<RayPipelineLibrary> libs;libs.reserve(shaders.size());UINT rg_index=UINT_MAX;
        for(auto v:shaders){
            auto stem=(dir/hex(v.key)).wstring();auto subfile=[&](const W& ext){return std::filesystem::path(stem+ext);};
            const auto& original=libraries.at(v.key).source;
            auto c=frosty::rt_collection({original});auto& m=c.modules.back();
            auto& lib=libs.emplace_back();lib.alias=v.alias();S name;
            if(v.stage==2){
                require(std::ranges::count_if(m.exports,[](auto& e){return bool(e.flags&1);})==1,"ambiguous hit-group collection");
                auto h=std::ranges::find_if(m.exports,[](auto& e){return bool(e.flags&1);});
                require(h->symbols[3].empty() && !h->symbols[1].empty(),"not a triangle closest-hit group");name=h->symbols[1];
                std::ifstream f(subfile(L".hit.plan"));UINT w,k,n;S imported;
                if(f && (f>>w>>k>>n>>imported) && w==words && k==field && n==constant && imported==name){
                    require(original==read(subfile(L".xsrt")),"different source collection for "+hex(v.key));lib.code=read(subfile(L".hit.dxil"));
                }else{
                    ComPtr<ID3DBlob> compiled;auto hr=request_ray_leaf(original,8,words,field,constant,name,compiled.GetAddressOf());
                    require(hr==S_OK && compiled,"cannot lift requested hit export "+hex(v.key)+"/"+name);
                    auto* bytes=static_cast<const BYTE*>(compiled->GetBufferPointer());lib.code.assign(bytes,bytes+compiled->GetBufferSize());
                    log("compiled bound hit export "+hex(v.key)+"/"+name);
                }
                lib.group=v.group();
            }else{
                require(std::ranges::count_if(m.exports,[&](auto& e){return !(e.flags&3) && e.type==(v.stage==0?0u:4u);})==1,"ambiguous raygen/miss collection");
                auto it=std::ranges::find_if(m.exports,[&](auto& e){return !(e.flags&3) && e.type==(v.stage==0?0u:4u);});
                require(it!=m.exports.end(),"missing original raygen/miss export");name=it->symbols[0];
                if(v.stage==0)lib.code=use->code;
                else{
                    UINT b{};if(std::filesystem::is_regular_file(subfile(L".rt.abi"))){
                        auto abi=read(subfile(L".rt.abi"));if(abi.size()==8)std::memcpy(&b,abi.data(),4);}
                    if(b==words*4 && std::filesystem::is_regular_file(subfile(L".rt.dxil"))){
                        require(original==read(subfile(L".xsrt")),"different source miss "+hex(v.key));lib.code=read(subfile(L".rt.dxil"));
                    }else{
                        ComPtr<ID3DBlob> ms;auto hr=request_ray_leaf(original,7,words,field,constant,name,ms.GetAddressOf());
                        require(hr==S_OK && ms,"cannot lift bound miss "+hex(v.key)+"/"+name);
                        auto* bytes=static_cast<const BYTE*>(ms->GetBufferPointer());lib.code.assign(bytes,bytes+ms->GetBufferSize());
                    }
                }
            }
            lib.entry.assign(name.begin(),name.end());if(v.stage==0)rg_index=UINT(libs.size()-1);
        }
        D3D12_ROOT_PARAMETER p{};p.ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;p.Descriptor={0,21301};
        D3D12_ROOT_SIGNATURE_DESC rd{1,&p,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE};ComPtr<ID3DBlob> b;
        auto hr=D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&b,nullptr);if(FAILED(hr))return hr;
        hr=prep.rays(dev,root,config,depth,libs,rg_index,{static_cast<const BYTE*>(b->GetBufferPointer()),b->GetBufferSize()},out.pipeline.GetAddressOf());
        if(FAILED(hr)){log("native closure creation HRESULT "+hex(UINT(hr)));return hr;}
        cache.emplace(cache_key,Linked{root,out.pipeline});log("linked raygen "+hex(rg.key)+" with "+std::to_string(shaders.size())+" source exports, "+std::to_string(records.size())+" linked records, "+std::to_string(scene->count)+" TLAS instances"+(complete?" (complete CHS-only table)":""));
    }
    ComPtr<ID3D12StateObjectProperties> props;auto hr=out.pipeline.As(&props);if(FAILED(hr))return hr;
    std::map<Shader,Id> native_ids;
    for(auto s:shaders){auto name=s.stage==2?s.group():s.alias();auto id=props->GetShaderIdentifier(name.c_str());
        require(id!=nullptr,"native shader table lookup failed");std::memcpy(native_ids[s].data(),id,32);}
    for(auto r:records)std::memcpy(table.data()+r.offset,native_ids.at(r.shader).data(),32);
    require(d.RayGenerationShaderRecord.SizeInBytes>=40,"raygen record too small for checked linkage");
    out.zero_offset=table.size();bool reused;
    if(FAILED(hr=packets.take(dev,table.size()+16,out.packet,reused)) || FAILED(hr=out.packet->write(table)))return hr;
    (reused?play_timing::ray_packet_hits:play_timing::ray_packet_allocations).fetch_add(1,std::memory_order_relaxed);
    auto va=out.packet->table->GetGPUVirtualAddress();out.desc.RayGenerationShaderRecord.StartAddress=va;
    out.desc.MissShaderTable.StartAddress+=va;out.desc.HitGroupTable.StartAddress+=va;return S_OK;
}catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
catch(const std::exception& e){log(e.what());return E_NOTIMPL;}
