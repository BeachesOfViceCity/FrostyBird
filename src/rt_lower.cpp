#include "rt_collection.hpp"
#include "isa.hpp"
#include "rt_machine.hpp"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cctype>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <sstream>

namespace frosty {
namespace {
using Str=std::string;
using View=std::string_view;
[[noreturn]] void fail(View s){throw Unsupported(Str(s));}
u32 integer(View v){
    if(v=="null")return 0;
    if(v.starts_with("lit(") && v.ends_with(')'))return integer(v.substr(4,v.size()-5));
    bool neg=v.starts_with('-');if(neg)v.remove_prefix(1);
    int base=v.starts_with("0x")?16:10;if(base==16)v.remove_prefix(2);
    u32 n{};auto [p,ec]=std::from_chars(v.data(),v.data()+v.size(),n,base);
    if(ec!=std::errc{} || p!=v.data()+v.size())fail("expected integer");
    return neg?0u-n:n;
}
enum class K {constant,expr,table,srd,root,stackbase,stackaddr,ret,pc,cont,dispatch,shift,shifted,as_low,as_high,as_node,
    hit_addr,instance_word};
struct Word {K k;u32 n{};Str s{};};
struct Range {char bank; u32 first,count;Str at(u32 i)const{return std::format("{}{}",bank,first+i);}};
Range range(View v){
    if(v.size()<2 || (v.front()!='s' && v.front()!='v'))fail("expected register range");
    char bank=v.front();v.remove_prefix(1);
    if(v.front()!='[')return {bank,integer(v),1};
    auto sep=v.find(':');if(sep==v.npos || !v.ends_with(']'))fail("malformed register range");
    u32 lo=integer(v.substr(1,sep-1)),hi=integer(v.substr(sep+1,v.size()-sep-2));
    if(hi<lo || hi>=256)fail("invalid register range");
    return {bank,lo,hi-lo+1};
}
struct Binding {u32 type,reg,space;Str name()const{return std::format("{}{}",type==0?'t':type==1?'u':type==2?'b':'s',reg);}};
class Link {
    const RtModule& m;unsigned stage,words;
    std::map<Str,Word> regs;
    // Model offsets relative to the entry stack pointer, not a driver VA.
    std::map<u32,Word> stack;std::map<Str,unsigned> strides;
    std::map<Str,Str> declarations;
    std::map<std::pair<u32,u32>,u32> cb_words;
    std::ostringstream body;unsigned seq=0;
    bool active=true;
    // Logical bases in the *translator*, never host or guest GPU addresses.
    // Only the selected leaf/instance fields can leave this address domain.
    static constexpr u32 tlas=0x100000,blas=0x200000,node=64;
    bool fixed(Word w)const{return w.k==K::constant || w.k==K::hit_addr;}
    Word folded(Word a,Word b,u32 n)const{
        if(!fixed(a)||!fixed(b))fail("dynamic hit-node address arithmetic");
        return {a.k==K::hit_addr||b.k==K::hit_addr?K::hit_addr:K::constant,n};
    }
    Word lit(u32 n)const{return {K::constant,n};}
    Word read(const Str& a)const{
        if(auto it=regs.find(a);it!=regs.end())return it->second;
        if(a.contains('.') && !a.starts_with("0x")){
            float f{};auto [p,ec]=std::from_chars(a.data(),a.data()+a.size(),f);
            if(ec==std::errc{} && p==a.data()+a.size())return lit(std::bit_cast<u32>(f));
        }
        return lit(integer(a));
    }
    Str u(Word w)const{
        if(w.k==K::expr)return w.s;
        if(w.k==K::constant)return std::format("{}u",w.n);
        fail("non-numeric ray ABI word");
    }
    u32 number(Word w)const{if(w.k!=K::constant)fail("nonconstant ray ABI field");return w.n;}
    Word expr(const Str& e){auto t=std::format("r{}",++seq);body<<"    uint "<<t<<" = "<<e<<";\n";return {K::expr,0,t};}
    Binding binding(u32 table,u32 offset)const{
        auto r=m.root;
        if(!r.size() || r.at<u32>(0)!=1 || table>=r.at<u32>(4) || offset%32)fail("unknown serialized root table");
        auto p=r.tail(r.at<u32>(8)+12ull*table);
        if(p.at<u32>(0)!=0)fail("not a descriptor table");
        auto tab=r.tail(p.at<u32>(8));
        if(tab.at<u32>(0)!=1)fail("multi-range ray table");
        auto q=r.tail(tab.at<u32>(4));auto type=q.at<u32>(0);
        if(type>2 || q.at<u32>(16)!=~0u || offset/32>=q.at<u32>(4))fail("unmapped ray SRD");
        return {type,q.at<u32>(8)+offset/32,q.at<u32>(12)};
    }
    Str resource(Binding b,bool as=false){
        auto name=b.name();Str decl;
        if(as){if(b.type)fail("AS bound as UAV");decl="RaytracingAccelerationStructure ";}
        else{
            auto it=strides.find(name);if(it==strides.end())fail("missing structured view stride for "+name);
            auto ty="E_"+name;decl=std::format("struct {} {{uint w[{}];}};\n{}StructuredBuffer<{}> ",ty,it->second/4,b.type?"RW":"",ty);
        }
        decl+=std::format("res_{} : register({},space{});",name,name,b.space);
        auto [it,added]=declarations.emplace(name,decl);if(!added && it->second!=decl)fail("conflicting ray binding");
        return "res_"+name;
    }
    Binding descriptor(Range d)const{
        auto v=read(d.at(0));if(d.bank!='s' || v.k!=K::srd || v.n%4 || d.count!=4)fail("missing ray buffer SRD");
        for(u32 i=1;i<d.count;++i){auto w=read(d.at(i));
            if(w.k!=K::srd || w.n!=v.n+i || w.s!=v.s)fail("partially overwritten ray descriptor");}
        return binding(v.n/4,integer(v.s));
    }
    Str constants()const{
        std::ostringstream o;
        for(auto [bind,max_words]:cb_words){auto [space,reg]=bind;
            o<<std::format("cbuffer CB{}_{} : register(b{},space{}) {{uint4 cb{}_{}[{}];}};\n",
                space,reg,reg,space,space,reg,(max_words+3)/4);}
        return o.str();
    }
    bool execute(const isa::Instruction& i){
        const auto& op=i.op;const auto& a=i.args;
        if(op=="s_waitcnt" || op=="s_waitcnt_vscnt" || op=="s_nop" || op=="v_nop")return false;
        if(!i.modifiers.empty() && !op.starts_with("buffer_"))fail("unmapped ray instruction modifiers");
        if(op=="s_setpc_b64" && a==std::vector<Str>{"s[30:31]"})return true;
        if(a.empty())fail("unknown ray instruction "+op);
        auto dst=a[0];auto x=[&](unsigned j){return read(a.at(j));};
        if(op=="s_mov_b32" && dst=="exec_lo"){active=number(x(1))!=0;return false;}
        if(op=="v_mov_b32" || op=="s_mov_b32"){regs[dst]=x(1);return false;}
        if(!active && op.starts_with("v_"))fail("unstructured inactive hit block");
        if(op=="s_load_dword" || op=="s_load_dwordx4"){
            auto src=range(a[1]);auto v=read(src.at(0));u32 offset=integer(a[2]);
            if(src.bank!='s' || src.count!=2)fail("bad SMEM source");
            if(v.k==K::root && src.first==6 && op=="s_load_dword" && !(offset&3))regs[dst]={K::table,offset/4};
            else if(v.k==K::table && number(read(src.at(1)))==0 && op=="s_load_dwordx4"){
                auto b=binding(v.n,offset);auto d=range(dst);if(d.count!=4)fail("bad SRD width");
                for(u32 n=0;n<4;++n)regs[d.at(n)]={K::srd,4*v.n+n,std::to_string(offset)};
                (void)b;
            }else fail("unknown ray SMEM address");
        }else if(op=="s_bfe_u32" && a[1]=="s13" && !regs.contains("s13") && (a[2]=="0x50010" || a[2]=="0x50018"))
            regs[dst]={K::shift,a[2]=="0x50018"?1u:0u};
        else if(op=="v_lshlrev_b32" && x(1).k==K::shift && !x(1).n && x(2).k==K::dispatch)regs[dst]={K::shifted};
        else if(op=="v_lshrrev_b32" && x(1).k==K::shift && !x(1).n && x(2).k==K::shifted)regs[dst]=expr("DispatchRaysIndex().x");
        else if(op=="v_lshrrev_b32" && x(1).k==K::shift && x(1).n==1 && x(2).k==K::dispatch)regs[dst]=expr("DispatchRaysIndex().y");
        else if(op.starts_with("s_buffer_load_dword")){
            auto d=range(dst),srd=range(a[1]);auto b=descriptor(srd);auto bytes=integer(a[2]);
            auto suffix=View(op).substr(View("s_buffer_load_dword").size());
            auto count=suffix.empty()?1u:suffix.starts_with('x')?integer(suffix.substr(1)):0u;
            if(b.type!=2 || d.bank!='s' || !std::has_single_bit(count) || count>16 || count!=d.count ||
               bytes%4 || bytes>65536-count*4)fail("unmapped ray CBV access");
            u32 off=bytes/4;auto& max=cb_words[{b.space,b.reg}];max=std::max(max,off+count);
            for(u32 k=0;k<count;++k)regs[d.at(k)]=expr(std::format("cb{}_{}[{}][{}]",b.space,b.reg,(off+k)/4,(off+k)%4));
        }else if(op=="v_cvt_f32_i32" || op=="v_cvt_f32_u32"){
            auto v=u(x(1));regs[dst]=expr("asuint(float("+(op=="v_cvt_f32_i32"?"asint("+v+")":v)+"))");
        }else if(op=="v_cvt_u32_f32"){
            auto v="asfloat("+u(x(1))+")";regs[dst]=expr("("+v+">=4294967296.0?0xffffffffu:("+v+">0.0?uint("+v+"):0u))");
        }else if(op=="v_add_f32"){regs[dst]=expr("asuint(asfloat("+u(x(1))+")+asfloat("+u(x(2))+"))");}
        else if(op=="v_add_nc_u32"){auto l=x(1),r=x(2);regs[dst]=fixed(l) && fixed(r)?
            folded(l,r,l.n+r.n):expr(u(l)+"+"+u(r));}
        else if(op=="v_lshl_add_u32" && x(3).k==K::stackbase)regs[dst]={K::stackaddr,number(x(1))<< (number(x(2))&31)};
        else if(op=="v_lshl_add_u32"){auto l=x(1),r=x(3);regs[dst]=folded(l,r,(l.n<<(number(x(2))&31))+r.n);}
        else if(op=="v_lshrrev_b32" && fixed(x(2))){auto v=x(2);v.n>>=number(x(1))&31;regs[dst]=v;}
        else if(op=="v_and_b32" && (x(1).k==K::instance_word || x(2).k==K::instance_word)){
            auto other=x(x(1).k==K::instance_word?2:1);if(number(other)!=0xffffffu)fail("unmapped instance descriptor bits");
            regs[dst]=expr("InstanceID()");
        }else if(op=="v_and_b32" || op=="v_or_b32" || op=="s_and_b32" || op=="s_or_b32" || op=="s_xor_b32"){
            auto l=x(1),r=x(2);bool is_and=op=="s_and_b32" || op=="v_and_b32",is_xor=op=="s_xor_b32";
            regs[dst]=fixed(l)&&fixed(r)?folded(l,r,is_and?(l.n&r.n):is_xor?(l.n^r.n):(l.n|r.n)):
                expr("("+u(l)+(is_and?" & ":is_xor?" ^ ":" | ")+u(r)+")");
        }else if(op=="v_cmp_eq_u32" || op=="v_cmp_gt_u32" || op=="v_cmp_ge_u32" || op=="v_cmp_le_u32"){
            auto l=x(1),r=x(2);
            // For a committed triangle both node references are valid. The
            // triangle selector is internal to the logical leaf representation.
            if(fixed(l)&&fixed(r))regs[dst]=lit((op=="v_cmp_eq_u32"?l.n==r.n:op=="v_cmp_gt_u32"?l.n>r.n:op=="v_cmp_ge_u32"?l.n>=r.n:l.n<=r.n)?~0u:0u);
            else regs[dst]=expr("(0u-uint("+u(l)+(op=="v_cmp_eq_u32"?" == ":op=="v_cmp_gt_u32"?" > ":op=="v_cmp_ge_u32"?" >= ":" <= ")+u(r)+"))");
        }else if(op=="s_and_saveexec_b32"){
            bool next=active && number(x(1));regs[dst]=lit(active?~0u:0u);active=next;
        }else if(op=="v_add_co_u32" || op=="v_add_co_ci_u32"){
            auto l=x(2),r=x(3);u64 sum=u64(l.n)+r.n+(op=="v_add_co_ci_u32"?u64(number(x(4))!=0):0ull);
            regs[dst]=folded(l,r,u32(sum));regs[a[1]]=lit(u32(sum>>32));
        }else if(op=="v_lshlrev_b64"){
            auto src=range(a[2]),d=range(dst);auto lo=read(src.at(0)),hi=read(src.at(1));
            if(src.count!=2 || d.count!=2 || !fixed(lo)||!fixed(hi))fail("unmapped double-word hit address");
            auto v=(u64(lo.n)|(u64(hi.n)<<32))<<(number(x(1))&63);
            regs[d.at(0)]=folded(lo,hi,u32(v));regs[d.at(1)]=folded(lo,hi,u32(v>>32));
        }else if(op=="global_load_dword" && a[2]=="off"){
            auto addr=range(a[1]);auto l=read(addr.at(0)),h=read(addr.at(1));
            if(!fixed(l)||!fixed(h)||h.n||l.k!=K::hit_addr)fail("not a logical hit-node field");
            if(l.n==tlas+node+0x70)regs[dst]=expr("InstanceIndex()");
            else if(l.n==tlas+node+0x30)regs[dst]={K::instance_word};
            else if(l.n==tlas+node+0x3c)regs[dst]={K::hit_addr,blas>>8};
            else if(l.n==blas+node+52)regs[dst]=expr("PrimitiveIndex()");
            else fail("unmapped hit-node field");
        }
        else if(op=="s_getpc_b64"){auto d=range(dst);regs[d.at(0)]={K::pc,i.pc+i.size};regs[d.at(1)]=lit(0);}
        else if(op=="s_add_u32" && x(1).k==K::pc){auto old=x(1).n,n=old+number(x(2));regs[dst]={K::pc,n};regs["scc"]=lit(n<old);}
        else if(op=="s_addc_u32")regs[dst]=lit(number(x(1))+number(x(2))+number(read("scc")));
        else if(op=="s_lshr_b64" || op=="v_lshrrev_b64"){
            auto src=range(a[op[0]=='s'?1:2]),d=range(dst);auto lo=read(src.at(0)),hi=read(src.at(1));
            if(number(x(op[0]=='s'?2:1))!=8 || src.count!=2 || d.count!=2)fail("unknown ray pointer shift");
            if(lo.k==K::pc && hi.k==K::constant && hi.n==0 && !(lo.n&255))regs[d.at(0)]={K::cont,lo.n};
            else if(lo.k==K::as_low && hi.k==K::as_high && lo.n==hi.n)regs[d.at(0)]={K::as_node,lo.n};
            else fail("unknown ray pointer provenance");
            regs[d.at(1)]=lit(0);
        }else if(op=="global_load_dwordx2" && a[2]=="off" && i.modifiers.empty()){
            auto src=range(a[1]),d=range(dst);auto ptr=read(src.at(0));
            if(ptr.k!=K::table || src.count!=2 || number(read(src.at(1))) || d.count!=2)fail("unknown AS descriptor address");
            resource(binding(ptr.n,0),true);regs[d.at(0)]={K::as_low,ptr.n};regs[d.at(1)]={K::as_high,ptr.n};
        }else if(op=="buffer_load_dword" || op=="buffer_load_dwordx3" || op=="buffer_store_dword"){
            bool store=op=="buffer_store_dword";auto srd=range(a[2]),d=range(dst);auto addr=x(1);
            auto mod=View(i.modifiers);auto sp=mod.find(' ');auto mode=mod.substr(0,sp);u32 off=0;
            if(sp!=mod.npos){mod.remove_prefix(sp+1);if(!mod.starts_with("offset:"))fail("unmapped buffer modifier");off=integer(mod.substr(7));}
            if(off%4)fail("unaligned ray word");
            if(srd.first==0 && srd.count==4 && mode=="offen" && a[3]=="s4" && addr.k==K::stackaddr){
                for(u32 n=0;n<d.count;++n){
                    auto loc=addr.n+off+4*n;
                    if(store)stack[loc]=read(d.at(n));
                    else{if(!stack.contains(loc))fail("read of unbound ray stack slot");regs[d.at(n)]=stack.at(loc);}
                }
            }else if(mode=="idxen" && a[3]=="0"){
                auto desc=read(srd.at(0));
                if(desc.k!=K::srd || desc.n%4 || srd.count!=4)fail("missing ray buffer SRD");
                for(u32 n=1;n<4;++n){auto part=read(srd.at(n));if(part.k!=desc.k || part.n!=desc.n+n || part.s!=desc.s)fail("modified ray SRD");}
                auto b=binding(desc.n/4,integer(desc.s));if(b.type!=u32(store))fail("wrong ray buffer descriptor type");
                auto name=resource(b);auto stride=strides.at(b.name());
                if(off+4ull*d.count>stride)fail("ray load crosses supplied view stride");
                for(u32 n=0;n<d.count;++n){
                    auto cell=std::format("{}[{}].w[{}]",name,u(addr),off/4+n);
                    if(store)body<<"    "<<cell<<" = "<<u(read(d.at(n)))<<";\n";else regs[d.at(n)]=expr(cell);
                }
            }else fail("unmapped ray buffer address");
        }else if(op=="v_cmp_ngt_f32")regs[dst]=expr("uint(!(asfloat("+u(x(1))+")>asfloat("+u(x(2))+")))");
        else if(op=="v_cndmask_b32"){
            auto c=x(3);regs[dst]=c.k==K::constant?(c.n?x(2):x(1)):expr(u(c)+"!=0u ? "+u(x(2))+" : "+u(x(1)));
        }
        else fail("unmapped ray instruction "+op);
        return false;
    }
    void block(const RtEntry& entry){
        u32 end=m.code.size();for(auto& e:m.entries)if(e.offset>entry.offset)end=std::min(end,e.offset);
        if(entry.offset)for(unsigned i=0;i<3;++i)
            if(m.code.at<u64>(entry.offset+8*i)!=0xd5800000ull)fail("unknown ray function preamble");
        for(u32 pc=entry.offset+24;pc<end;){
            try{auto i=isa::decode(m.code,pc,32);if(pc+i.size>end)fail("RT instruction crosses symbol");
                body<<std::format("    // +0x{:x} {}\n",pc,i.op);
                if(i.op=="s_cbranch_execz"){
                    auto to=int64_t(pc)+i.size+4ll*std::bit_cast<std::int16_t>(std::uint16_t(m.code.at<u32>(pc)));
                    if(to<=pc || to>=end)fail("unsupported hit branch target");
                    pc=active?pc+i.size:u32(to);continue;
                }
                if(execute(i))return;
                pc+=i.size;
            }catch(const Error& e){throw Unsupported(std::format("{}+0x{:x}: {}",entry.name,pc-entry.offset,e.what()));}
        }
        fail("RT subprogram lacks scheduler return");
    }
public:
    Str launch_guard(const RtEntry& entry){
        // Diagnostic entry: execute the original straight-line launch code
        // and expose its first lane guard. Never substitute this entry for a
        // retail export or give it a source-matched collection sidecar.
        regs["s6"]={K::root};regs["v1"]={K::dispatch};regs["v7"]=lit(0);regs["v26"]={K::stackbase};
        u32 end=m.code.size();for(auto& e:m.entries)if(e.offset>entry.offset)end=std::min(end,e.offset);
        for(u32 pc=entry.offset+24;pc<end;){
            auto i=isa::decode(m.code,pc,32);
            if(i.op=="s_and_saveexec_b32"){
                auto branch=isa::decode(m.code,pc+i.size,32);
                if(branch.op!="s_cbranch_execz" || cb_words.empty())fail("not a guarded ray launch");
                auto mask=u(read(i.args.at(1)));
                return constants()+"RWByteAddressBuffer measured : register(u0,space31);\n"
                    "[shader(\"raygeneration\")] void MeasureLaunchGuard(){\n"+body.str()+
                    "    uint3 id=DispatchRaysIndex(), extent=DispatchRaysDimensions();\n"
                    "    measured.Store(4u*(id.x+extent.x*(id.y+extent.y*id.z)),uint("+mask+" != 0u));\n}\n";
            }
            if(i.op.starts_with("s_branch") || i.op.starts_with("s_cbranch") || i.op.starts_with("buffer_store") ||
               i.op.starts_with("image_") || execute(i))fail("not a straight-line ray launch guard");
            pc+=i.size;
        }
        fail("no ray launch guard in entry");
    }
    Link(const RtModule& m,unsigned stage,unsigned words,View layout):m(m),stage(stage),words(words){
        if(!words || words>32)fail("unsupported ray payload width");
        while(!layout.empty()){
            auto sep=layout.find(',');auto item=layout.substr(0,sep);auto eq=item.find('=');
            if(eq==item.npos || eq<2 || (item[0]!='t' && item[0]!='u'))fail("invalid ray view layout");
            auto stride=integer(item.substr(eq+1));if(!stride || stride>2048 || stride%4)fail("invalid ray view stride");
            auto name=std::format("{}{}",item[0],integer(item.substr(1,eq-1)));
            if(!strides.emplace(name,stride).second)fail("duplicate ray view");
            if(sep==layout.npos)break;
            layout.remove_prefix(sep+1);
        }
    }
    Str run(const RtExport& exported,bool header){
        const RtEntry* start=nullptr;
        auto name=exported.symbols[0];for(auto& e:m.entries)if(e.name==name+".ss_0")start=&e;
        auto role=exported.type;
        if(!start || !((stage==6&&role==0)||(stage==7&&role==4)||(stage==8&&(role==1||role==2))))fail("unsupported ray entry linkage");
        if(name.empty() || !std::ranges::all_of(name,[](unsigned char c){return std::isalnum(c)||c=='_';}))fail("invalid ray export name");
        regs["s6"]={K::root};regs["v7"]=lit(0);regs["v26"]={K::stackbase};stack[0]={K::ret};
        if(stage==7 || stage==8){
            regs["v9"]=lit(1);
            if(stage==8){
                regs["v33"]={K::stackbase};regs["v6"]=expr("asuint(RayTCurrent())");
                regs["v14"]=expr("asuint(attr.barycentrics.x)");regs["v15"]=expr("asuint(attr.barycentrics.y)");
                regs["v2"]={K::hit_addr,14};regs["v3"]={K::hit_addr,8};
                regs["v16"]={K::hit_addr,tlas>>8};regs["v17"]={K::hit_addr,0};
            }
            for(u32 n=0;n<words;++n)stack[4+4*n]={K::expr,0,std::format("p.w[{}]",n)};
            block(*start);if(read("v8").k!=K::ret || !active)fail("unsupported hit/miss continuation");
            for(u32 n=0;n<words;++n){auto dst=std::format("p.w[{}]",n);auto value=u(stack.at(4+n*4));
                if(dst!=value)body<<"    "<<dst<<" = "<<value<<";\n";}
            for(auto& [slot,_]:stack)if(slot>words*4)fail("miss writes outside payload");
        }else{
            regs["v1"]={K::dispatch};block(*start);
            auto as=read("v16");if(as.k!=K::as_node || number(read("v8"))!=42)fail("unsupported raygen scheduler operation");
            auto sp=number(read("v7")),frame=sp*4,payload=number(read("v9"))*4;
            auto cont=stack.find(frame);if(cont==stack.end() || cont->second.k!=K::cont)fail("missing raygen continuation address");
            auto next=std::ranges::find(m.entries,cont->second.n,&RtEntry::offset);
            if(next==m.entries.end() || next->name!=name+".ss_1")fail("unknown trace continuation");
            auto xyz=[&](u32 n){return std::format("asfloat(uint3({},{},{}))",u(read("v"+std::to_string(n))),u(read("v"+std::to_string(n+1))),u(read("v"+std::to_string(n+2))));};
            body<<"    RayDesc r; r.Origin="<<xyz(19)<<"; r.Direction="<<xyz(23)<<";\n"
                <<"    r.TMin=asfloat("<<u(read("v22"))<<"); r.TMax=asfloat("<<u(read("v6"))<<"); P p;\n";
            for(u32 j=0;j<words;++j){auto it=stack.find(payload+j*4);if(it==stack.end())fail("uninitialized trace payload slot");
                body<<std::format("    p.w[{}]={};\n",j,u(it->second));}
            auto packed=u(read("v18"));
            body<<std::format("    TraceRay({}, {}, {}&255u, ({}>>8)&15u, ({}>>12)&15u, {}>>16, r, p);\n",
                resource(binding(as.n,0),true),u(read("v17")),packed,packed,packed,packed);
            // The XS scheduler re-enters a continuation with its own SGPRs,
            // VGPRs and the preserved stack. Do not leak pre-trace temporaries.
            regs.clear();regs["v7"]=lit(sp);regs["v26"]={K::stackbase};regs["s6"]={K::root};regs["v1"]={K::dispatch};
            for(u32 j=0;j<words;++j)stack[payload+j*4]={K::expr,0,std::format("p.w[{}]",j)};
            block(*next);if(read("v8").k!=K::ret)fail("unsupported nested ray continuation");
        }
        std::ostringstream out;if(header)out<<"struct P {uint w["<<words<<"];};\n";out<<constants();for(auto& [_,decl]:declarations)out<<decl<<'\n';
        out<<"[shader(\""<<(role==0?"raygeneration":role==4?"miss":role==2?"closesthit":"anyhit")<<"\")] void "<<name<<"("
           <<(stage!=6?"inout P p":"")<<(stage==8?", BuiltInTriangleIntersectionAttributes attr":"")<<"){\n"<<body.str()<<"}\n";
        return out.str();
    }
};
}
std::string rt_source(const RtCollection& c,unsigned stage,unsigned words,std::string_view layout){
    if(c.modules.empty() || std::ranges::none_of(c.modules.back().entries,[](const auto& e){return e.name=="XDXR_TraceRay.ss_0";}))
        fail("unknown ray scheduler ABI");
    Str result;auto& m=c.modules.back();std::set<Str> names;
    for(auto& e:m.exports)if(!(e.flags&3) && names.insert(e.symbols[0]).second){
        if(stage==6 && std::ranges::find(m.entries,e.symbols[0]+".ss_2",&RtEntry::name)!=m.entries.end()){
            if(!result.empty())fail("multiple public multi-continuation raygens in one library");
            result+=ray_machine_source(m,e,words,layout);
        }else result+=Link(m,stage,words,layout).run(e,result.empty());
    }
    if(result.empty())fail("no ray stage body");
    return result;
}
std::string rt_guard_source(const RtCollection& c){
    const auto& m=c.modules.back();
    auto rg=std::ranges::find_if(m.exports,[](auto& e){return e.type==0 && !(e.flags&3);});
    if(rg==m.exports.end())fail("no public raygen for launch measurement");
    auto entry=std::ranges::find(m.entries,rg->symbols[0]+".ss_0",&RtEntry::name);
    if(entry==m.entries.end())fail("no entry for launch measurement");
    return Link(m,6,1,"").launch_guard(*entry);
}
}
