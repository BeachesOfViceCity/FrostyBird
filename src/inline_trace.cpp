#include "inline_trace.hpp"
#include <algorithm>
#include <charconv>
#include <optional>
#include <string_view>

namespace frosty::isa {
namespace {
using S=std::string;using V=std::string_view;
using Seq=std::vector<const Instruction*>;
bool reg(V v){return v.size()>1 && (v[0]=='s'||v[0]=='v') && v.find_first_not_of("0123456789",1)==v.npos;}
std::optional<unsigned> dec(V s){
    unsigned n{};auto [p,ec]=std::from_chars(s.data(),s.data()+s.size(),n);if(ec!=std::errc{}||p!=s.data()+s.size())return {};return n;
}
std::vector<S> regs(V s){
    if(reg(s) || s=="vcc_lo" || s=="exec_lo" || s=="vcc")return {S(s)};
    std::vector<S> r;
    if(s.size()>2 && s.front()=='[' && s.back()==']'){
        s=s.substr(1,s.size()-2);while(!s.empty()){auto k=s.find(',');auto e=s.substr(0,k);
            while(e.starts_with(' '))e.remove_prefix(1);
            if(!reg(e))return {};
            r.emplace_back(e);
            if(k==s.npos)break;
            s.remove_prefix(k+1);}
    }else if(s.size()>=6 && s[1]=='[' && s.back()==']'){
        char bank=s[0];auto colon=s.find(':');if(colon==s.npos)return {};
        auto lo=dec(s.substr(2,colon-2)),hi=dec(s.substr(colon+1,s.size()-colon-2));
        if(!lo||!hi||*lo>*hi||*hi>255)return {};
        for(auto i=*lo;i<=*hi;++i)r.push_back(S(1,bank)+std::to_string(i));
    }
    return r;
}
bool trivia(const Instruction& i){return i.op=="s_nop"||i.op=="s_waitcnt";}
constexpr unsigned tlas=0x10000,blas=0x20000,instance=tlas+64,triangle=blas+64;
std::optional<HitWord> hit_word(unsigned a){
    if(a%4)return {};
    using H=HitWord;
    if(a>=instance && a<instance+48)return H{H::transform,(a-instance)/4};
    if(a==instance+48)return H{H::instance_id};
    if(a==instance+52)return H{H::sbt_offset};
    if(a==instance+60)return H{H::literal,blas>>8};
    if(a==instance+112)return H{H::instance_index};
    if(a==triangle+48)return H{H::geometry_word};
    if(a==triangle+60)return H{H::literal,9}; // slot zero, ordinary A/B/C winding
    if(a>=triangle && a<triangle+36)return H{H::vertex,(a-triangle)/4};
    // Uncompressed BLAS: no per-axis exponent or bounding-box bias.
    if(a==blas+60 || (a>=blas+16 && a<=blas+36))return H{H::literal,0};
    return {};
}
// Follow the compiler's pointer arithmetic for a committed instance and
// slot-zero triangle. Only accept a global load if *every* addressed dword
// resolves to a known field. Unknown/runtime data do not become constants.
bool hit_reads(const Graph& g,InlineTrace& t){
    using W=std::optional<u32>;
    S cond,saved;auto it=g.ins.lower_bound(t.end);
    for(;it!=g.ins.end() && it->first<t.end+160;++it){
        const auto& i=it->second;const auto& a=i.args;
        if(i.op=="v_cmp_ne_u32" && a[1]=="0" && a[2]==t.committed)cond=a[0];
        if(i.op=="s_and_saveexec_b32" && !cond.empty() && a[1]==cond)saved=a[0];
        if(i.op=="s_cbranch_execz"){
            if(saved.empty())return false;
            auto n=dec(a[0]);if(!n||*n>1024)return false;
            t.post_begin=i.pc+i.size;t.post_end=t.post_begin+*n*4;break;
        }
    }
    if(!t.post_begin || !g.ins.contains(t.post_end))return false;
    const auto& join=g.ins.at(t.post_end);
    if(join.op!="s_mov_b32" || join.args!=std::vector<S>{"exec_lo",saved})return false;
    // First carry pair adds (instance token >> 3) << 6 to the TLAS.
    for(auto j=g.ins.lower_bound(t.post_begin);j->first<t.post_end;++j){
        const auto& i=j->second;const auto& a=i.args;
        if(i.op.starts_with("global_load"))break;
        if(i.op=="v_add_co_u32" && t.base_lo.empty()){
            for(auto k:{2,3})if(a[k].starts_with('s'))t.base_lo=a[k];
        }
        if(i.op=="v_add_co_ci_u32" && t.base_hi.empty()){
            for(auto k:{2,3})if(a[k].starts_with('s'))t.base_hi=a[k];
        }
    }
    if(t.base_lo.empty() || t.base_hi.empty())return false;
    using Words=std::map<S,W>;
    Words words{{t.base_lo,tlas},{t.base_hi,0},{t.committed,14},{t.leaf,8},{"exec_lo",~0u}};
    std::map<unsigned,Words> incoming;
    auto meet=[](Words& a,const Words& b){
        for(auto& [k,v]:a)if(auto f=b.find(k);f==b.end() || v!=f->second)v.reset();
    };
    bool live=true;
    auto val=[&](const S& s)->W{
        if(auto f=words.find(s);f!=words.end())return f->second;
        V v=s;bool neg=v.starts_with('-');if(neg)v.remove_prefix(1);
        int base=v.starts_with("0x")?16:10;if(base==16)v.remove_prefix(2);
        u32 n;auto [p,ec]=std::from_chars(v.data(),v.data()+v.size(),n,base);
        return ec==std::errc{} && p==v.data()+v.size()?W(neg?0u-n:n):W{};
    };
    for(auto j=g.ins.lower_bound(t.post_begin);j->first<t.post_end;++j){
        const auto& i=j->second;const auto& a=i.args;auto op=V(i.op);
        if(auto f=incoming.find(i.pc);f!=incoming.end()){
            if(live)meet(words,f->second);else words=std::move(f->second);
            live=true;
        }
        if(!live)continue;
        if(a.empty() || trivia(i))continue;
        auto exec=val("exec_lo");
        if(op=="s_cbranch_execz"){
            auto n=dec(a[0]);if(!n || *n>1024)return false;
            unsigned fall=i.pc+i.size,to=fall+4*(*n);if(to>t.post_end || !g.ins.contains(to))return false;
            if(!exec || !*exec){
                auto [dest,new_edge]=incoming.try_emplace(to,words);if(!new_edge)meet(dest->second,words);
            }
            if(exec){t.post_edges[i.pc]=*exec?fall:to;live=*exec!=0;}
            continue;
        }
        if(op=="s_branch" || op.starts_with("s_cbranch"))return false;
        auto dst=regs(a[0]);if(dst.empty())continue;
        W v;auto x=a.size()>1?val(a[1]):W{},y=a.size()>2?val(a[2]):W{},z=a.size()>3?val(a[3]):W{};
        if(op=="s_and_saveexec_b32"){
            words[a[0]]=exec;words["exec_lo"]=exec&&x?W(*exec & *x):W{};continue;
        }
        if(op.starts_with("global_load_dword")){
            auto rr=regs(a.at(1));if(a.size()!=3 || a[2]!="off" || rr.size()!=2)return false;
            unsigned off=0;
            if(!i.modifiers.empty()){
                V m=i.modifiers;if(!m.starts_with("offset:"))return false;
                auto n=dec(m.substr(7));if(!n)return false;off=*n;
            }
            auto lo=val(rr[0]),hi=val(rr[1]);if(!lo || hi!=W(0))return false;
            for(unsigned k=0;k<dst.size();++k){
                auto w=hit_word(*lo+off+4*k);if(!w)return false;
                t.reads[i.pc].push_back(*w);words[dst[k]]=w->field==HitWord::literal?W(w->n):W{};
            }
            continue;
        }
        if(op=="v_lshlrev_b64"){
            auto src=regs(a[2]);if(src.size()!=2 || dst.size()!=2)return false;
            auto l=val(src[0]),h=val(src[1]);std::optional<u64> wide;
            if(l && h && x)wide=((u64(*h)<<32)|*l)<<(*x&63);
            words[dst[0]]=wide?W(u32(*wide)):W{};words[dst[1]]=wide?W(*wide>>32):W{};continue;
        }
        if(op=="v_add_co_u32" || op=="v_add_co_ci_u32"){
            W c=op=="v_add_co_u32"?W(0):val(a.at(4));std::optional<u64> sum;
            if(y&&z&&c)sum=u64(*y)+*z+(*c!=0);
            words[a[1]]=sum?W(0u-u32((*sum>>32)!=0)):W{};v=sum?W(u32(*sum)):W{};
        }else if(op=="v_mov_b32" || op=="s_mov_b32" || op=="s_movk_i32")v=x;
        else if(x && y){
            if(op=="v_and_b32" || op=="s_and_b32")v=*x&*y;
            else if(op=="v_or_b32" || op=="s_or_b32")v=*x|*y;
            else if(op=="s_xor_b32")v=*x^*y;
            else if(exec && op.starts_with("v_cmp_")){
                if(op=="v_cmp_ne_u32")v=*exec & (0u-u32(*x!=*y));
                if(op=="v_cmp_eq_u32")v=*exec & (0u-u32(*x==*y));
                if(op=="v_cmp_gt_u32")v=*exec & (0u-u32(*x>*y));
                if(op=="v_cmp_lt_u32")v=*exec & (0u-u32(*x<*y));
                if(op=="v_cmp_gt_i32")v=*exec & (0u-u32(std::int32_t(*x)>std::int32_t(*y)));
            }
            else if(op=="v_lshlrev_b32")v=*y<<(*x&31);
            else if(op=="v_lshrrev_b32")v=*y>>(*x&31);
            else if(op=="v_add_nc_u32")v=*x+*y;
            else if(op=="v_mul_u32_u24")v=(*x&0xffffff)*(*y&0xffffff);
            else if(op=="v_or3_b32" && z)v=*x|*y|*z;
            else if(op=="v_bfe_u32" && z)v=(*x>>(*y&31))&((1u<<(*z&31))-1);
        }
        for(auto& d:dst)words[d]=v;
    }
    return !t.reads.empty();
}
// A single-entry, single-exit forward-only body and one scalar back edge.
// Reject stores other than the private traversal stack, calls, and other
// resource accesses. Unmodelled outputs are explicitly killed by the lowerer.
std::optional<InlineTrace> contract(const Program& p,const Graph& g,const Instruction& bvh){
    if(bvh.args.size()!=3||!bvh.modifiers.empty())return {};
    auto addr=regs(bvh.args[1]),out=regs(bvh.args[0]);if(addr.size()!=12||out.size()!=4)return {};
    InlineTrace t;
    for(auto it=g.ins.upper_bound(bvh.pc);it!=g.ins.end();++it){
        auto& i=it->second;if(i.op!="s_cbranch_scc1")continue;
        int disp=std::bit_cast<std::int16_t>(std::uint16_t(p.code.at<u32>(i.pc)));
        auto to=std::int64_t(i.pc+i.size)+4ll*disp;
        if(to>=0 && to<=bvh.pc && disp<0){t.begin=unsigned(to);t.end=i.pc+i.size;break;}
    }
    if(!t.end || t.end-t.begin>4096)return {};
    std::set<unsigned> in,outgoing;
    auto inside=[&](unsigned pc){return t.begin<=pc && pc<t.end;};
    for(auto& [h,edges]:g.edges)for(auto x:edges){
        if(!inside(h)&&inside(x))in.insert(x);
        else if(inside(h)&&!inside(x))outgoing.insert(x);
    }
    if(in.size()!=1 || outgoing!=std::set<unsigned>{t.end})return {};
    t.entry=*in.begin();t.tmax=addr[2];
    Seq seq;unsigned b=0,bvhs=0,exchanges=0;
    for(auto it=g.ins.lower_bound(t.begin);it!=g.ins.end() && it->first<t.end;++it){
        auto& i=it->second;auto op=V(i.op);
        if(i.pc==bvh.pc)b=seq.size();
        if(op=="image_bvh64_intersect_ray")++bvhs;
        else if(op=="ds_wrxchg_rtn_b32")++exchanges;
        else if(op.starts_with("global_load_dword") || op=="ds_write_b32" || trivia(i) ||
                op=="s_branch" || op.starts_with("s_cbranch_")){}
        else if(op.starts_with("v_") || op.starts_with("s_")){
            if(op.starts_with("s_load") || op.starts_with("s_buffer_") || op.starts_with("s_store") || op.starts_with("s_send") ||
               op.starts_with("s_set") || op.starts_with("s_get") || op.starts_with("s_call") || op.starts_with("s_swap") ||
               op=="s_barrier" || op=="s_endpgm" || op=="s_trap" || op.starts_with("v_movrel") || op=="v_writelane_b32")return {};
        }else return {};
        if(!i.args.empty() && !trivia(i) && !op.starts_with("s_cbranch") && op!="s_branch" && op!="ds_write_b32"){
            for(auto& n:regs(i.args[0]))t.clobbers.insert(n);
            if(op.find("_co_")!=op.npos && i.args.size()>1)for(auto& n:regs(i.args[1]))t.clobbers.insert(n);
        }
        if(!trivia(i))seq.push_back(&i);
    }
    if(bvhs!=1 || exchanges!=1 || b>=seq.size() || seq[b]->pc!=bvh.pc)return {};
    // Recover the world-space ray restored when the walker leaves an instance.
    for(unsigned k=0;k<6;++k){
        std::set<S> candidates;for(unsigned j=0;j<b;++j){auto& i=*seq[j];auto& a=i.args;
            if(i.op=="v_cndmask_b32" && a.size()==4 && a[0]==addr[3+k] && a[1]==a[0])candidates.insert(a[2]);}
        if(candidates.size()!=1)return {};
        t.world[k]=*candidates.begin();
    }
    // BVH64 takes an absolute node index. The high-level AS comes from the
    // page pointer used in (page << 5) + node, not from a chosen t-register.
    for(unsigned j=b>10?b-10:0;j<b;++j){auto& i=*seq[j];auto& a=i.args;
        if(i.op=="v_lshlrev_b64" && a.size()==3 && a[1]=="5"){
            auto pg=regs(a[2]);if(pg.size()!=2)return {};t.as_page=pg.front();}}
    if(t.as_page.empty())return {};
    S instance_word,flag,shifted;unsigned bit=0;
    bool bound_mask=false,face_mode=false;S recip,dist,hit_guard,min_guard,max_guard,face_guard,face_result,walk_status,instance_flags,flag_pair,xor_pair;
    for(unsigned j=0;j<b;++j){auto& i=*seq[j];auto& a=i.args;
        if(i.op=="v_cmp_eq_u32" && a[0]=="vcc_lo" && a[1]=="3")walk_status=a[2];}
    for(unsigned j=b+1;j<seq.size();++j){
        auto& i=*seq[j];auto& a=i.args;
        if(i.op=="global_load_dwordx4" && instance_word.empty()){
            auto d=regs(a.at(0));if(d.size()!=4)return {};instance_word=d.front();instance_flags=d[1];
        }
        if(i.op=="v_cmp_ne_u32" && a.size()==3 && a[1]=="7")flag=a[0];
        if(!flag.empty() && i.op=="v_cndmask_b32" && a[1]=="0" && a[2]=="1" && a[3]==flag)flag=a[0];
        if(i.op=="v_lshrrev_b32" && a[2]==instance_word){
            if(auto s=dec(a[1]);s && *s>=24 && *s<=31){shifted=a[0];bit=*s-24;}
        }
        if(!bound_mask && !flag.empty() && ((i.op=="v_and_b32_sdwa" && a[1]==instance_word && a[2]==flag &&
             i.modifiers=="dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:DWORD") ||
             (i.op=="v_and_b32" && ((a[1]==shifted && a[2]==flag)||(a[2]==shifted && a[1]==flag))))){
            // The AND must control the instance predicate, not merely some
            // unrelated flag computed from its descriptor.
            for(unsigned z=j+1;z<std::min<unsigned>(j+5,seq.size());++z){auto& test=*seq[z];
                if(test.op=="v_cmp_ne_u32" && test.args[1]=="0" && test.args[2]==a[0]){
                    t.mask=1u<<(i.op=="v_and_b32_sdwa"?0:bit);bound_mask=true;break;}}
        }
        if(i.op=="v_bfe_u32" && a[1]==instance_flags && a[2]=="24" && a[3]=="2")flag_pair=a[0];
        if(i.op=="v_xor_b32" && a[1]=="1" && a[2]==flag_pair)xor_pair=a[0];
        if(i.op=="v_mul_i32_i24" && a[0]==a[2] && (a[1]=="0" || (a[2]==xor_pair && (a[1]=="1" || a[1]=="-1"))) && j+3<seq.size()){
            auto& sh=*seq[j+1];auto& pack=*seq[j+2];auto& upd=*seq[j+3];
            face_mode=sh.op=="v_lshlrev_b32" && sh.args[0]==a[0] && sh.args[1]=="2" && sh.args[2]==a[0] &&
                 pack.op=="v_and_or_b32" && pack.args[1]==a[0] && pack.args[2]=="12" && pack.args[3]=="16" &&
                 upd.op=="v_bfi_b32" && upd.args[1]=="60" && upd.args[2]==pack.args[0];
            if(face_mode)t.facing=a[1]=="0"?0:a[1]=="1"?1:-1;
        }
        if(i.op=="v_rcp_f32" && a[1]==out[1])recip=a[0];
        if(i.op=="v_mul_legacy_f32" && ((a[1]==recip&&a[2]==out[0])||(a[2]==recip&&a[1]==out[0])))dist=a[0];
        if(i.op=="v_mul_i32_i24_sdwa" && a[1]=="sext("+out[1]+")" && i.modifiers.find("src0_sel:WORD_1")!=S::npos)face_result=a[0];
        if(i.op=="v_cmp_lt_i32" && a[1]=="-1" && a[2]==face_result)face_guard=a[0];
        if(!dist.empty()){
            if(i.op=="v_cmp_nlt_f32" && a[1]==dist){min_guard=a[0];t.tmin=a[2];}
            if(i.op=="v_cmp_ngt_f32" && a[2]==dist){min_guard=a[0];t.tmin=a[1];}
            if(i.op=="v_cmp_nge_f32" && a[1]==dist && a[2]==t.tmax)max_guard=a[0];
        }
        if(i.op=="s_and_b32" && !min_guard.empty() && !max_guard.empty()){
            if((a[1]==min_guard&&a[2]==max_guard)||(a[2]==min_guard&&a[1]==max_guard))hit_guard=a[0];
            else if(!hit_guard.empty() && ((a[1]==face_guard&&a[2]==hit_guard)||(a[2]==face_guard&&a[1]==hit_guard)))hit_guard=a[0];
        }
        if(i.op=="s_and_saveexec_b32" && !hit_guard.empty() && !face_guard.empty() && a[1]==hit_guard && j+4<seq.size()){
            // Accept-first-hit: write the committed instance, time, and the
            // finished state. Closest-hit/material walks have more outputs.
            auto& obj=*seq[j+1];auto& time=*seq[j+2];auto& done=*seq[j+3];auto& restore=*seq[j+4];
            if(obj.op=="v_mov_b32" && reg(obj.args[1]) && time.op=="v_mov_b32" && time.args[0]==t.tmax && time.args[1]==dist &&
               done.op=="v_mov_b32" && done.args[1]=="0" && restore.op=="s_mov_b32" && restore.args[0]=="exec_lo"){
                 t.committed=obj.args[0];t.status=done.args[0];}
            // Closest triangle: retain the best T, leaf node and instance;
            // keep walking with the tightened upper bound. Some allocations
            // use the same register for the returned T and the upper bound.
            if(obj.op=="v_mov_b32" && obj.args[1]==dist && time.op=="v_mov_b32" && reg(time.args[1]) &&
               done.op=="v_mov_b32" && reg(done.args[1])){
                bool shared=obj.args[0]==t.tmax;unsigned z=j+4;
                if(!shared && restore.op=="v_mov_b32" && restore.args[0]==t.tmax && restore.args[1]==dist)++z;
                else if(!shared)continue;
                if(z<seq.size() && seq[z]->op=="s_mov_b32" && seq[z]->args[0]=="exec_lo"){
                    t.hit_t=obj.args[0];t.leaf=time.args[0];t.committed=done.args[0];t.status=walk_status;
                }
            }
        }
    }
    if(!bound_mask || !face_mode || t.committed.empty() || t.tmin.empty() || seq.size()<6)return {};
    auto& done=*seq[seq.size()-6];auto& budget=*seq[seq.size()-5];auto& both=*seq[seq.size()-4];
    if(!t.leaf.empty()){
        if(done.op!="v_cmp_ge_u32" || budget.op!="s_xor_b32" || budget.args[1]!="vcc_lo" || budget.args[2]!="-1" ||
           both.op!="s_or_b32" || both.args[1]!=done.args[0] || both.args[2]!=budget.args[0] || !hit_reads(g,t))return {};
        t.limit=done.args[2];return t;
    }
    if(done.op!="v_cmp_ne_u32" || done.args[1]!="3" || done.args[2]!=t.status || budget.op!="v_cmp_ge_u32" ||
       both.op!="s_or_b32" || both.args[1]!=budget.args[0] || both.args[2]!=done.args[0])return {};
    t.limit=budget.args[2];
    return t;
}
}
std::map<unsigned,InlineTrace> inline_visibility(const Program& p,const Graph& g){
    std::map<unsigned,InlineTrace> plans;
    if(p.stage!=compute || p.wave!=32 || std::ranges::none_of(p.resources,[](auto& r){return r.kind==16 && r.type==4;}))return plans;
    for(auto& [pc,i]:g.ins)if(i.op=="image_bvh64_intersect_ray")
        if(auto t=contract(p,g,i))plans.emplace(t->entry,std::move(*t));
    return plans;
}
}
