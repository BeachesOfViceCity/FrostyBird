#include "rt_machine.hpp"
#include "bit_transform.hpp"
#include "view_fields.hpp"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cctype>
#include <deque>
#include <format>
#include <optional>
#include <sstream>

namespace frosty {
namespace {
using S=std::string;using V=std::string_view;using I=isa::Instruction;
[[noreturn]] void bad(V msg){throw Unsupported(S(msg));}
u32 imm(V s){
    if(s=="null")return 0;
    if(s.starts_with("lit(")&&s.ends_with(')'))return imm(s.substr(4,s.size()-5));
    if(s.contains('.') && !s.starts_with("0x")){
        float f;auto [p,err]=std::from_chars(s.data(),s.data()+s.size(),f);
        if(err==std::errc{} && p==s.data()+s.size())return std::bit_cast<u32>(f);
        bad("invalid floating immediate");
    }
    bool neg=s.starts_with('-');if(neg)s.remove_prefix(1);
    int base=s.starts_with("0x")?16:10;if(base==16)s.remove_prefix(2);
    u32 v;auto [p,err]=std::from_chars(s.data(),s.data()+s.size(),v,base);
    if(err!=std::errc{} || p!=s.data()+s.size())bad("expected ray instruction immediate");
    return neg?0u-v:v;
}
bool gpr(V s){return s.size()>1 && (s[0]=='s'||s[0]=='v') && std::isdigit(static_cast<unsigned char>(s[1]));}
struct R {char bank;u32 lo,count;S at(u32 i=0)const{return std::format("{}{}",bank,lo+i);} };
R rr(V s){
    if(s.size()<2 || (s[0]!='s'&&s[0]!='v'))bad("invalid ray register tuple");
    char b=s[0];s.remove_prefix(1);u32 lo,hi;
    if(s.starts_with('[')&&s.ends_with(']')){s=s.substr(1,s.size()-2);auto pos=s.find(':');if(pos==V::npos)bad("invalid ray tuple");
        lo=imm(s.substr(0,pos));hi=imm(s.substr(pos+1));}else lo=hi=imm(s);
    if(lo>hi || hi>=(b=='s'?106u:256u))bad("invalid ray register index");
    return {b,lo,hi-lo+1};
}
bool branch(const I& i){return i.op=="s_branch"||i.op.starts_with("s_cbranch_");}
bool terminal(const I& i){return i.op=="s_setpc_b64" && i.args==std::vector<S>{"s[30:31]"};}
u32 dest(const RtModule& m,const I& i){return u32(int64_t(i.pc)+i.size+4ll*std::bit_cast<std::int16_t>(std::uint16_t(m.code.at<u32>(i.pc))));}
RayPart graph(const RtModule& m,const RtEntry& e){
    u32 end=m.code.size();for(auto& en:m.entries)if(en.offset>e.offset)end=std::min(end,en.offset);
    RayPart g{e.offset+24,{},{}};std::set<u32> work{g.start},heads{g.start};std::map<u32,I> ins;
    auto edge=[&](u32 to){if(to<g.start || to>=end || to%4)bad("ray branch outside continuation");heads.insert(to);work.insert(to);};
    while(!work.empty()){
        u32 pc=*work.begin();work.erase(work.begin());
        while(!ins.contains(pc)){
            if(pc>=end)bad("ray continuation falls through its allocation");
            auto i=isa::decode(m.code,pc,32);auto next=pc+i.size;
            auto it=ins.lower_bound(pc);
            if(next>end || (it!=ins.end() && next>it->first) || (it!=ins.begin() && std::prev(it)->first+std::prev(it)->second.size>pc))
                bad("ray branch crosses instruction");
            ins.emplace(pc,i);
            if(terminal(i))break;
            if(i.op=="s_setpc_b64" || i.op=="s_endpgm" || i.op=="s_swappc_b64")bad("unknown ray scheduler transfer");
            if(branch(i)){edge(dest(m,i));if(i.op!="s_branch")edge(next);break;}
            pc=next;
        }
    }
    for(auto head:heads){
        RayBlock b{head,{},{}};
        for(u32 pc=head;;){
            const auto& i=ins.at(pc);auto next=pc+i.size;b.ins.push_back(i);
            if(terminal(i))break;
            if(branch(i)){b.edges.push_back(dest(m,i));if(i.op!="s_branch")b.edges.push_back(next);break;}
            if(heads.contains(next)){b.edges.push_back(next);break;}pc=next;
        }
        g.blocks.emplace(head,std::move(b));
    }
    std::map<u32,u32> degree;for(auto& [h,b]:g.blocks){degree.try_emplace(h,0);for(auto to:b.edges)++degree[to];}
    std::deque<u32> q;for(auto [h,n]:degree)if(!n)q.push_back(h);
    while(!q.empty()){auto h=q.front();q.pop_front();g.order.push_back(h);for(auto to:g.blocks.at(h).edges)if(!--degree[to])q.push_back(to);}
    if(g.order.size()!=g.blocks.size())g.order.clear();
    return g;
}
RayBind binding(const RtModule& m,u32 table,u32 offset){
    auto r=m.root;
    if(!r.size() || r.at<u32>(0)!=1 || table>=r.at<u32>(4))bad("missing original ray root table");
    auto p=r.tail(r.at<u32>(8)+12ull*table);
    if(p.at<u32>(0)!=0)bad("ray parameter is not a descriptor table");
    auto t=r.tail(p.at<u32>(8));if(t.at<u32>(0)!=1)bad("multiple ray descriptor ranges");
    auto d=r.tail(t.at<u32>(4));auto type=d.at<u32>(0),stride=type==3?16u:32u;
    if(type>3 || d.at<u32>(16)!=~0u || offset%stride || offset/stride>=d.at<u32>(4))bad("ray SRD outside root range");
    return {type,d.at<u32>(8)+offset/stride,d.at<u32>(12)};
}
enum class T {value,root,zero,table,srd,dispatch,dims,xshift,yshift,xshifted,stack_desc};
struct Tag{T t{};u32 table{},off{},part{};bool operator==(const Tag&)const=default;};
using Tags=std::map<S,Tag>;
Tag tag(const Tags& s,const S& v){if(v=="0"||v=="null")return {T::zero};auto i=s.find(v);return i==s.end()?Tag{}:i->second;}
void put(Tags& s,const S& v,Tag a){if(a.t==T::value)s.erase(v);else s[v]=a;}
u32 extra_offset(V mod){if(mod.empty())return 0;if(!mod.starts_with("offset:"))bad("unsupported flat load modifiers");return imm(mod.substr(7));}
void transfer(const RtModule& m,const I& i,Tags& s,RayProgram* result){
    const auto& a=i.args;V op=i.op;
    if(branch(i)||terminal(i)||op=="s_waitcnt"||op=="s_waitcnt_vscnt"||op=="s_nop"||op=="v_nop")return;
    if(a.empty())bad("unexpected ray op without operands");
    auto known=[&](u32 k){return tag(s,a.at(k));};
    auto special=[&](V kind){if(result)result->special[i.pc]=kind;};
    auto access=[&](RayBind b,RayView view,u32 words=0){
        if(!result)return;
        result->access[i.pc]=b;auto [at,added]=result->used.try_emplace(b,RayUse{view,words});
        if(!added){if(at->second.view!=view)bad("incompatible ray view kinds");at->second.words=std::max(at->second.words,words);}
    };
    auto desc=[&](V tuple){auto r=rr(tuple);auto st=tag(s,r.at());
        if(st.t!=T::srd || r.bank!='s' || st.part)bad("no original SRD at ray memory operation");
        for(u32 j=1;j<r.count;++j)if(tag(s,r.at(j))!=Tag{T::srd,st.table,st.off,j})bad("modified original ray SRD");
        return binding(m,st.table,st.off);};
    if(op=="v_mov_b32" || op=="s_mov_b32"){put(s,a[0],known(1));return;}
    if(op=="s_load_dword" || op=="s_load_dwordx4" || op=="s_load_dwordx8"){
        auto addr=rr(a.at(1)),dst=rr(a[0]);auto base=tag(s,addr.at()),hi=tag(s,addr.at(1));u32 off=imm(a.at(2));
        if(addr.bank!='s'||addr.count!=2||dst.bank!='s')bad("invalid ray SMEM range");
        if(base.t==T::root && dst.count==1 && off%4==0 && off/4<m.root.at<u32>(4)){
            put(s,dst.at(),{T::table,off/4});special("table");return;
        }
        if(base.t!=T::table||hi.t!=T::zero)bad("ray SMEM not rooted in the shader table");
        (void)binding(m,base.table,off);
        for(u32 k=0;k<dst.count;++k)put(s,dst.at(k),{T::srd,base.table,off,k});
        special("srd");return;
    }
    if(op=="s_bfe_u32" && known(1).t==T::dims && (a[2]=="0x50010" || a[2]=="0x50018")){
        put(s,a[0],{a[2]=="0x50010"?T::xshift:T::yshift});special("axis");return;
    }
    if(op=="v_lshlrev_b32" && known(1).t==T::xshift && known(2).t==T::dispatch){
        put(s,a[0],{T::xshifted});special("axis");return;
    }
    if(op=="v_lshrrev_b32" && ((known(1).t==T::xshift && known(2).t==T::xshifted) ||
                               (known(1).t==T::yshift && known(2).t==T::dispatch))){
        special(known(1).t==T::xshift?"x":"y");put(s,a[0],{});return;
    }
    if(op.starts_with("s_buffer_load_dword")){
        auto b=desc(a.at(1));auto dst=rr(a[0]);bool indexed=gpr(a.at(2)) && a[2][0]=='s';
        u32 off=indexed?0:imm(a[2]);
        if((b.type!=2 && b.type!=0) || off%4 || dst.count>16 || (b.type==2 && off>65536-4*dst.count))bad("invalid ray scalar buffer load");
        access(b,b.type==2?RayView::cb:RayView::buffer_words,indexed?16384:off/4+dst.count);
    }else if(op=="global_load_dwordx2"){
        auto addr=rr(a.at(1)),dst=rr(a[0]);auto base=tag(s,addr.at());
        if(base.t!=T::table||tag(s,addr.at(1)).t!=T::zero||addr.count!=2||dst.count!=2||a.at(2)!="off")bad("unknown ray AS descriptor address");
        auto b=binding(m,base.table,extra_offset(i.modifiers));if(b.type!=0)bad("AS descriptor must be an SRV");
        access(b,RayView::accel);
    }else if(op=="image_load"||op=="image_load_mip"||op=="image_store"){
        auto b=desc(a.at(2));bool store=op=="image_store",vol=V(i.modifiers).contains("SQ_RSRC_IMG_3D");
        if(b.type!=u32(store) || (store&&vol))bad("invalid ray image resource");
        access(b,store?RayView::rwtex2:vol?RayView::tex3:RayView::tex2);
        if(store)return;
    }else if(op=="buffer_load_format_x"){
        auto b=desc(a.at(2));if(b.type!=0)bad("invalid typed ray buffer");access(b,RayView::typed);
    }else if(op=="buffer_store_dword" || op=="buffer_load_dword"){
        if(a.at(2)!="s[0:3]" || a.at(3)!="s4" || !V(i.modifiers).starts_with("offen"))bad("unmapped ray buffer addressing");
        for(u32 k=0;k<4;++k)if(tag(s,"s"+std::to_string(k))!=Tag{T::stack_desc,0,0,k})bad("modified ray stack descriptor");
        if(op=="buffer_store_dword")return;
    }else if(op.starts_with("s_cmp"))return;
    else if(result){
        // Symbolic descriptor/launch words have no portable arithmetic bit
        // representation. Only the patterns above can consume them.
        for(size_t k=1;k<a.size();++k)if(gpr(a[k])){auto t=known(k).t;
            if(t!=T::value && t!=T::zero)bad("numeric use of an unmapped ray ABI word");}
    }
    if(gpr(a[0]) || (a[0].size()>2 && a[0][1]=='[')){
        auto d=rr(a[0]);for(u32 k=0;k<d.count;++k)put(s,d.at(k),{});
    }
}
void resolve(const RtModule& m,const RayPart& part,RayProgram& out){
    Tags abi{{"s6",{T::root}},{"s7",{T::zero}},{"s13",{T::dims}},{"v1",{T::dispatch}}};
    for(u32 k=0;k<4;++k)abi.emplace("s"+std::to_string(k),Tag{T::stack_desc,0,0,k});
    std::map<u32,Tags> heads{{part.start,abi}};std::deque<u32> q{part.start};
    auto walk=[&](u32 h,Tags& st,RayProgram* result){for(auto& i:part.blocks.at(h).ins)try{transfer(m,i,st,result);}
        catch(const Error& e){throw Unsupported(std::format("ray +0x{:x} {}: {}",i.pc,i.op,e.what()));}};
    while(!q.empty()){
        auto h=q.front();q.pop_front();auto st=heads.at(h);walk(h,st,nullptr);
        for(auto to:part.blocks.at(h).edges){
            auto [it,added]=heads.try_emplace(to,st);bool change=added;
            if(!added){auto& a=it->second;auto old=a.size();std::erase_if(a,[&](const auto& v){return tag(st,v.first)!=v.second;});change=a.size()!=old;}
            if(change && std::ranges::find(q,to)==q.end())q.push_back(to);
        }
    }
    for(auto& [h,st]:heads)walk(h,st,&out);
}
}
std::string RayBind::name()const{return std::format("{}{}",type==0?'t':type==1?'u':type==2?'b':'s',reg);}
RayProgram ray_program(const RtModule& m,const RtExport& e){
    RayProgram p{};if(e.type!=0 || (e.flags&3) || e.frame%4 || e.frame>32768 || m.stack>32768)bad("unsupported raygen frame");
    p.frame_words=e.frame/4;p.stack_words=std::max(p.frame_words+1,(m.stack+3)/4);
    for(unsigned n=0;;++n){auto it=std::ranges::find(m.entries,e.symbols[0]+".ss_"+std::to_string(n),&RtEntry::name);
        if(it==m.entries.end())break;
        auto& part=p.parts.emplace_back(graph(m,*it));resolve(m,part,p);}
    if(p.parts.empty())bad("missing raygen continuation sequence");
    return p;
}

namespace {
// The packet contains only the XS scheduler ABI registers. Private
// temporaries belong to the individual continuation; the software call
// frame and the hardware payload are separate and persist across TraceRay.
constexpr std::array<u32,13> packet_regs{6,7,8,9,16,17,18,19,20,21,22,23,24};
constexpr u32 virtual_code_base=0x1000000;
S hreg(V s){
    if(s=="exec_lo")return "xm";
    if(s=="vcc_lo")return "vc";
    if(s=="scc")return "cc";
    if(gpr(s)){(void)rr(s);return S(s);}
    bad("invalid ray ALU destination");
}
S bits(V s){
    if(s.starts_with('-') && s.size()>1 && (s[1]=='|'||s[1]=='v'||s[1]=='s'))
        return "("+bits(s.substr(1))+" ^ 0x80000000u)";
    if(s.starts_with('|') && s.ends_with('|'))return "("+bits(s.substr(1,s.size()-2))+" & 0x7fffffffu)";
    if(gpr(s)||s=="exec_lo"||s=="vcc_lo"||s=="scc")return hreg(s);
    return std::format("0x{:x}u",imm(s));
}
S ident(RayBind b){return std::format("res_{}_{}",b.space,b.name());}
std::vector<S> address(V a){
    std::vector<S> v;
    if(a.starts_with('[')&&a.ends_with(']')){
        a=a.substr(1,a.size()-2);
        while(!a.empty()){auto p=a.find(',');auto el=a.substr(0,p);while(el.starts_with(' '))el.remove_prefix(1);
            if(!gpr(el))bad("invalid ray image address");
            v.emplace_back(el);
            if(p==V::npos)break;
            a.remove_prefix(p+1);}
    }else{auto r=rr(a);for(u32 k=0;k<r.count;++k)v.push_back(r.at(k));}
    return v;
}
struct ImageOp {u32 mask;bool volume;};
ImageOp image_op(const I& i){
    V mod=i.modifiers;constexpr V prefix="dmask:";if(!mod.starts_with(prefix))bad("missing ray image mask");
    auto n=mod.find(' ');if(n==V::npos)bad("missing ray image dimension");auto mask=imm(mod.substr(prefix.size(),n-prefix.size()));auto tail=mod.substr(n);
    bool volume=tail==" dim:SQ_RSRC_IMG_3D unorm";if((tail!=" dim:SQ_RSRC_IMG_2D unorm" && !volume)||!mask||mask>15)bad("unsupported ray image modifiers");
    return {mask,volume};
}
class RayEmit {
    const RtModule& m;const RtExport& e;const RayProgram& p;u32 words,storage_words;
    std::optional<RayContract> contract;
    std::map<S,S> types;std::map<RayBind,u32> handles;
    std::map<RayBind,u32> strides;
    isa::BitTransforms bit_transforms;
    std::ostringstream out;
    std::ostream* block_output{};
    void line(const S& s){(block_output?*block_output:out)<<"    "<<s<<'\n';}
    S value_type(RayBind b)const{
        auto i=types.find(b.name());if(i==types.end())bad("missing ray view type for "+b.name()+" (supply float, uint or int)");
        return i->second;
    }
    void declarations(){
        if(contract)out<<"RWByteAddressBuffer xs_status:register(u0,space21301);\n"
            "void xs_fault(uint what,uint arg,uint flags,uint p){uint old;xs_status.InterlockedCompareExchange(0,0,what,old);"
            "if(!old){xs_status.Store3(4,uint3(arg,flags,p));}}\n";
        out<<std::format("struct P {{uint w[{}];}};\nstruct XsPacket {{",words);
        for(auto r:packet_regs)out<<"uint v"<<r<<";";
        out<<"uint v25;};\n";
        for(auto [b,u]:p.used){
            auto id=ident(b),bind=std::format("register({},space{})",b.name(),b.space);
            if(u.view==RayView::cb)out<<std::format("cbuffer C{}_{} : {} {{uint4 {}[{}];}};\n",b.reg,b.space,bind,id,(u.words+3)/4);
            else if(u.view==RayView::accel){handles.emplace(b,u32(handles.size())+1);out<<"RaytracingAccelerationStructure "<<id<<" : "<<bind<<";\n";}
            else if(u.view==RayView::buffer_words){
                auto ty=value_type(b);
                if(ty=="raw")out<<"ByteAddressBuffer "<<id<<" : "<<bind<<";\n";
                else{
                    auto bytes=imm(ty);if(!bytes || bytes%4 || bytes>2048)bad("invalid ray structured view stride");
                    strides[b]=bytes/4;
                    out<<std::format("struct E_{0} {{uint w[{1}];}};StructuredBuffer<E_{0}> {0} : {2};\n",id,bytes/4,bind);
                }
            }else{
                S ty=u.view==RayView::typed?"Buffer":u.view==RayView::tex2?"Texture2D":u.view==RayView::tex3?"Texture3D":"RWTexture2D";
                auto scalar=value_type(b);if(scalar!="float"&&scalar!="int"&&scalar!="uint")bad("expected ray image numeric type for "+b.name());
                out<<ty<<"<"<<scalar<<(u.view==RayView::typed?"":"4")<<"> "<<id<<" : "<<bind<<";\n";
            }
        }
        out<<R"(
uint xs_ballot(bool pred){uint4 b=WaveActiveBallot(pred);return b[WaveGetLaneIndex()>>5u];}
uint xs_hi(uint x,uint y){uint a=x&65535u,b=x>>16,c=y&65535u,d=y>>16;
    uint t=b*c+((a*c)>>16);return b*d+(t>>16)+(((t&65535u)+a*d)>>16);}
uint2 xs_shr(uint lo,uint hi,uint n){n&=63u;if(!n)return uint2(lo,hi);
    return n>=32u?uint2(hi>>(n-32u),0u):uint2((lo>>n)|(hi<<(32u-n)),hi>>n);}
uint xs_uint(float f){return !(f>0.0f)?0u:f>=4294967296.0f?~0u:uint(f);}
float xs_legacy(float a,float b){return min(abs(a),abs(b))==0.0?0.0:a*b;}
)";
    }
    void instruction(const I& i){
        V op=i.op;const auto& a=i.args;
        if(op=="s_waitcnt" || op=="s_waitcnt_vscnt" || op=="s_nop" || op=="v_nop")return;
        auto x=[&](size_t k){return bits(a.at(k));};auto f=[&](size_t k){return "asfloat("+x(k)+")";};
        auto vput=[&](const S& d,const S& expr){line("if(live) "+hreg(d)+" = "+expr+";");};
        auto sput=[&](const S& d,const S& expr){if(d!="null")line(hreg(d)+" = "+expr+";");};
        if(auto it=p.special.find(i.pc);it!=p.special.end()){
            if(it->second=="x" || it->second=="y")vput(a[0],"DispatchRaysIndex()."+it->second);
            return; // tag only: consumers of tables, SRDs and axis selectors were validated by resolve()
        }
        if(auto it=p.access.find(i.pc);it!=p.access.end()){
            auto b=it->second;auto id=ident(b);auto d=rr(a[0]);
            if(op.starts_with("s_buffer_load_dword")){
                line("{ uint idx=("+x(2)+")>>2u;");
                for(u32 k=0;k<d.count;++k){
                    if(b.type==2)sput(d.at(k),std::format("{}[(idx+{}u)>>2u][(idx+{}u)&3u]",id,k,k));
                    else if(auto s=strides.find(b);s!=strides.end())sput(d.at(k),std::format("{0}[(idx+{1}u)/{2}u].w[(idx+{1}u)%{2}u]",id,k,s->second));
                    else sput(d.at(k),std::format("{}.Load((idx+{}u)*4u)",id,k));
                }
                line("}");return;
            }
            if(op=="global_load_dwordx2"){
                vput(d.at(0),std::format("{}u",handles.at(b)<<8));vput(d.at(1),"0u");return;
            }
            if(op=="buffer_load_format_x"){
                if(d.count!=1 || i.modifiers!="idxen" || a.at(3)!="0")bad("extended ray typed-buffer load");
                vput(a[0],"asuint("+id+"["+x(1)+"])");return;
            }
            auto [mask,volume]=image_op(i);auto addr=address(a[1]);u32 dims=volume?3:2;
            bool mip=op=="image_load_mip",store=op=="image_store";
            if(addr.size()!=dims+u32(mip) || d.count!=u32(std::popcount(mask)))bad("wrong ray MIMG tuple size");
            S coords;for(auto& n:addr)coords+=(coords.empty()?"":",")+n;
            if(!store){
                if(!mip)coords+=",0";
                line(std::format("if(live){{uint4 loaded=asuint({}.Load(int{}({})));",id,dims+1,coords));
                for(u32 k=0,j=0;k<4;++k)if(mask&(1u<<k))line(std::format("{}=loaded[{}];",d.at(j++),k));
                line("}");
            }else{
                if(mask!=15 || dims!=2)bad("partial ray UAV store");
                S v=std::format("uint4({},{},{},{})",d.at(0),d.at(1),d.at(2),d.at(3));
                S ty=value_type(b);if(ty!="uint")v=(ty=="float"?"asfloat(":"asint(")+v+")";
                line(std::format("if(live) {}[uint2({})]={};",id,coords,v));
            }
            return;
        }
        if(op=="buffer_load_dword" || op=="buffer_store_dword"){
            V mod=i.modifiers;if(!mod.starts_with("offen"))bad("unsupported stack access");mod.remove_prefix(5);if(mod.starts_with(' '))mod.remove_prefix(1);
            auto off=extra_offset(mod);if(off%4 || off/4>=p.stack_words)bad("stack offset outside serialized allocation");
            if(storage_words!=p.stack_words){
                line(std::format("if(live){{uint slot=({}+{}u)>>2u;if(slot>={}u){{xs_fault(5u,slot,{}u,{}u);ctx.v8=~0u;return;}}",
                    x(1),off,p.stack_words,i.pc,p.stack_words));
                line(op=="buffer_load_dword"?hreg(a[0])+"=stk[slot];}":"stk[slot]="+x(0)+";}");
                return;
            }
            auto cell=std::format("stk[({}+{}u)>>2u]",x(1),off);
            if(op=="buffer_load_dword")vput(a[0],cell);else line("if(live) "+cell+"="+x(0)+";");return;
        }
        if(op=="s_getpc_b64"){
            auto d=rr(a[0]);if(d.count!=2)bad("invalid ray GETPC destination");
            sput(d.at(),std::format("{}u",virtual_code_base+i.pc+i.size));sput(d.at(1),"0u");return;
        }
        if(op=="s_lshr_b64" || op=="v_lshrrev_b64"){
            bool v=op[0]=='v';auto src=rr(a[v?2:1]),dst=rr(a[0]);if(src.count!=2||dst.count!=2)bad("bad ray shift tuple");
            line(std::format("{}{{uint2 q=xs_shr({},{},{});{}=q.x;{}=q.y;}}",v?"if(live) ":"",src.at(),src.at(1),x(v?1:2),dst.at(),dst.at(1)));
            if(!v)sput("scc","uint(("+dst.at()+"|"+dst.at(1)+")!=0u)");
            return;
        }
        if(op=="v_readfirstlane_b32"){
            sput(a[0],std::format("WaveReadLaneAt({},(WaveGetLaneIndex()&~31u)+uint(firstbitlow(xm)))",x(1)));return;
        }
        if(op=="s_and_saveexec_b32"){
            line(std::format("{{uint old=xm;xm&={};{}=old;cc=uint(xm!=0u);live=(xm&lane_bit)!=0u;}}",x(1),hreg(a[0])));return;
        }
        if(op.starts_with("v_cmp_") || op.starts_with("s_cmp_")){
            bool vec=op[0]=='v',fp=op.ends_with("_f32"),sg=op.ends_with("_i32");size_t k=vec?1:0;
            S lhs=x(k),rhs=x(k+1);if(fp){lhs="asfloat("+lhs+")";rhs="asfloat("+rhs+")";}else if(sg){lhs="asint("+lhs+")";rhs="asint("+rhs+")";}
            V cmp=op.substr(6,op.size()-10);S rel;bool neg=false;
            if(cmp=="eq")rel="==";else if(cmp=="ne"||cmp=="lg")rel="!=";else if(cmp=="ge")rel=">=";else if(cmp=="le")rel="<=";
            else if(cmp=="gt"||cmp=="ngt"){rel=">";neg=cmp=="ngt";}else if(cmp=="lt")rel="<";else bad("unknown ray comparison");
            S test=(neg?"!(":"(")+lhs+rel+rhs+")";if(fp && cmp=="lg")test="(!isnan("+lhs+")&&!isnan("+rhs+")&&"+test+")";
            sput(vec?a[0]:"scc",vec?"xs_ballot(live && "+test+")":"uint("+test+")");return;
        }
        if(op=="v_cndmask_b32"){vput(a[0],"("+x(3)+" & lane_bit) ? "+x(2)+" : "+x(1));return;}
        if(op=="s_add_u32"||op=="s_addc_u32"||op=="s_add_i32"||op=="s_sub_i32"){
            bool sub=op=="s_sub_i32",carry=op=="s_addc_u32";S result=sub?"l-r":"l+r";if(carry)result+="+cc";
            S flag=sub?"((l^r)&(l^n))>>31u":op=="s_add_i32"?"((~(l^r))&(l^n))>>31u":carry?"uint(n<l||(cc!=0u&&n==l))":"uint(n<l)";
            line(std::format("{{uint l={},r={},n={};cc={};{}=n;}}",x(1),x(2),result,flag,hreg(a[0])));return;
        }
        bool vec=op.starts_with("v_");S v,fp;bool flag=false;
        if(op=="s_mov_b32"||op=="v_mov_b32")v=x(1);
        else if(op=="s_cselect_b32")v="cc!=0u?"+x(1)+":"+x(2);
        else if(op=="s_or_b32"||op=="s_and_b32"||op=="s_andn2_b32"||op=="s_xor_b32"||op=="v_or_b32"||op=="v_and_b32"||op=="v_xor_b32"){
            v=x(1)+(op.contains("andn2")?" & ~":op.contains("and")?" & ":op.contains("xor")?" ^ ":" | ")+x(2);flag=!vec;
        }else if(op=="v_xor_b32_sdwa"){
            V prefix="dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:DWORD src1_sel:";V m=i.modifiers;
            if(!m.starts_with(prefix))bad("unmapped ray SDWA");
            m.remove_prefix(prefix.size());
            if(m!="BYTE_0"&&m!="WORD_0")bad("unmapped ray SDWA selector");
            v=x(1)+" ^ ("+x(2)+(m=="BYTE_0"?" & 255u)":" & 65535u)");
        }else if(op=="s_lshl_b32"||op=="v_lshlrev_b32"||op=="v_lshrrev_b32"||op=="v_ashrrev_i32"){
            S val=x(vec?2:1),shift="("+x(vec?1:2)+"&31u)";
            v=op=="v_ashrrev_i32"?"asuint(asint("+val+")>>"+shift+")":val+(op.contains("lshl")?"<<":">>")+shift;flag=!vec;
        }else if(op=="s_bfe_u32")v=std::format("xs_bfe({},{},({}>>16u)&127u)",x(1),x(2),x(2)),flag=true;
        else if(op=="v_bfe_u32")v=std::format("xs_bfe({},{},({}&31u))",x(1),x(2),x(3));
        else if(op=="v_add_nc_u32"||op=="v_sub_nc_u32"||op=="v_subrev_nc_u32")
            v=op=="v_subrev_nc_u32"?x(2)+"-"+x(1):x(1)+(op=="v_add_nc_u32"?"+":"-")+x(2);
        else if(op=="v_add3_u32")v=x(1)+"+"+x(2)+"+"+x(3);
        else if(op=="v_and_or_b32")v="("+x(1)+" & "+x(2)+") | "+x(3);
        else if(op=="v_or3_b32")v=x(1)+"|"+x(2)+"|"+x(3);
        else if(op=="v_xad_u32")v="("+x(1)+"^"+x(2)+")+"+x(3);
        else if(op=="v_lshl_add_u32"||op=="v_lshl_or_b32")v="("+x(1)+"<<("+x(2)+"&31u))"+(op.contains("add")?"+":"|")+x(3);
        else if(op=="s_mul_i32"||op=="v_mul_lo_u32")v=x(1)+"*"+x(2);
        else if(op=="s_mul_hi_u32"||op=="v_mul_hi_u32")v="xs_hi("+x(1)+","+x(2)+")";
        else if(op=="v_min_u32")v="min("+x(1)+","+x(2)+")";
        else if(op=="v_bfrev_b32")v="reversebits("+x(1)+")";
        else if(op=="v_cvt_f32_i32")fp="float(asint("+x(1)+"))";
        else if(op=="v_cvt_f32_u32")fp="float("+x(1)+")";
        else if(op=="v_cvt_u32_f32")v="xs_uint("+f(1)+")";
        else if(op=="v_add_f32"||op=="v_sub_f32"||op=="v_subrev_f32")fp=op=="v_subrev_f32"?f(2)+"-"+f(1):f(1)+(op=="v_add_f32"?"+":"-")+f(2);
        else if(op=="v_mul_legacy_f32")fp="xs_legacy("+f(1)+","+f(2)+")";
        else if(op=="v_fma_f32"||op=="v_fmac_f32")fp=std::format("mad({},{},{})",f(1),f(2),f(op=="v_fma_f32"?3:0));
        else if(op=="v_rcp_f32"||op=="v_rcp_iflag_f32")fp="1.0f/"+f(1);
        else if(op=="v_max_f32"||op=="v_min_f32")fp=S(op=="v_min_f32"?"min(":"max(")+f(1)+","+f(2)+")";
        else if(op=="v_min3_f32"||op=="v_max3_f32"){S fn=op=="v_min3_f32"?"min":"max";fp=std::format("{0}({0}({1},{2}),{3})",fn,f(1),f(2),f(3));}
        else if(op=="v_sin_f32"||op=="v_cos_f32")fp=S(op=="v_cos_f32"?"cos(":"sin(")+"6.283185307179586f*"+f(1)+")";
        else{
            static const std::map<V,V> unary{{"v_ceil_f32","ceil"},{"v_rndne_f32","round"},{"v_rsq_f32","rsqrt"},
                {"v_sqrt_f32","sqrt"},{"v_exp_f32","exp2"},{"v_log_f32","log2"},{"v_trunc_f32","trunc"}};
            if(auto it=unary.find(op);it!=unary.end())fp=S(it->second)+"("+f(1)+")";else bad("unmapped RT ISA "+S(op));
        }
        if(!i.modifiers.empty() && op!="v_xor_b32_sdwa"){
            if(i.modifiers=="clamp" && !fp.empty())fp="saturate("+fp+")";else bad("unmapped RT ALU modifier");
        }
        if(!fp.empty())v="asuint("+fp+")";
        if(vec)vput(a[0],v);
        else if(flag){line("{uint result="+v+";");sput(a[0],"result");line("cc=uint(result!=0u);}");}
        else sput(a[0],v);
        if(a[0]=="exec_lo")line("live=(xm&lane_bit)!=0u;");
    }
    void part(const RayPart& sub,u32 n){
        out<<std::format("void xs_part{}(inout XsPacket ctx,inout uint stk[{}]){{\n",n,storage_words);
        std::set<S> names;
        for(auto& [_,block]:sub.blocks)for(auto& i:block.ins)for(const auto& operand:i.args){
            V a=operand;if(a.starts_with('-'))a.remove_prefix(1);if(a.starts_with('|')&&a.ends_with('|'))a=a.substr(1,a.size()-2);
            if(gpr(a) || (a.size()>3 && (a[0]=='v'||a[0]=='s') && a[1]=='[')){
                auto r=rr(a);for(u32 k=0;k<r.count;++k)names.insert(r.at(k));
            }else if(a.starts_with('['))for(auto& v:address(a))names.insert(v);
        }
        std::set<S> packet{"v25"};for(u32 r:packet_regs)packet.insert(std::format("v{}",r));
        for(auto& name:names)line("uint "+name+" = "+(packet.contains(name)?"ctx."+name:"0u")+";");
        line("uint lane_bit=1u<<(WaveGetLaneIndex()&31u),xm=xs_ballot(true),vc=0u,cc=0u;bool live=true;");
        line(std::format("uint pc={}u;",sub.start));
        std::map<u32,S> bodies,conditions;std::map<u32,std::vector<u32>> edges;
        auto jump=[&](const S& expr){line("pc="+expr+";");};
        for(auto& [h,block]:sub.blocks){
            std::ostringstream text;block_output=&text;edges[h]=block.edges;
            for(const auto& i:block.ins){
                try{
                    if(terminal(i)){
                        for(const auto& name:packet)if(names.contains(name))line("ctx."+name+"="+name+";");
                        line("return;");continue;
                    }
                    if(branch(i)){
                        u32 to=block.edges.at(0);if(i.op=="s_branch"){jump(std::format("{}u",to));continue;}
                        const auto& op=i.op;S test;
                        if(op=="s_cbranch_execz")test="xm==0u";else if(op=="s_cbranch_execnz")test="xm!=0u";
                        else if(op=="s_cbranch_vccz")test="vc==0u";else if(op=="s_cbranch_vccnz")test="vc!=0u";
                        else if(op=="s_cbranch_scc1")test="cc!=0u";else if(op=="s_cbranch_scc0")test="cc==0u";
                        else bad("unmapped ray branch");
                         conditions[h]=test;jump(std::format("({})?{}u:{}u",test,to,block.edges.at(1)));continue;
                    }
                     if(auto it=bit_transforms.saved.find(i.pc);it!=bit_transforms.saved.end())for(auto& s:it->second)
                         line("uint "+s.name+" = v"+std::to_string(s.vg)+";");
                     instruction(i);
                     if(auto it=bit_transforms.result.find(i.pc);it!=bit_transforms.result.end())
                         line("if(live) "+hreg(i.args[0])+" = "+it->second+";");
                }catch(const Error& ex){throw Unsupported(std::format("{}+0x{:x}: {}",e.name,i.pc,ex.what()));}
            }
            if(!branch(block.ins.back())&&!terminal(block.ins.back()))jump(std::format("{}u",block.edges.at(0)));
            bodies[h]=std::move(text).str();block_output=nullptr;
        }
        // Each .ss continuation keeps its original entry and exits back to
        // the checked trace scheduler. Only branches *within* it are reduced.
        isa::emit_resolved_flow(out,bodies,edges,conditions,sub.start);
        out<<"}\n";
    }
public:
    RayEmit(const RtModule& m,const RtExport& e,const RayProgram& p,u32 words,V layout,std::optional<RayContract> c):m(m),e(e),p(p),words(words),storage_words(p.stack_words),contract(c){
        if(c && (c->word>=words || c->selector>0xffffff || words>p.stack_words))bad("invalid ray specialization contract");
        if(c && p.stack_words>=16 && p.stack_words<=64){
            // ACO lowers indirectly indexed private arrays of <=256 bytes to
            // select trees. A medium continuation frame can contain hundreds
            // of such accesses. Let it use native private memory; the checked
            // addressable extent is still precisely the guest frame.
            unsigned accesses=0;
            for(auto& part:p.parts)for(auto& [_,block]:part.blocks)for(auto& i:block.ins)
                accesses+=i.op=="buffer_store_dword" || i.op=="buffer_load_dword";
            if(accesses>=24)storage_words=68;
        }
        while(!layout.empty()){
            auto comma=layout.find(',');auto item=layout.substr(0,comma);auto eq=item.find('=');
            if(eq==V::npos || eq<2)bad("invalid ray view specification");
            auto name=item.substr(0,eq),type=item.substr(eq+1);
            if(name[0]!='t' && name[0]!='u')bad("invalid ray view binding");
            (void)imm(name.substr(1));
            if(!types.emplace(S(name),S(type)).second)bad("duplicate ray view type");
            if(comma==V::npos)break;
            layout.remove_prefix(comma+1);
        }
    }
    S run(){
        if(!words || words>32 || !p.stack_words)bad("unsupported raygen payload/frame");
        if(e.symbols[0].empty() || !std::ranges::all_of(e.symbols[0],[](unsigned char c){return std::isalnum(c)||c=='_';}))bad("invalid ray export");
        declarations();
        for(auto& sub:p.parts)for(auto& [_,b]:sub.blocks){
            std::vector<const I*> block;for(auto& i:b.ins)block.push_back(&i);bit_transforms.analyze(block);}
        if(!bit_transforms.result.empty())out<<isa::BitTransforms::helper;
        line("uint xs_bfe(uint x,uint off,uint n){off&=31u;return n>=32u?(x>>off):((x>>off)&((1u<<n)-1u));}");
        for(u32 k=0;k<p.parts.size();++k)part(p.parts[k],k);
        out<<std::format("uint xs_resume(inout XsPacket c,inout uint stk[{}]){{uint label=c.v8;if(label==42u){{\n",storage_words);
        if(contract)line(std::format("if(c.v7>={0}u || c.v9>{1}u){{xs_fault(2u,c.v18,c.v7,c.v9);return ~0u;}}",p.stack_words,p.stack_words-words));
        if(handles.empty())bad("trace scheduler with no resolved AS");
        line("RayDesc ray;ray.Origin=asfloat(uint3(c.v19,c.v20,c.v21));ray.TMin=asfloat(c.v22);ray.Direction=asfloat(uint3(c.v23,c.v24,c.v25));ray.TMax=asfloat(c.v6);P payload;");
        for(u32 j=0;j<words;++j)line(std::format("payload.w[{0}]=stk[c.v9+{0}u];",j));
        S route="(c.v18>>8u)&15u,(c.v18>>12u)&15u,c.v18>>16u";
        if(contract){
            line(std::format("if((c.v18>>8u)!={0}u || (c.v17&2u)!=0u || payload.w[{1}]!={2}u){{xs_fault(3u,c.v18,c.v17,payload.w[{1}]);return ~0u;}}",
                contract->selector,contract->word,contract->value));
            // Only the successful side of the original runtime check gets
            // constant SBT indices. Ray masks, flags and coordinates stay live.
            route=std::format("{}u,{}u,{}u",contract->selector&15,(contract->selector>>4)&15,contract->selector>>8);
            line(std::format("payload.w[{}]={}u;",contract->word,contract->value));
        }
        line("switch(c.v16){");
        for(auto [as,id]:handles)line(std::format("case {}u:TraceRay({},c.v17,c.v18&255u,{},ray,payload);break;",id,ident(as),route));
        line(S("default:")+(contract?"xs_fault(4u,c.v16,c.v17,c.v18);":"")+"return ~0u;}");
        for(u32 j=0;j<words;++j)line(std::format("stk[c.v9+{0}u]=payload.w[{0}];",j));
        line("label=stk[c.v7];}return label;}");
        out<<"[shader(\"raygeneration\")] void "<<e.symbols[0]<<"(){\n";
        // Every invocation starts in continuation zero with its original
        // frame base. Expose that first call to constant propagation instead
        // of making the initialized stack live around the back edge.
        line(std::format("XsPacket c=(XsPacket)0;uint stk[{}];c.v7={}u;stk[c.v7]=~0u;xs_part0(c,stk);",storage_words,p.frame_words));
        line("[loop] for(;;){uint label=xs_resume(c,stk);if(label==~0u)return;switch(label){");
        for(u32 k=0;k<p.parts.size();++k)line(std::format("case {}u:xs_part{}(c,stk);break;",(virtual_code_base+p.parts[k].start-24)>>8,k));
        line(S("default:")+(contract?"xs_fault(1u,label,c.v17,c.v7);":"")+"return;}}}");return out.str();
    }
};
}
std::string ray_machine_source(const RtModule& m,const RtExport& e,unsigned words,std::string_view layout,std::optional<RayContract> contract){
    auto p=ray_program(m,e);return RayEmit(m,e,p,words,layout,contract).run();
}
RayLiveSource ray_live_source(const RtCollection& c,u32 words,RayContract contract,V input){
    const auto& m=c.modules.back();
    if(std::ranges::none_of(m.entries,[](auto& e){return e.name=="XDXR_TraceRay.ss_0";}))bad("unknown ray scheduler ABI");
    const RtExport* rg=nullptr;for(auto& e:m.exports)if(e.type==0 && !(e.flags&3)){if(rg)bad("ambiguous raygen export");rg=&e;}
    if(!rg)bad("missing original raygen");
    std::map<u32,std::pair<u32,S>> bound;
    while(!input.empty()){
        auto comma=input.find(',');auto row=input.substr(0,comma);
        auto a=row.find(':'),b=a==V::npos?V::npos:row.find(':',a+1);
        if(b==V::npos || row.find(':',b+1)!=V::npos)bad("invalid ray view tuple");
        auto reg=imm(row.substr(0,a)),dim=imm(row.substr(a+1,b-a-1));
        if(reg&~0x17fu || !bound.emplace(reg,std::pair{dim,S(row.substr(b+1))}).second)bad("duplicate or invalid ray binding");
        if(comma==V::npos)break;
        input.remove_prefix(comma+1);
    }
    auto program=ray_program(m,*rg);RayLiveSource result;S layout;unsigned as_count=0;
    for(auto [bind,use]:program.used){
        if(bind.space!=1)bad("different ray global register space");
        if(use.view==RayView::cb)continue;
        auto reg=bind.reg|(bind.type==1?0x100u:0u);auto it=bound.find(reg);if(it==bound.end())bad("unbound ray resource "+bind.name());
        u32 expected=use.view==RayView::accel?11u:use.view==RayView::tex3?8u:
            (use.view==RayView::tex2||use.view==RayView::rwtex2)?4u:1u;
        if(it->second.first!=expected)bad("wrong view dimension at "+bind.name());
        if(use.view==RayView::accel){result.bindings.push_back(view::ray_as|reg);++as_count;continue;}
        result.bindings.push_back(reg);if(!layout.empty())layout+=',';
        layout+=bind.name()+"="+it->second.second;
    }
    if(as_count!=1)bad("dispatch specialization needs one resolved TLAS");
    result.source=RayEmit(m,*rg,program,words,layout,contract).run();return result;
}
// Partial evaluation of a closest-hit export with one proved payload
// constant. Unknown pure values may die in the shader prologue; no unknown
// value may determine control, a memory effect or a returned payload word.
std::string ray_hit_specialization(const RtModule& m,const RtExport& e,u32 words,u32 fixed,u32 constant){
    if(e.type!=2 || fixed>=words || words>32)bad("invalid hit specialization");
    auto it=std::ranges::find(m.entries,e.symbols[0]+".ss_0",&RtEntry::name);
    if(it==m.entries.end())bad("missing closest hit entry");
    enum K {unknown,num,ptr,scratch,input,hit,ret};
    struct W{K k{};u32 n{};bool operator==(const W&)const=default;};
    std::map<S,W> regs{{"s32",{scratch}},{"v7",{num,0}},{"v9",{num,1}},{"v26",{ptr,0}},{"v6",{hit}},{"exec_lo",{num,~0u}}};
    std::map<u32,W> stack{{0,{ret}}},spills;
    for(u32 j=0;j<words;++j)stack[4+4*j]=j==fixed?W{num,constant}:W{input,j};
    auto initial=stack;u32 end=m.code.size();for(auto& q:m.entries)if(q.offset>it->offset)end=std::min(end,q.offset);
    auto val=[&](V v){if(gpr(v)||v=="exec_lo"||v=="scc"||v=="vcc_lo")return regs[S(v)];return W{num,imm(v)};};
    auto require=[&](W v){if(v.k!=num)bad("nonconstant closest-hit predicate");return v.n;};
    u32 pc=it->offset+24;std::set<u32> seen;
    for(;;){
        try{
            if(pc>=end || !seen.insert(pc).second)bad("nonterminating specialized hit path");
            auto i=isa::decode(m.code,pc,32);V op=i.op;auto& a=i.args;u32 next=pc+i.size;
            if(terminal(i)){if(regs["v8"].k!=ret || !require(regs["exec_lo"]))bad("unknown closest-hit return");break;}
            bool live=require(regs["exec_lo"])!=0;
            if(branch(i)){
                bool take;
                if(op=="s_branch")take=true;else if(op=="s_cbranch_execz")take=!live;
                else if(op=="s_cbranch_vccz")take=!require(regs["vcc_lo"]);
                else if(op=="s_cbranch_scc1")take=require(regs["scc"]);
                else if(op=="s_cbranch_scc0")take=!require(regs["scc"]);
                else bad("unknown specialized hit branch");
                pc=take?dest(m,i):next;continue;
            }
            pc=next;
            if(op.starts_with("s_waitcnt") || op=="s_nop" || op=="v_nop" || (!live && (op.starts_with("v_")||op.starts_with("buffer_")||op.starts_with("global_"))))continue;
            if(a.empty())bad("unknown closest-hit operation");
            auto d=a[0];auto x=[&](u32 j){return val(a.at(j));};
            auto put=[&](W w){if(d!="null")regs[d]=w;};
            if(op=="s_mov_b32"||op=="v_mov_b32"||op=="v_readfirstlane_b32")put(x(1));
            else if(op=="v_writelane_b32"||op=="v_readlane_b32")put({}); // lane spills are opaque unless dead
            else if(op=="s_and_saveexec_b32"){
                auto old=regs["exec_lo"];auto n=require(old)&require(x(1));put(old);regs["exec_lo"]={num,n};regs["scc"]={num,u32(n!=0)};
            }else if(op=="v_lshl_add_u32"){
                auto b=x(3),n=x(1),shift=x(2);put((b.k==ptr||b.k==num)&&n.k==num&&shift.k==num?W{b.k,b.n+(n.n<<(shift.n&31))}:W{});
            }else if(op=="buffer_load_dword"||op=="buffer_store_dword"){
                if(a.at(2)!="s[0:3]")bad("unmapped hit memory access");
                bool store=op=="buffer_store_dword";bool spill=a.at(1)=="off"&&x(3).k==scratch;
                auto mod=V(i.modifiers);u32 addr=0;
                if(!spill){auto b=x(1);
                    if(a.at(3)!="s4"||b.k!=ptr||!mod.starts_with("offen"))bad("unknown hit payload address");
                    addr=b.n;mod.remove_prefix(5);if(mod.starts_with(' '))mod.remove_prefix(1);}
                auto off=extra_offset(mod);if(off>UINT32_MAX-addr)bad("wrapped hit address");addr+=off;
                auto& mem=spill?spills:stack;
                if(addr%4 || (!spill && (addr>words*4 || (!addr && store))))bad("hit write outside call frame");
                if(store)mem[addr]=x(0);
                else put(mem.contains(addr)?mem.at(addr):W{});
            }else if(op=="v_cmp_eq_u32"||op=="v_cmp_ne_u32"){
                auto l=x(1),r=x(2);
                put(l.k==num && r.k==num?W{num,0u-u32((l.n==r.n)==(op=="v_cmp_eq_u32"))}:W{});
            }else if(op=="v_lshrrev_b32" || op=="v_and_b32" || op=="v_add_nc_u32" || op=="v_or_b32"){
                auto l=x(1),r=x(2);put(l.k==num && r.k==num?W{num,op=="v_lshrrev_b32"?r.n>>(l.n&31):op=="v_and_b32"?l.n&r.n:op=="v_or_b32"?l.n|r.n:l.n+r.n}:W{});
            }else if(op=="v_add_co_u32" || op=="v_add_co_ci_u32"){put({});regs[a.at(1)]={};}
            else if(op=="global_load_dwordx2"||op=="global_load_dword"||op=="s_lshr_b64"||op=="s_lshl_b32"||op=="s_load_dword"||op=="s_load_dwordx2"||op=="s_load_dwordx4"){
                if(!i.modifiers.empty() && !V(i.modifiers).starts_with("offset:"))bad("unmapped hit load modifiers");
                auto dst=rr(d);for(u32 k=0;k<dst.count;++k)regs[dst.at(k)]={};
                if(op=="s_lshr_b64"||op=="s_lshl_b32")regs["scc"]={};
            }else bad("unsupported live hit instruction "+S(op));
        }catch(const Error& ex){throw Unsupported(std::format("{} near +0x{:x}: {}",e.name,pc,ex.what()));}
    }
    if(!std::ranges::all_of(e.symbols[0],[](unsigned char c){return std::isalnum(c)||c=='_';}))bad("invalid closest hit symbol");
    S out=std::format("struct P{{uint w[{}];}};\n[shader(\"closesthit\")] void {}(inout P p,BuiltInTriangleIntersectionAttributes attr){{P old=p;\n",words,e.symbols[0]);
    for(u32 j=0;j<words;++j){auto w=stack.at(4+4*j);if(w==initial.at(4+4*j))continue;
        S expr;if(w.k==num)expr=std::format("{}u",w.n);else if(w.k==input)expr=std::format("old.w[{}]",w.n);else if(w.k==hit)expr="asuint(RayTCurrent())";
        else bad("unresolved closest-hit payload result");
        out+=std::format("p.w[{}]={};\n",j,expr);}
    return out+"}\n";
}
}
