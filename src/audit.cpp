#include "audit.hpp"
#include "archive.hpp"
#include "isa.hpp"
#include "rdna2_spec.hpp"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <fstream>
#include <format>
#include <iostream>
#include <set>
#include <thread>

namespace frosty {
namespace {
using Str=std::string;
using View=std::string_view;
using isa::catalog;
View base_op(View op) {
    for(auto s:{"_sdwa","_dpp8","_dpp"})if(op.ends_with(s))return op.substr(0,op.size()-View(s).size());
    return op;
}
struct Count {
    u64 instances{},records{},lowered{},compiled{};
    unsigned lib=~0u,index=~0u,pc=~0u;
    void add(const Count& b) {
        instances+=b.instances;records+=b.records;lowered+=b.lowered;compiled+=b.compiled;
        if(std::tie(b.lib,b.index,b.pc)<std::tie(lib,index,pc)){lib=b.lib;index=b.index;pc=b.pc;}
    }
    Str example() const {return records?std::format("{}[{}]+0x{:x}",lib?"Frosty":"Global",index,pc):"-";}
};
using Counts=std::map<Str,Count>;
struct Row {
    u64 key{};unsigned frequency{};
    Str scan="-",lower="-",dxil="-",scan_detail,detail,where;
};
Str one_line(Str s) {
    for(char& c:s)if(c=='\t'||c=='\r'||c=='\n')c=' ';
    return s;
}
void measure(const ShaderArchive& ar,unsigned lib,unsigned index,bool dxil,Row& r,Counts& counts,Counts& forms) {
    Counts local,variants;
    auto add=[&](const isa::Instruction& i,Counts& m,const Str& name){
        auto& n=m[name];++n.instances;n.lib=lib;n.index=index;n.pc=std::min(n.pc,i.pc);
    };
    auto entry=ar[index];r.frequency=entry.frequency;
    auto record=entry.unpack();auto s=inspect_shader({record});r.key=s.key();
    if(!s.xbch.size()){r.scan="no-machine-object";return;}
    Buffer obj;Bytes code;
    try {
        obj=s.machine_object();std::vector<Str> gaps;code=isa::machine_code(s,obj);
        auto g=isa::decode_graph(code,&gaps,isa::machine_wave(s,obj),s.stage==isa::vertex);
        for(const auto& [_,i]:g.ins){
            add(i,local,Str(base_op(i.op)));
            add(i,variants,i.op+(i.modifiers.empty()?"":" "+i.modifiers));
        }
        r.scan=gaps.empty()?"ok":"partial";
        if(!gaps.empty())r.scan_detail=gaps.front();
    }catch(const Error& e){r.scan="unsupported";r.scan_detail=e.what();}
    try {
        auto t=isa::recompile(s);r.lower="ok";
        if(dxil)try {isa::compile_dxil(t);r.dxil="ok";}catch(const std::exception& e){r.dxil="failed";r.detail=e.what();}
    }catch(const std::exception& e){
        r.lower="failed";r.detail=e.what();
        auto at=r.detail.find("+0x");unsigned pc;
        if(at!=Str::npos && std::from_chars(r.detail.data()+at+3,r.detail.data()+r.detail.size(),pc,16).ec==std::errc{} && pc<code.size())try{
            auto i=isa::decode(code,pc,isa::machine_wave(s,obj));r.where=i.op;for(const auto& a:i.args)r.where+=" "+a;
            if(!i.modifiers.empty())r.where+=" "+i.modifiers;
        }catch(const Error&){} // retain the original diagnostic
    }
    auto merge=[&](Counts& to,Counts& from){for(auto& [k,v]:from){
        v.records=1;v.lowered=r.lower=="ok";v.compiled=r.dxil=="ok";to[k].add(v);
    }};
    merge(counts,local);merge(forms,variants);
}
auto report(const std::filesystem::path& file) {
    std::ofstream f;f.exceptions(std::ios::failbit|std::ios::badbit);f.open(file);return f;
}
// Translation-only opcode probes. This ABI is for the offline catalog, never
// the guest. The synthetic numeric registers have real definitions, and all
// memory ops must still resolve through the production resource linker.
Str probe_lowering(const isa::OpcodeSpec& c,const isa::Instruction& i,Bytes sample){
    if(c.flags || c.group=="EXPORT" || c.family=="VINTRP" || c.name.starts_with("v_interp"))
        return "needs stage/control-flow fixture";
    Buffer code;
    auto word=[&](u32 n){for(unsigned k=0;k<4;++k)code.push_back(u8(n>>(8*k)));};
    auto smov=[&](unsigned dst,unsigned src){word(0xbe800300u|(dst<<16)|src);};
    auto vmov=[&](unsigned dst,unsigned src){word(0x7e000200u|(dst<<17)|src);};
    auto sload=[&](unsigned dst,unsigned width,unsigned offset){
        word((width==8?0xf40c0000u:0xf4080000u)|(dst<<6)|24u);word(0xfa000000u|offset);};
    auto first=[](Str s){
        if(!s.starts_with("s["))throw Unsupported("specimen has no SGPR resource range");
        unsigned r=0;auto [end,ec]=std::from_chars(s.data()+2,s.data()+s.size(),r);
        if(ec!=std::errc{} || end==s.data()+s.size() || *end!=':')throw Unsupported("invalid SGPR resource range");
        return r;
    };
    isa::Program p{};p.stage=isa::compute;p.wave=64;p.threads={64,1,1};p.cs_rsrc2=8u*2+128;
    p.lds_bytes=c.family=="DS"?1024:4; // give lane and mask ops an explicit guest-wave model
    smov(40,8);
    for(unsigned j=0;j<40;++j)smov(j,40);
    for(unsigned j=1;j<40;++j)vmov(j,256); // data varies by local lane
    smov(124,128); // M0, relative-register / LDS byte base
    smov(49,128); // high half of the probe descriptor table is zero
    p.resources.push_back({48,0,4,1,8,12,4});sload(60,4,0);
    if(c.family=="MUBUF" || c.family=="MTBUF" || c.family=="MIMG" || c.name.starts_with("s_buffer_load")){
        bool img=c.family=="MIMG",cb=c.name.starts_with("s_buffer_load");
        if(i.args.size()>=(cb?2u:3u)){
            bool writable=c.name.contains("_atomic")||c.name.contains("_store");
            bool typed=c.name.contains("_format") || img;
            unsigned width=img?8:4,ty=cb?2:writable?(typed?6:7):(typed?3:4);
            p.resources.push_back({48,32,width,0,ty,cb?13u:img?2u:typed?10u:11u,cb?0u:typed?4u:1u});
            sload(first(i.args[cb?1:2]),width,32);
            if(img && i.args.size()==4){p.resources.push_back({48,64,4,0,1,14,0});sload(first(i.args[3]),4,64);}
        }
    }
    word(0x7d840080u); // v_cmp_eq_u32 vcc,0,v0
    word(0xbf068028u); // s_cmp_eq_u32 s40,0
    code.insert(code.end(),sample.data.begin(),sample.data.end());
    word(0xbf810000u);p.code={code};
    isa::recompile(p);return "lowered in fixture";
}
}
bool audit(Bytes pak,const std::filesystem::path& dir,unsigned jobs,bool dxil) {
    if(!jobs || jobs>64)throw Error("audit worker count must be 1..64");
    std::filesystem::create_directories(dir);
    auto rows=report(dir/"records.tsv");
    rows<<"library\tindex\tfrequency\tkey\tdecode\tlower\tdxil_default\tdecode_detail\tdetail\traw_instruction_at_failure\n";
    Counts counts,forms;std::map<Str,u64> issues;
    unsigned lib=0,scanned=0,whole=0,compiled=0,total=0;
    for(auto name:{"Global","Frosty"}){
        auto ar=open_library(pak,name);std::vector<Row> records(ar.size());
        std::vector<Counts> tally(jobs),variants(jobs);
        std::atomic_uint next=0;
        {
            std::vector<std::jthread> workers;
            for(unsigned j=0;j<jobs;++j)workers.emplace_back([&,j]{
                for(unsigned i;(i=next.fetch_add(1))<ar.size();)try{
                    measure(ar,lib,i,dxil,records[i],tally[j],variants[j]);
                }catch(const std::exception& e){records[i].scan="error";records[i].detail=e.what();}
            });
        }
        for(unsigned i=0;i<ar.size();++i){
            auto& r=records[i];
            rows<<std::format("{}\t{}\t{}\t{:016x}\t{}\t{}\t{}\t{}\t{}\t{}\n",name,i,r.frequency,r.key,
                r.scan,r.lower,r.dxil,one_line(r.scan_detail),one_line(r.detail),one_line(r.where));
            auto why=r.detail.empty()?r.scan_detail:r.detail;
            if(!why.empty()){
                if(auto p=why.find(": ");p!=Str::npos && why.substr(0,p).contains("+0x"))why.erase(0,p+2);
                ++issues[one_line(why.substr(0,220))];
            }
            ++total;scanned+=r.scan=="ok";whole+=r.lower=="ok";compiled+=r.dxil=="ok";
        }
        for(auto& t:tally)for(auto& [k,v]:t)counts[k].add(v);
        for(auto& t:variants)for(auto& [k,v]:t)forms[k].add(v);
        rows.flush();std::cout<<std::format("{}: inventoried {} records\n",name,ar.size())<<std::flush;++lib;
    }
    auto ops=report(dir/"opcodes.tsv"),vf=report(dir/"forms.tsv"),bugs=report(dir/"issues.tsv");
    ops<<"opcode\tfamily\tgroup\tencoding_forms\tinstances\trecords\tin_lowered_records\tin_dxil_records\texample\n";
    unsigned observed=0;
    auto print=[&](View name,View family,View group,unsigned enc,const Count& v){
        ops<<std::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n",name,family,group,enc,v.instances,v.records,v.lowered,v.compiled,v.example());
    };
    for(const auto& c:catalog){auto n=counts.extract(Str(c.name));const Count v=n.empty()?Count{}:n.mapped();
        observed+=v.instances!=0;print(c.name,c.family,c.group,c.encodings,v);}
    for(const auto& [k,v]:counts)print(k,"uncatalogued","-",0,v);
    vf<<"llvm_mnemonic_and_modifiers\tinstances\trecords\tin_lowered_records\tin_dxil_records\texample\n";
    for(auto& [k,v]:forms)vf<<std::format("{}\t{}\t{}\t{}\t{}\t{}\n",k,v.instances,v.records,v.lowered,v.compiled,v.example());
    bugs<<"records\tfirst_diagnostic\n";std::vector<std::pair<Str,u64>> ranked(issues.begin(),issues.end());
    std::stable_sort(ranked.begin(),ranked.end(),[](auto& x,auto& y){return x.second>y.second;});
    for(const auto& [why,n]:ranked)bugs<<n<<'\t'<<why<<'\n';
    auto message=std::format("{} records; {} completely decoded; {} lowered; default DXIL: {}; {}/{} RDNA 2 mnemonics observed, {} other spellings\n",
        total,scanned,whole,dxil?std::to_string(compiled):"not tested",observed,std::size(catalog),counts.size());
    report(dir/"summary.txt")<<message;std::cout<<message;
    return total && scanned==total && (dxil?compiled:whole)==total;
}
void probe_catalog(const std::filesystem::path& file) {
    auto f=report(file);
    f<<"official_mnemonic\tbase_family\tstatus\tsize\tdecoded_instruction_or_diagnostic\tisolated_lowering\n";
    unsigned match=0,fields=0,lowered=0;
    for(const auto& c:catalog){
        Buffer raw;
        for(unsigned i=0;i<c.size;++i)raw.push_back(u8(c.specimen[i/4]>>(8*(i%4))));
        Str status,detail,probe="-";
        try{
            auto i=isa::decode({raw},0,64);
            status=(i.op==c.name && i.size==c.size)?(i.field_decode?"vendor-fields":"decoded"):"different-decode";
            detail=i.op;for(const auto& a:i.args)detail+=" "+a;
            if(!i.modifiers.empty())detail+=" "+i.modifiers;
            match+=status=="decoded";
            fields+=status=="vendor-fields";
            if(i.field_decode)probe="requires format-aware image ABI";
            if(status=="decoded")try{probe=probe_lowering(c,i,{raw});lowered+=probe=="lowered in fixture";}
            catch(const std::exception& e){probe=e.what();}
        }catch(const std::exception& e){status="unresolved-specimen";detail=e.what();}
        f<<std::format("{}\t{}\t{}\t{}\t{}\t{}\n",c.name,c.family,status,c.size,one_line(detail),one_line(probe));
    }
    std::cout<<std::format("Catalog: {} of {} base specimens independently decoded to the specified mnemonic and length\n",match,std::size(catalog));
    std::cout<<std::format("{} additional base specimens identified by vendor fields; {} unresolved\n",fields,std::size(catalog)-match-fields);
    std::cout<<std::format("{} individual instructions lowered in the synthetic compute fixture (not a GPU or whole-shader count)\n",lowered);
}
}
