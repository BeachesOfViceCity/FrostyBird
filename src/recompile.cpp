#include "isa.hpp"
#include "mask_edges.hpp"
#include "active_regions.hpp"
#include "bit_transform.hpp"
#include "float_compare.hpp"
#include "relative_windows.hpp"
#include "inline_trace.hpp"
#include "ray_fetch.hpp"
#include "view_fields.hpp"
#include "soft64.hpp"
#include <algorithm>
#include <bit>
#include <bitset>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <format>
#include <set>
#include <sstream>

namespace frosty::isa {
namespace {
// Host-only lowering. Metadata denotes the stage ABI; an Instruction denotes
// the work to perform. No archive key, filename or game entry point enters
// this pass. All generated words retain the guest register bit pattern.
using Str = std::string;
using View = std::string_view;
constexpr View xyzw="xyzw";
[[noreturn]] void unsupported(View why) { throw Unsupported(Str(why)); }

unsigned number(View v) {
    unsigned n{};
    int base=v.starts_with("0x")?16:10;
    if (base==16) v.remove_prefix(2);
    auto [end,ec]=std::from_chars(v.data(),v.data()+v.size(),n,base);
    if (ec!=std::errc{} || end!=v.data()+v.size()) unsupported("non-immediate operand");
    return n;
}
struct Range { char bank; unsigned first, count; };
Range registers(View v) {
    if (v.size()<2 || (v[0]!='s' && v[0]!='v')) unsupported("expected register");
    char bank=v[0]; v.remove_prefix(1);
    unsigned lo,hi;
    if (v.front()=='[' && v.back()==']' && v.contains(':')) {
        v=v.substr(1,v.size()-2); auto mid=v.find(':');
        lo=number(v.substr(0,mid)); hi=number(v.substr(mid+1));
    } else lo=hi=number(v);
    if (lo>hi || hi>=(bank=='s'?106u:256u)) unsupported("invalid register range");
    return {bank,lo,hi-lo+1};
}
Str reg(char bank,unsigned n) { return std::format("{}{}",bank,n); }
#include "lds_alignment.inl"
bool vgpr(View r){return r.size()>1 && r[0]=='v' && std::ranges::all_of(r.substr(1),[](char c){return c>='0'&&c<='9';});}
// LLVM operands include scalar names, ranges and NSA lists (e.g. [v1,v4]).
// Used only to prove a spill cell unobserved, never to invent its contents.
bool mentions_word(View operand,char bank,unsigned index){
    for(size_t pos=0;(pos=operand.find(bank,pos))!=View::npos;){
        auto s=operand.substr(++pos);bool range=s.starts_with('[');
        if(range)s.remove_prefix(1);
        unsigned lo{},hi{};auto [last,ec]=std::from_chars(s.data(),s.data()+s.size(),lo);
        if(ec!=std::errc{})continue;
        hi=lo;
        if(range){
            if(last==s.end() || *last!=':')return true;
            auto [end,err]=std::from_chars(last+1,s.data()+s.size(),hi);
            if(err!=std::errc{} || end==s.end() || *end!=']')return true;
        }
        if(lo<=index && index<=hi)return true;
    }
    return false;
}
// WRITELANE/READLANE are also used to spill scalar register pairs. One
// half may never have been initialized. Suppress a cell only if the VGPR
// has *no* general or dynamic read, and every restore of that lane dies
// before an arbitrary branch or read. Other lanes of the pair stay live.
std::set<unsigned> dead_spill_transfers(const Graph& g,unsigned wave){
    using Cell=std::pair<unsigned,unsigned>;
    std::map<Cell,std::vector<unsigned>> candidates;std::set<unsigned> banks;
    for(const auto& [pc,i]:g.ins){
        if(i.op.starts_with("v_movrel") || i.op.starts_with("v_swaprel"))return {};
        if(i.op=="v_writelane_b32" && i.args.size()==3 && vgpr(i.args[0]))banks.insert(registers(i.args[0]).first);
    }
    std::set<Cell> live;
    for(auto vg:banks)for(auto it=g.ins.begin();it!=g.ins.end();++it){
        const auto& [pc,i]=*it;bool load=i.op=="v_readlane_b32",store=i.op=="v_writelane_b32";
        bool fixed=(load || store) && i.modifiers.empty() && i.args.size()==3 && i.args[load?1:0]==reg('v',vg) &&
            !i.args[2].empty() && std::isdigit(static_cast<unsigned char>(i.args[2][0]));
        // If the same VGPR occurs anywhere except the spill operand, it can
        // expose the whole wave, including this lane.
        bool general=false;
        for(size_t k=0;k<i.args.size();++k)if(!(fixed && k==unsigned(load?1:0)))general|=mentions_word(i.args[k],'v',vg);
        if(general){for(unsigned lane=0;lane<wave;++lane)live.emplace(vg,lane);break;}
        if(!fixed)continue;
        Cell cell{vg,number(i.args[2])&(wave-1)};
        candidates[cell].push_back(pc);
        if(!load)continue;
        auto& dst=i.args[0];bool dead=false;
        if(dst.size()>1 && dst[0]=='s' && std::isdigit(static_cast<unsigned char>(dst[1]))){
            auto sg=registers(dst).first;
            for(auto k=std::next(it);k!=g.ins.end();++k){
                const auto& n=k->second;
                if(n.op=="s_endpgm"){dead=true;break;}
                if(n.op=="s_branch" || n.op.starts_with("s_cbranch") || n.op.contains("pc_") || n.op=="s_trap")break;
                if(n.op=="s_mov_b32" && n.args.size()==2 && n.args[0]==dst && !mentions_word(n.args[1],'s',sg)){dead=true;break;}
                if(std::ranges::any_of(n.args,[&](auto& a){return mentions_word(a,'s',sg);}))break;
            }
        }
        if(!dead)live.insert(cell);
    }
    std::set<unsigned> result;
    for(const auto& [cell,pcs]:candidates)if(!live.contains(cell))result.insert(pcs.begin(),pcs.end());
    return result;
}
bool needs_quad_lod(View op){return op=="image_sample" || op=="image_sample_o" || op=="image_sample_c" ||
    op.starts_with("image_sample_b") || op.starts_with("image_gather");}
// P1 is a symbolic interpolation intermediate, not a numeric pixel input.
// Where P2 completes it under the very same EXEC, this pair is a single
// predicated assignment as far as the physical destination is concerned.
// Prove that the partial value cannot escape (including register ranges),
// no branch enters the pair, and no instruction changes its participation.
std::map<unsigned,unsigned> paired_quad_interpolants(const Graph& graph,const std::set<unsigned>& targets,const std::set<Str>& coordinates){
    std::map<unsigned,unsigned> result;
    for(auto it=graph.ins.begin();it!=graph.ins.end();++it){
        const auto& first=it->second;
        if(first.op!="v_interp_p1_f32" || first.args.size()!=3 || !first.modifiers.empty() ||
           !coordinates.contains(first.args[0]) || !vgpr(first.args[0]))continue;
        auto word=registers(first.args[0]).first;
        for(auto next=std::next(it);next!=graph.ins.end() && !targets.contains(next->first);++next){
            const auto& i=next->second;auto op=View(i.op);
            if(op=="v_interp_p2_f32" && i.args.size()==3 && i.modifiers.empty() &&
               i.args[0]==first.args[0] && i.args[2]==first.args[2] && i.args[1]!=i.args[0]){
                result.emplace(first.pc,i.pc);break;
            }
            bool scalar=op.starts_with("s_buffer_load") || op.starts_with("s_load") || op.starts_with("s_waitcnt") ||
                        op=="s_clause" || op=="s_nop" || op=="s_inst_prefetch";
            bool vector=op.starts_with("v_") && !op.starts_with("v_cmpx") && !op.contains("rel") && !op.starts_with("v_swap");
            if((!scalar && !vector) || std::ranges::any_of(i.args,[&](const Str& a){
                return a.starts_with("exec") || a=="m0" || mentions_word(a,'v',word);
            }))break;
        }
    }
    return result;
}
unsigned lane_count(View op, View base) {
    if (op==base) return 1;
    if (!op.starts_with(Str(base)+"x")) unsupported("unknown memory operation");
    unsigned n=number(op.substr(base.size()+1));
    if (n!=2 && n!=3 && n!=4 && n!=8 && n!=16) unsupported("unsupported transfer width");
    return n;
}
Str suffix(unsigned n) {return n==1?"":std::to_string(n);}
Str type(const Semantic& s) {
    auto norm=s.mask>>std::min(31,std::countr_zero(s.mask));
    if (s.type<1 || s.type>3 || !s.mask || s.mask>15 || (norm&(norm+1)))
        unsupported("packed or extended signature type");
    if(s.system==9 && norm==1)return "bool";
    return Str(s.type==3?"float":s.type==2?"int":"uint")+suffix(std::popcount(s.mask));
}
Str field(const Semantic& s) {
    if(s.reg==~0u)return std::format("sys{}_{}",s.system,s.index);
    auto col=std::countr_zero(s.mask);
    return reg('r',s.reg)+(col?"_c"+std::to_string(col):"");
}
Str component(unsigned mask,unsigned c) {
    if (c>=4 || !(mask&(1u<<c))) unsupported("missing signature component");
    return std::has_single_bit(mask)?"":Str(".")+xyzw[c-std::countr_zero(mask)];
}
Str as_output(const Semantic& s,const Str& bits) {
    return s.type==3?"asfloat("+bits+")":s.type==2?"asint("+bits+")":bits;
}

// SGPRs can hold numeric data, descriptor addresses, SRD words, or a lane
// predicate. Keep their provenance distinct: integers are never guessed to
// be bindings. Registers in the HLSL are ordinary per-invocation variables;
// DXC promotes these to SSA and inserts phis at control-flow joins.
enum class Kind { unknown, number, masked_number, table, descriptor, predicate, bary, partial, prim,
    gs_indices, gs_in, gs_wave, gs_out, gs_param, gs_lane, ancillary, front_face, code_lo, code_hi, mask_high, undef_word, immediate,
    ray_address,ray_node,opaque_word,bounded_relative };
struct Value {
    Kind kind{};
    unsigned id{}, part{};
    bool uniform{};
    unsigned raw_set{};
    unsigned mask_origin{};
    bool operator==(const Value&) const = default;
};
constexpr Value scalar{Kind::number,0,0,true}, varying{Kind::number};
using State=std::map<Str,Value>;
using Names=std::set<Str>;
struct Block {
    unsigned end; State entry; bool masked; std::vector<Names> definitions;
    unsigned guard{},side{},epoch{};
};
struct LoopUse { Names defined,live_in,zero_high; };
// Strongly connected components, in execution order. Use resolved edges
// (including cloned mask blocks), not adjacency inferred from byte offsets.
std::vector<std::vector<unsigned>> components(const std::map<unsigned,std::vector<unsigned>>& edges,unsigned start){
    std::map<unsigned,unsigned> index,low;std::set<unsigned> on;std::vector<unsigned> stack;
    std::vector<std::vector<unsigned>> out;
    auto visit=[&](auto&& self,unsigned v)->void{
        low[v]=index[v]=unsigned(index.size());stack.push_back(v);on.insert(v);
        for(auto w:edges.at(v)){
            if(!index.contains(w)){self(self,w);low[v]=std::min(low[v],low[w]);}
            else if(on.contains(w))low[v]=std::min(low[v],index[w]);
        }
        if(low[v]!=index[v])return;
        auto& c=out.emplace_back();
        for(;;){auto w=stack.back();stack.pop_back();on.erase(w);c.push_back(w);if(w==v)break;}
        std::ranges::sort(c);
    };
    visit(visit,start);std::ranges::reverse(out);return out;
}
#include "structured_flow.inl"
#include "output_progress.inl"
// A device queue may require the reference one-block-at-a-time scheduler,
// but looking up a sparse PC need not walk every earlier case in order.
// Every edge remains the original PC and each iteration still runs one
// block, including all of its votes, device waits and group barriers.
void emit_pc_tree(std::ostream& out,const std::map<unsigned,Str>& cases){
    using It=decltype(cases.begin());
    auto emit=[&](auto&& self,It first,It end,size_t n)->void{
        if(n<=8){
            out<<"    switch(wave_pc){\n";
            for(auto it=first;it!=end;++it)out<<"    case "<<it->first<<"u:{\n"<<it->second<<"    }\n";
            out<<"    default:return;\n    }\n";return;
        }
        auto mid=std::next(first,n/2);
        out<<"    [branch] if(wave_pc<"<<mid->first<<"u){\n";self(self,first,mid,n/2);
        out<<"    }else{\n";self(self,mid,end,n-n/2);out<<"    }\n";
    };
    out<<"    [loop] for(;;){uint wave_pc=WaveReadLaneFirst(pc);\n";
    emit(emit,cases.begin(),cases.end(),cases.size());out<<"    }\n}\n";
}
void emit_flow_regions(std::ostream& o,const std::map<unsigned,Str>& code,
        const std::map<unsigned,std::vector<unsigned>>& jumps,const std::map<unsigned,Str>& predicates,unsigned start,bool natural=false){
    struct Region {Str body,branch;std::vector<unsigned> out;};
    std::map<unsigned,Region> regions;
    for(auto& [h,text]:code){
        auto& node=regions[h];node.out=jumps.at(h);auto n=text.size();
        if(!node.out.empty()){
            auto at=text.rfind('\n',n-2);n=at==Str::npos?0:at+1;
            if(!View(text).substr(n).starts_with("    pc"))unsupported("unrecognized CFG terminator");
        }
        node.body="{\n"+text.substr(0,n)+"    }\n";
        if(node.out.size()==2){if(node.out[0]==node.out[1])node.out.pop_back();else node.branch=predicates.at(h);}
    }
    // Reduce only regions with a single entry. Both edges of every diamond
    // keep their actual wave vote. Merging excludes the dispatch entry or a
    // block with another predecessor, even if its code looks identical.
    while(true){
        std::map<unsigned,unsigned> indegree;
        for(auto& [_,r]:regions)for(auto v:r.out)++indegree[v];
        bool changed=false;
        for(auto& [h,r]:regions){
            auto private_node=[&](unsigned v){return v!=h && v!=start && indegree[v]==1;};
            if(r.out.size()==1 && private_node(r.out[0])){
                auto v=r.out[0];auto tail=std::move(regions.at(v));regions.erase(v);
                r.body+=tail.body;r.out=std::move(tail.out);r.branch=std::move(tail.branch);changed=true;break;
            }
            if(r.out.size()!=2)continue;
            auto a=r.out[0],b=r.out[1];auto& yes=regions.at(a);auto& no=regions.at(b);
            if(private_node(a) && private_node(b) && yes.out==no.out && yes.out.size()<=1){
                r.body+="    if("+r.branch+"){\n"+yes.body+"    }else{\n"+no.body+"    }\n";
                r.out=yes.out;regions.erase(a);regions.erase(b);
            }else if(private_node(a) && yes.out.size()==1 && yes.out[0]==b){
                r.body+="    if("+r.branch+"){\n"+yes.body+"    }\n";r.out={b};regions.erase(a);
            }else if(private_node(b) && no.out.size()==1 && no.out[0]==a){
                r.body+="    if(!("+r.branch+")){\n"+no.body+"    }\n";r.out={a};regions.erase(b);
            }else continue;
            r.branch.clear();changed=true;break;
        }
        if(!changed)break;
    }
    if(natural){
        size_t cap=32768;unsigned fresh=regions.rbegin()->first+1;
        for(auto& [h,r]:regions)cap+=r.body.size()*2+256;
        if(auto text=structured_flow(regions,start,fresh,cap)){o<<*text;return;}
    }
    std::map<unsigned,std::vector<unsigned>> edges;for(auto& [h,r]:regions)edges.emplace(h,r.out);
    for(auto& c:components(edges,start)){
        auto h=c[0];auto& first=regions.at(h);
        bool self=c.size()==1 && std::ranges::find(first.out,h)!=first.out.end();
        if(self && first.out.size()==2){
            bool back=first.out[0]==h;auto next=first.out[back?1:0];
            o<<"    if(pc=="<<h<<"u){ [loop] for(;;){\n"<<first.body<<"    if("<<(back?"!(":"(")<<first.branch<<"))break;\n"
             <<"    } pc="<<next<<"u; }\n";continue;
        }
        bool cycle=c.size()>1 || self;
        if(cycle)o<<"    [loop] for(;;){bool advanced=false;\n";
        for(auto v:c){auto& r=regions.at(v);o<<"    if(pc=="<<v<<"u){\n";
            if(cycle)o<<"    advanced=true;\n";
            o<<r.body;
            if(r.out.size()==1)o<<"    pc="<<r.out[0]<<"u;\n";
            else if(r.out.size()==2)o<<"    pc=("<<r.branch<<")?"<<r.out[0]<<"u:"<<r.out[1]<<"u;\n";
            o<<"    }\n";
        }
        if(cycle)o<<"    if(!advanced)break; }\n";
    }
}
// Split a native workgroup at the *guest* S_BARRIER instructions. Within a
// phase waves may branch independently: there must be no workgroup execution
// barrier in that divergent control flow. In a linear barrier sequence emit
// the synchronizations directly. Otherwise each wave runs to its next
// rendezvous or terminates; finished waves still participate in host barriers.
#include "barrier_schedule.inl"
void emit_wave_phases(std::ostream& o,const std::map<unsigned,Str>& code,
        const std::map<unsigned,std::vector<unsigned>>& edges,const std::map<unsigned,Str>& preds,
        const std::map<unsigned,unsigned>& barriers,unsigned start,bool device_sync,const Graph& graph,
        const MaskEdges& clones,bool literal){
    constexpr unsigned done=~0u;
    struct Phase{std::set<unsigned> nodes,stops;};
    std::map<unsigned,Phase> phases;
    std::vector<unsigned> entries{start};
    size_t copies=0;
    while(!entries.empty()){
        auto entry=entries.back();entries.pop_back();if(phases.contains(entry))continue;
        auto& ph=phases[entry];std::vector<unsigned> todo{entry};
        while(!todo.empty()){
            auto h=todo.back();todo.pop_back();if(!ph.nodes.insert(h).second)continue;
            if(auto it=barriers.find(h);it!=barriers.end()){
                ph.stops.insert(it->second);entries.push_back(it->second);
            }else if(edges.at(h).empty())ph.stops.insert(done);
            else for(auto v:edges.at(h))todo.push_back(v);
        }
        copies+=ph.nodes.size();
    }
    const bool compact=copies<=4*code.size(); // avoid duplicating large shared tails
    auto emit=[&](unsigned entry,const auto& jumps_in){
        std::map<unsigned,Str> blocks;std::map<unsigned,std::vector<unsigned>> jumps;
        for(auto& [h,out]:jumps_in){auto text=code.at(h);
            if(auto it=barriers.find(h);it!=barriers.end()){
                auto at=text.rfind("    pc = ");if(at==Str::npos)unsupported("missing wave rendezvous terminator");
                text.resize(at);text+=std::format("    resume={}u; pc=0xffffffffu;\n",it->second);jumps[h]={};
            }else jumps[h]=out;
            blocks.emplace(h,std::move(text));
        }
        emit_flow_regions(o,blocks,jumps,preds,entry,true);
    };
    auto phase=[&](unsigned entry,const Phase& ph){
        std::map<unsigned,std::vector<unsigned>> jumps;for(auto h:ph.nodes)jumps[h]=edges.at(h);
        emit(entry,jumps);
    };
    std::vector<unsigned> order;std::set<unsigned> seen;auto next=start;bool linear=compact;
    while(linear && next!=done){
        if(!seen.insert(next).second){linear=false;break;}
        order.push_back(next);auto stops=phases.at(next).stops;stops.erase(done);
        if(stops.size()>1){linear=false;break;}next=stops.empty()?done:*stops.begin();
    }
    o<<"    uint resume=0u,pc=0u;\n";
    const char* sync=device_sync?"AllMemoryBarrierWithGroupSync();":"GroupMemoryBarrierWithGroupSync();";
    if(linear){
        for(auto entry:order){
            o<<std::format("    if(resume=={}u){{ pc=resume;\n",entry);
            phase(entry,phases.at(entry));o<<"    }\n";
            if(entry!=order.back())o<<"    "<<sync<<"\n";
        }
        return;
    }
    if(literal)if(auto steps=literal_barriers(graph,clones,edges,barriers,start)){
        o<<"    // Literal scalar control proves a common barrier sequence.\n";
        for(const auto& step:*steps){
            o<<"    { pc="<<step.entry<<"u;\n";emit(step.entry,step.jumps);o<<"    }\n";
            if(&step!=&steps->back())o<<"    "<<sync<<"\n";
        }
        return;
    }
    o<<"    bool retired=false;\n    [loop] for(;;){pc=resume;\n";
    if(compact){
        o<<"    switch(resume){\n";
        for(auto& [entry,ph]:phases){o<<std::format("    case {}u:{{\n",entry);phase(entry,ph);o<<"    }break;\n";}
        o<<"    default:break;}\n";
    }else{
        o<<"    [loop] while(pc!=0xffffffffu){switch(pc){\n";
        for(auto& [h,text]:code){
            o<<std::format("    case {}u:{{\n",h);
            if(auto it=barriers.find(h);it!=barriers.end()){
                auto at=text.rfind("    pc = ");if(at==Str::npos)unsupported("missing wave rendezvous terminator");
                o<<text.substr(0,at)<<std::format("    resume={}u;pc=0xffffffffu;break;\n",it->second);
            }else o<<text<<"\n    break;\n";
            o<<"    }\n";
        }
        o<<"    default:pc=resume=0xffffffffu;break;}}\n";
    }
    o<<"    if(!retired && resume==0xffffffffu){if(WaveIsFirstLane()){InterlockedAdd(xs_live,0xffffffffu);}retired=true;}\n"
      <<"    "<<sync<<"uint remaining=xs_live;GroupMemoryBarrierWithGroupSync();\n"
      <<"    if(remaining==0u)return;\n    }\n";
}
class Lower {
    const Program& p;
    Instruction decode(Bytes b,unsigned pc) const {return isa::decode(b,pc,p.wave);}
    Graph decode_graph(Bytes b,std::vector<Str>* gaps=nullptr) const {return isa::decode_graph(b,gaps,p.wave);}
    State state;
    mutable std::vector<LoopUse> accesses;
    std::vector<Block> blocks;
    struct EarlyExit {unsigned pc;std::optional<State> state;bool masked{};};
    std::vector<EarlyExit> loop_exits;
    std::optional<Graph> counted_graph;
    std::map<unsigned,unsigned> counted_latches;
    std::set<unsigned> emitting_loops;
    std::set<Str> vars;
    std::set<Str> representatives;
    // Equivalent single bits for full, native wave32 scalar mask words.
    // Local to a straight-line block; the numeric SGPR remains available.
    std::map<Str,Str> mask_bits;
    BitTransforms bit_transforms;
    // Full-wave values inherited by lanes excluded by an AND_SAVEEXEC.
    // The following complementary arm can still read their old VGPR words.
    std::map<unsigned,std::map<Str,Str>> opposite_values;
    // Interpolation coordinates are hardware inputs, not scalar numbers.
    // Keep their identity on lanes in the untouched arm of a masked region.
    using BaryMap=std::map<Str,Value>;
    BaryMap active_bary;
    std::map<unsigned,BaryMap> complementary_bary;
    bool dispatch_lowering=false;
    std::set<unsigned> used, exported;
    std::set<unsigned> sampled_textures,loaded_textures;
    std::set<unsigned> half_images,comparison_samplers,regular_samplers;
    std::map<unsigned,unsigned> view_widths; // resource id -> declared component count
    std::set<std::pair<unsigned,unsigned>> image_fields;
    std::set<std::pair<unsigned,unsigned>> sampler_fields;
    std::set<unsigned> fixed_widths;
    std::set<unsigned> integer_views;
    std::set<unsigned> atomic64_images;
    std::set<Str> gradient_regs,raw_words;
    std::map<unsigned,unsigned> paired_interp;
    bool track_quad_scopes=false;
    Names initial_undef,physical_quad_temps,undef_quad_temps;
    mutable Names provisional_reads;
    bool quad_uniform=true,multi_wave=false,native_group=false,soft_double=false,indexed_vgprs=false,lds_read_sequence=false;
    bool device_sync=false,vm_wait=false,cube_gradients=false,native_mulz=false,reference_queue=false;
    bool selective_vm=false,half_vectors=false,native_fcmp_pair=false,check_buffer_inputs=false,half_lds=false;
    std::set<unsigned> vm_access,vm_waits,vm_required,resolved_buffers;
    bool raw_sgprs=false;
    bool ray_mode=false,split_lane_predicates=false;
    bool pixel_wave_election=false,masked_pixel_election=false;
    std::map<unsigned,RelativeWindow> relative_windows;
    std::map<unsigned,unsigned> local_banks;
    bool triangle_queries=false;
    const InlineTrace* hit_post{};
    std::map<unsigned,std::pair<unsigned,unsigned>> table_words; // user SGPR -> (constant slot, binding)
    mutable std::vector<std::set<unsigned>> raw_sources{{}};
    std::set<unsigned> raw_writes;
    mutable std::set<unsigned> raw_reads;
    unsigned pool_start=~0u;
    unsigned arm{},side{},epoch{},mask_ids{};
    std::map<unsigned,unsigned> cb_end;
    std::map<unsigned,Str> gs_values;
    unsigned gs_emitted{},gs_flags{};
    std::ostringstream body;
    unsigned seq{},loop_depth{},emit_pc{};
    bool fetch_called{}, masked{}, virtual_wave{}, reachable{true};
    bool compare_alu(const Instruction&);
    bool scalar_alu(const Instruction&);
    bool vector_alu(const Instruction&);
    bool subword(const Instruction&);
    bool permute_alu(const Instruction&);
    bool half_alu(const Instruction&);
    bool relative_move(const Instruction&);
    bool subword_buffer(const Instruction&);
    bool wide_alu(const Instruction&);
    void scalar_dest(const Str&,const Str&);
    bool lds_io(const Instruction&);
    bool wide_lds_atomic(const Instruction&);
    bool atomic_op(View,Str,Str,unsigned,Range,bool);
    void atomic32(View,const Str&,const Str&,unsigned,const Str&,const Str&,const Str&);
    Str wide(const Str&,bool fp=false) const;
    Str halfword(Str,bool high=false) const;
    Value peek(const Str& r) const {
        if (auto it=state.find(r);it!=state.end()) return it->second;
        return {};
    }
    std::set<unsigned> sources(Value v) const{
        if(v.kind==Kind::descriptor)return {8*v.id+v.part};
        return raw_sources.at(v.raw_set);
    }
    unsigned merged_sources(Value a,Value b) const{
        auto s=sources(a),other=sources(b);s.insert(other.begin(),other.end());
        auto it=std::ranges::find(raw_sources,s);if(it!=raw_sources.end())return unsigned(it-raw_sources.begin());
        raw_sources.push_back(std::move(s));return unsigned(raw_sources.size()-1);
    }
    void consume_sources(Value v) const{
        if(!raw_sgprs || (!v.raw_set && v.kind!=Kind::descriptor))return;
        auto deps=sources(v);
        for(auto d:deps){
            auto& r=p.resources.at(d/8);
            bool structured=r.kind==12 && (r.type==5 || r.type==8);
            bool raw=r.kind==11 && (r.type==4 || r.type==7);
            bool typed_base=r.kind==10 && (r.type==3 || r.type==6) && d%8==0;
            if((!structured && !raw && !typed_base) || d%8>2)
                throw Unsupported(std::format("unmapped numeric SRD word {} of {}{} (kind {})",d%8,r.type>=6?'u':'t',r.reg,r.kind));
            raw_reads.insert(d);
        }
    }
    Value value(const Str& r) const {
        auto v=peek(r);
        if(v.kind!=Kind::unknown) for(auto& u:accesses) if(!u.defined.contains(r))u.live_in.insert(r);
        return v;
    }
    void line(const Str& s) { body<<"    "<<s<<"\n"; }
    bool physical_waves() const{return virtual_wave && p.native_compute_wave && !multi_wave && !reference_queue;}
    bool native_math() const{
        return p.stage==pixel || p.stage==vertex || (p.native_compute_wave && (!reference_queue ||
            u64(p.threads[0])*p.threads[1]*p.threads[2]==p.wave));
    }
    Str scalar_condition() const{return physical_waves() && value("scc").uniform?"(scc != 0u)":"ANY(scc != 0u)";}
    void begin_lds(bool read){
        if(!physical_waves())line("GroupMemoryBarrierWithGroupSync();");
        else if(!read || !lds_read_sequence)line("GroupMemoryBarrier();");
        // The next write still orders all preceding reads. Two LDS reads
        // in one straight-line block need no execution rendezvous or fence
        // between them. A branch target starts with unknown predecessor state.
        lds_read_sequence=read;
    }
    void end_lds(){if(!physical_waves())line("GroupMemoryBarrierWithGroupSync();");}
    Str temp(const Str& t,const Str& e) {
        auto name="tmp"+std::to_string(++seq);
        line(t+" "+name+" = "+e+";"); return name;
    }
    void set(const Str& r,Value v) {
        mask_bits.erase(r);
        state[r]=v;representatives.erase(r);
        active_bary.erase(r);
        if(track_quad_scopes && r.starts_with('s'))state.erase("quadmask_"+r);
        if(gradient_regs.contains(r)){state["grad_"+r]={};state.erase("quadword_"+r);state.erase("interp_base_"+r);state.erase("quaddef_"+r);}
        if(r.starts_with('v') && !r.starts_with("vcc")) {
            state.erase("flat_input_"+r);
            auto prefix="spill_"+r+"_";
            for(auto it=state.lower_bound(prefix);it!=state.end() && it->first.starts_with(prefix);)it=state.erase(it);
        }
        for(auto& u:accesses)u.defined.insert(r);
    }
    void write(const Str& r,const Str& expr,Value v,bool pred=false) {
        bool coordinate=gradient_regs.contains(r);
        bool quad_word=coordinate && peek("quadword_"+r).kind==Kind::immediate;
        auto defined=coordinate?peek("quaddef_"+r):Value{};
        set(r,v);vars.insert(r);
        if(gradient_regs.contains(r) && v.kind==Kind::number && (quad_word || (!masked && quad_uniform)))
            state["quadword_"+r]={Kind::immediate,1};
        if(track_quad_scopes && coordinate && v.kind==Kind::number){
            auto q=peek("quadmask_active");
            if(q.kind==Kind::immediate && q.part)state["quaddef_"+r]=q;
            else if(defined.kind==Kind::immediate)state["quaddef_"+r]=defined;
        }
        if (v.kind==Kind::number && pred && masked && v.uniform) {
            // EXEC may hide VGPR writes, but a readfirstlane under the same
            // mask can use the uniform value it would read in any active lane.
            auto u="uniform_"+r;vars.insert(u);line(u+" = "+expr+";");
            representatives.insert(r);state[r].uniform=false;
            line("if (active) "+r+" = "+u+";");
        } else line((pred && masked?"if (active) ":"")+r+" = "+expr+";");
        if(gradient_regs.contains(r) && v.kind==Kind::number && !masked && blocks.empty() && !loop_depth && quad_uniform) {
            for(auto axis:{"x","y"}){
                auto d=Str("dd")+axis+"_"+r;vars.insert(d);
                line(d+" = asuint(dd"+axis+"_fine(asfloat("+r+")));");
            }
            state["grad_"+r]=scalar;
        }
    }
    void check_range(Range r,char bank,unsigned count) const {
        if (r.bank!=bank || r.count!=count) unsupported("unexpected register bank/width");
    }
    Str bits(const Str& a) const {
        if (a=="null") return "0u";
        if((a=="vcc_lo"||a=="vcc_hi") && value("word_"+a).kind==Kind::number)return "word_"+a;
        if(a=="m0" && value(a).kind==Kind::number)return a;
        if(raw_words.contains(a))return "raw_"+a;
        if (a.empty()) unsupported("missing operand");
        if(a.starts_with("lit(") && a.ends_with(')'))return bits(a.substr(4,a.size()-5));
        if (a.starts_with('-') && (a.size()>1 && (a[1]=='v'||a[1]=='s'||a[1]=='|')))
            return "("+bits(a.substr(1))+" ^ 0x80000000u)";
        if (a.starts_with('|') && a.ends_with('|')) return "("+bits(a.substr(1,a.size()-2))+" & 0x7fffffffu)";
        if (a.starts_with('s') || a.starts_with('v') || a.starts_with("tmp")) {
            auto v=value(a);
            if(hit_post && v.kind==Kind::ray_address){
                if(a==hit_post->base_lo)return "0x10000u";
                if(a==hit_post->base_hi)return "0u";
            }
            if(raw_sgprs && v.kind==Kind::table)return a;
            consume_sources(v);
            if(raw_sgprs && v.kind==Kind::descriptor)return a;
            if(v.kind==Kind::undef_word && initial_undef.contains(a)){
                provisional_reads.insert(a);return a;
            }
            if (v.kind!=Kind::number && !(v.kind==Kind::masked_number && v.id==arm && v.part==side))
                throw Unsupported("unbound numeric register "+a);
            return a;
        }
        if (!a.starts_with("0x") && a.contains('.')) {
            float f;
            auto [end,ec]=std::from_chars(a.data(),a.data()+a.size(),f);
            if(ec!=std::errc{} || end!=a.data()+a.size()) unsupported("invalid float operand");
            return std::format("0x{:08x}u",std::bit_cast<u32>(f));
        }
        return std::format("{}u",a[0]=='-'?0u-number(View(a).substr(1)):number(a));
    }
    bool uniform(const Str& a) const {
        if (a.empty()) return false;
        if((a=="vcc_lo"||a=="vcc_hi") && value("word_"+a).kind==Kind::number)return true;
        if (a[0]=='-' || a[0]=='|') return uniform(a.substr(1,a.size()-(a[0]=='|'?2:1)));
        return (a[0]!='s' && a[0]!='v' && !a.starts_with("tmp")) || value(a).uniform;
    }
    Str active_bits(const Str& a) const {
        if(vgpr(a) && representatives.contains(a))return "uniform_"+a;
        if(a.size()>1 && a[0]=='-' && (a[1]=='v' || a[1]=='s' || a[1]=='|'))
            return "("+active_bits(a.substr(1))+" ^ 0x80000000u)";
        if(a.size()>2 && a.front()=='|' && a.back()=='|')
            return "("+active_bits(a.substr(1,a.size()-2))+" & 0x7fffffffu)";
        return bits(a);
    }
    bool active_uniform(const Str& a) const {
        if(!a.empty() && (a[0]=='-' || a[0]=='|'))return active_uniform(a.substr(1,a.size()-(a[0]=='|'?2:1)));
        return uniform(a) || (vgpr(a) && representatives.contains(a));
    }
    Str maskname(const Str& a) const {
        if (a=="vcc" || a=="vcc_lo") return "vcc";
        if (a=="exec" || a=="exec_lo") return "active";
        auto r=registers(a);
        if (r.bank!='s' || r.count>2) unsupported("unknown mask register");
        return reg('s',r.first);
    }
    Str mask(const Str& a) const {
        if(a=="-1")return "true";
        if(a=="0")return "false";
        auto r=maskname(a);
        if (r=="active") return r;
        if(auto it=mask_bits.find(r);it!=mask_bits.end())return it->second;
        if(virtual_wave && p.wave==64 && a.starts_with("s[") && value(r).kind==Kind::predicate){
            auto hi=reg('s',registers(a).first+1);auto high=value(hi);
            if(high.kind==Kind::number)return "(lane<32u ? "+r+"!=0u : (("+hi+">>(lane&31u))&1u)!=0u)";
            if(high.kind!=Kind::mask_high)unsupported("unbound upper half of wave64 mask");
            return "(lane<32u ? "+r+"!=0u : "+hi+"!=0u)";
        }
        if(virtual_wave && value(r).kind==Kind::number){
            consume_sources(value(r));
            if(p.wave==32)return "(("+r+" >> (lane & 31u)) & 1u)!=0u";
            auto regs=registers(a);auto hi=reg('s',regs.first+1);
            if(regs.bank=='s' && regs.first<105 && value(hi).kind==Kind::number){
                consume_sources(value(hi));
                return "((((lane<32u?"+r+":"+hi+") >> (lane&31u)) & 1u)!=0u)";
            }
        }
        if (value(r).kind!=Kind::predicate) unsupported("non-predicate used as a lane mask");
        return "("+r+" != 0u)";
    }
    bool full(const Str& a) const {
        if(a=="-1")return true;
        if(a=="0")return false;
        auto r=maskname(a);
        return r=="active"?!masked:value(r).kind==Kind::predicate && value(r).part==1;
    }
    // An exact WQM token is a union of complete raster quads, not necessarily
    // a full wave. Narrowing retains containment but loses completeness. Only
    // acyclic graphs use instruction PCs as tokens. The ordinary CFG meet
    // discards different definitions; a clobbered SGPR half loses the pair.
    Value quad_scope(const Str& a) const {
        if(!track_quad_scopes)return {};
        if(a=="-1" || a=="0")return {Kind::immediate,a=="0"?~1u:~0u,1};
        if(a=="exec" || (p.wave==32 && a=="exec_lo"))return peek("quadmask_active");
        if(a.size()<2 || a[0]!='s')return {};
        auto range=registers(a);
        if(range.count!=(p.wave==64?2u:1u))return {};
        auto q=peek("quadmask_"+reg('s',range.first));
        if(range.count==2 && q!=peek("quadmask_"+reg('s',range.first+1)))return {};
        return q;
    }
    Value quad_logic(Value a,Value b,bool intersect,bool invert_b=false) const {
        if(a.kind==Kind::immediate && a.part && b.kind==Kind::immediate && b.part)
            return {Kind::immediate,emit_pc+1,1};
        if(intersect){
            if(a.kind==Kind::immediate){a.part=0;return a;}
            if(!invert_b && b.kind==Kind::immediate){b.part=0;return b;}
        }
        return {};
    }
    static Str quad_any(const Str& source){
        // Boolean short-circuiting would remove each set lane from later
        // QuadRead operations. All four lanes must supply their original bit.
        return std::format("((uint({0}) | QuadReadAcrossX(uint({0})) | QuadReadAcrossY(uint({0})) | QuadReadAcrossDiagonal(uint({0}))) != 0u)",source);
    }
    void putmask(const Str& a,const Str& e,bool all=false,Value q={}) {
        auto r=maskname(a);
        if(r=="vcc"){
            state.erase("word_vcc_lo");
            if(a!="vcc_lo" || p.wave!=32)state.erase("word_vcc_hi");
            state.erase("split_vcc_lo");state.erase("split_vcc_hi");
        }
        if (r=="active") { line("active = "+(multi_wave?"member && ("+e+")":e)+";"); masked=!all||multi_wave;representatives.clear();active_bary.clear();arm=0;++epoch; }
        else if(raw_sgprs && r!="vcc"){
            // A hardware SGPR is still a 32-bit word after a mask operation.
            // At CFG joins one predecessor can supply an integer and another
            // a ballot. Normalize mask writes into actual words for these
            // kernels instead of joining per-lane booleans with integers.
            auto regs=registers(a);
            if(regs.bank!='s' || regs.count>2)unsupported("invalid scalar mask width");
            auto pred=temp("bool",e);
            auto lo=temp("uint","group_reduce("+pred+",lane,4u)");
            if(regs.count==2)write(reg('s',regs.first+1),"group_reduce("+pred+",lane,5u)",scalar);
            write(r,lo,scalar);
        }
        else {
            write(r,"("+e+" ? 1u : 0u)",{Kind::predicate,0,unsigned(all)});
            if(split_lane_predicates || !relative_windows.empty())state[r].mask_origin=emit_pc+1;
            if (a.find(':')!=a.npos) {auto rr=registers(a);auto h=reg('s',rr.first+1);
                if(virtual_wave && p.wave==64)write(h,r,{Kind::mask_high,rr.first,unsigned(all)});
                else set(h,{Kind::mask_high,rr.first,unsigned(all)});
                if(split_lane_predicates)state[h].mask_origin=emit_pc+1;}
        }
        if(track_quad_scopes){
            if(r=="active"){
                state.erase("quadmask_active");
                if(q.kind==Kind::immediate && (a=="exec" || p.wave==32))state["quadmask_active"]=q;
            }else if(a.starts_with('s') && q.kind==Kind::immediate){
                auto range=registers(a);
                if(range.count==(p.wave==64?2u:1u))for(unsigned k=0;k<range.count;++k)
                    state["quadmask_"+reg('s',range.first+k)]=q;
            }
        }
    }
    const Semantic& semantic(const std::vector<Semantic>& sig,unsigned r,unsigned mask=1) const {
        auto it=std::ranges::find_if(sig,[&](auto& s){return s.reg==r && (s.mask&mask)==mask;});
        if (it==sig.end()) unsupported("linkage register not present in signature");
        return *it;
    }
    void inputs() {
        for (const auto& r:p.resources) {
            auto name=reg('s',r.table);
            if(raw_sgprs && !table_words.contains(r.table)){
                unsigned slot=table_words.size();
                if(slot>=view::table_count)unsupported("user-SGPR table constant limit");
                unsigned group=r.type==1?3u:r.type==2?2u:r.type>=6?1u:0u;
                table_words.emplace(r.table,std::pair{slot,r.reg|(group<<8)});
                write(name,std::format("xs_tables[{}].{}",slot/4,xyzw[slot%4]),{Kind::table,r.table,0,true});
            }else if(!raw_sgprs)set(name,{Kind::table,r.table});
        }
        if (p.stage==compute) {
            auto hw=p.cs_rsrc2;
            if (hw&1 || ((hw>>11)&3)>2) unsupported("extended compute system registers");
            unsigned g=(hw>>1)&31;
            for (unsigned c=0;c<3;++c) if (hw&(128u<<c)) write(reg('s',g++),Str("group.")+xyzw[c],scalar);
            for (unsigned c=0;c<=((hw>>11)&3);++c) write(reg('v',c),Str("local.")+xyzw[c],varying);
        } else if (p.stage==vertex) {
            for (const auto& s:p.inputs) if (s.used && (s.system==6 || s.system==8))
                write(s.system==6?"v0":"v3","input."+field(s),varying);
        } else if(p.stage==geometry) {
            set("v0",{Kind::gs_indices,0});set("v1",{Kind::gs_indices,2});
            set("s2",{Kind::gs_out});set("s3",{Kind::gs_wave});
            set("s8",{Kind::gs_param,0});set("s9",{Kind::gs_param,1});
        } else {
            set("s24",{Kind::prim});
            constexpr unsigned modes[]{6,2,3,0,7,4,5};
            unsigned vgpr=0;
            for(unsigned b=0;b<std::size(modes);++b)if(p.ps_input&(1u<<b))
                for(unsigned ij=0;ij<2;++ij)set(reg('v',vgpr++),{Kind::bary,ij,modes[b]});
            for (unsigned c=0;c<4;++c) if(p.ps_input&(256u<<c)) {
                for(const auto& s:p.inputs) if(s.system==1 && (s.used&(1u<<c)))
                    write(reg('v',vgpr),std::format("asuint(input.{}{})",field(s),component(s.mask,c)),varying);
                ++vgpr;
            }
            if(p.ps_input&(1u<<12)){
                for(unsigned j=0;j<p.inputs.size();++j)if(p.inputs[j].system==9 && p.inputs[j].used)
                    set(reg('v',vgpr),{Kind::front_face,j});
                ++vgpr;
            }
            if(p.ps_input&(1u<<13)) {
                for(const auto& s:p.inputs)if(s.system==4 && s.used)
                    set(reg('v',vgpr),{Kind::ancillary,s.reg}); // packed rasterizer layer field only
            }
        }
    }
    // In a hardware GS, the packed input VGPRs designate per-primitive LDS
    // vertices. Follow address provenance; do not pretend their numeric
    // hardware addresses are available through desktop D3D.
    bool gs_instruction(const Instruction& i) {
        auto& a=i.args;auto& op=i.op;
        auto src=[&](unsigned n){return value(a.at(n));};
        auto imm=[&](unsigned n,u32 v){return a.at(n)==std::to_string(v) || a[n]==std::format("0x{:x}",v);};
        auto out=[&](Kind k,unsigned id=0,unsigned part=0){set(a.at(0),{k,id,part});return true;};
        if(op=="s_bfe_i32" && src(1).kind==Kind::gs_wave && imm(2,0x80008))return out(Kind::gs_wave,1);
        if(op=="s_bfm_b64" && a.at(0)=="exec" && src(1)==Value{Kind::gs_wave,1} && imm(2,0)) {
            gs_flags|=1;return true; // GS invocation represents one live primitive lane
        }
        if(op=="s_cmp_eq_u32" && src(0)==Value{Kind::gs_wave,1} && imm(1,64)){gs_flags|=2;return true;}
        if(op=="s_cselect_b64" && a==std::vector<Str>{"exec","-1","exec"} && (gs_flags&3)==3) {gs_flags|=4;return true;}
        if(op=="s_and_b32" && src(1)==Value{Kind::gs_param,1} && imm(2,255))return out(Kind::gs_param,2);
        if(op=="s_mov_b32" && src(1).kind==Kind::gs_param){set(a[0],src(1));return true;}
        if(op=="s_load_dword" && imm(2,0xb8)) {
            auto base=registers(a[1]);check_range(base,'s',2);
            if(value(reg('s',base.first))==Value{Kind::gs_param,0} && value(reg('s',base.first+1))==Value{Kind::gs_param,2})
                return out(Kind::gs_param,3);
        }
        if(op=="s_lshl_b32" && src(1)==Value{Kind::gs_param,3} && imm(2,8))return out(Kind::gs_param,4);
        if(op=="s_add_i32" && src(1)==Value{Kind::gs_param,4} && src(2)==Value{Kind::gs_out})
            return out(Kind::gs_out,1);
        if(op=="v_mbcnt_lo_u32_b32" && a.at(1)=="-1" && imm(2,0))return out(Kind::gs_lane,0);
        if(op=="v_mbcnt_hi_u32_b32" && a.at(1)=="-1" && src(2)==Value{Kind::gs_lane,0})return out(Kind::gs_lane,1);
        if(op=="v_mad_u32_u24" && imm(1,p.max_vertices*p.output_vectors*16) &&
           src(2)==Value{Kind::gs_lane,1} && src(3)==Value{Kind::gs_out,1})return out(Kind::gs_out,2);
        if(op=="v_lshlrev_b32" && imm(1,2) && src(2).kind==Kind::gs_indices)return out(Kind::gs_in,src(2).id,1);
        if(op=="v_and_b32" && imm(1,0x3fffc) && src(2).kind==Kind::gs_in && src(2).part==1)
            return out(Kind::gs_in,src(2).id);
        if(op=="v_lshlrev_b32_sdwa" && src(1)==Value{Kind::number,2,2,true} &&
           src(2)==Value{Kind::gs_indices,0} && i.modifiers=="dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:DWORD src1_sel:WORD_1")
            return out(Kind::gs_in,1);
        if(op=="v_mov_b32" && imm(1,2)) {write(a[0],"2u",{Kind::number,2,2,true});return true;}
        if(op=="s_bfe_u32" && src(1)==Value{Kind::gs_wave} && imm(2,0x80010))return out(Kind::gs_wave,2);
        if(op=="s_mov_b32" && a.at(0)=="m0" && src(1)==Value{Kind::gs_wave,2}){gs_flags|=8;return true;}
        bool read=op=="ds_read_b32" || op=="ds_read2_b32",write_lds=op=="ds_write_b32" || op=="ds_write2_b32";
        if(read || write_lds) {
            bool pair=op.contains("2_");auto addr=src(read?1:0);auto dst=read?registers(a.at(0)):Range{};
            if(read)check_range(dst,'v',pair?2:1);
            if ((read && (addr.kind!=Kind::gs_in || addr.part || addr.id>=3)) ||
                (write_lds && addr!=Value{Kind::gs_out,2}))unsupported("unmapped GS ring address");
            unsigned offsets[2]{};std::istringstream fields(i.modifiers);Str token;
            while(fields>>token) {
                if(pair && (token.starts_with("offset0:") || token.starts_with("offset1:")))
                    offsets[token[6]-'0']=4*number(View(token).substr(8));
                else if(!pair && token.starts_with("offset:"))offsets[0]=number(View(token).substr(7));
                else unsupported("extended GS ring addressing");
            }
            for(unsigned j=0;j<(pair?2u:1u);++j) {
                auto off=offsets[j];if(off%4)unsupported("unaligned GS ring IO");
                if(read) {
                    auto& s=semantic(p.inputs,off/16,1u<<(off%16/4));
                    write(reg('v',dst.first+j),std::format("asuint(input[{}].{}{})",addr.id,field(s),component(s.mask,off%16/4)),varying);
                } else {
                    if(off/(p.output_vectors*16)!=gs_emitted)unsupported("non-sequential GS stream layout");
                    // Capture the value at the store, before another instruction reuses its VGPR.
                    gs_values[off%(p.output_vectors*16)]=temp("uint",bits(a.at(j+1)));
                }
            }
            return true;
        }
        if(op=="s_sendmsg" && a==std::vector<Str>{"sendmsg(MSG_GS","GS_OP_EMIT","0)"}) {
            if((gs_flags&15)!=15 || gs_emitted>=p.max_vertices)unsupported("invalid GS emit state");
            for(auto& s:p.outputs)for(unsigned c=0;c<4;++c)if(s.mask&(1u<<c)) {
                auto off=16*s.reg+4*c;
                if(!gs_values.contains(off))unsupported("missing vertex ring output");
                line(std::format("output.{}{} = {};",field(s),component(s.mask,c),as_output(s,gs_values.at(off))));
            }
            ++gs_emitted;gs_values.clear();line("stream.Append(output);");return true;
        }
        if(op=="s_sendmsg" && a==std::vector<Str>{"sendmsg(MSG_GS","GS_OP_CUT","0)"}){line("stream.RestartStrip();");return true;}
        if(op=="s_sendmsg" && a==std::vector<Str>{"sendmsg(MSG_GS_DONE","GS_OP_NOP)"}){gs_flags|=16;return true;}
        // Control transfers require a model of the number/order of emitted vertices.
        if(op.starts_with("s_branch") || op.starts_with("s_cbranch") || op.starts_with("s_sendmsg"))
            unsupported("extended geometry control flow");
        return false;
    }
    void fetch() {
        for (const auto& a:p.fetch) {
            const auto& s=semantic(p.inputs,a.reg,a.mask);
            if (s.system || (s.mask&a.mask)!=a.mask || a.base+std::popcount(a.mask)>256) unsupported("unknown vertex fetch signature");
            for (unsigned c=0;c<4;++c) if (a.mask&(1u<<c))
                write(reg('v',a.base+c),std::format("asuint(input.{}{})",field(s),component(s.mask,c)),varying);
        }
        fetch_called=true;
    }
    bool isolated_mask_arm(unsigned begin,unsigned end,std::vector<Range> saved) const {
        if(saved.size()>8)return false;
        for(unsigned pc=begin;pc<end;){
            auto v=decode(p.code,pc);pc+=v.size;if(pc>end)return false;
            if(!v.args.empty() && v.args[0].size()>1 && v.args[0][0]=='s' &&
               (std::isdigit(static_cast<unsigned char>(v.args[0][1])) || v.args[0][1]=='[')){
                auto d=registers(v.args[0]);
                if(std::ranges::any_of(saved,[&](Range s){return d.first<s.first+s.count && s.first<d.first+d.count;}))return false;
            }
            if(v.op=="s_and_saveexec_b64" || v.op=="s_and_saveexec_b32"){
                if(pc>=end)return false;
                auto guard=decode(p.code,pc);
                if(guard.op!="s_cbranch_execz" || target(guard)<pc+guard.size || target(guard)>=end)return false;
                auto restore=decode(p.code,target(guard));
                if(restore.op!="s_mov_b64" || restore.args!=std::vector<Str>{"exec",v.args[0]})return false;
                auto nested=saved;nested.push_back(registers(v.args[0]));
                if(!isolated_mask_arm(pc+guard.size,restore.pc,std::move(nested)))return false;
                pc=restore.pc+restore.size;continue;
            }
            if(v.op=="s_branch" || v.op.starts_with("s_cbranch") || v.op.contains("exec") ||
               v.op.starts_with("v_cmpx") || v.op=="v_writelane_b32" ||
               (!v.args.empty() && v.args[0].starts_with("exec")))return false;
        }
        return true;
    }
    bool control(const Instruction& i) {
        auto& a=i.args; auto& op=i.op;
        if(op=="s_setreg_imm32_b32" || op=="s_getreg_b32") {
            bool read=op=="s_getreg_b32";
            // SOPK HW_REG_MODE[3:2] is FP_ROUND for FP16 and FP64. Both
            // our FP16 narrowing and the software FP64 path implement RNE.
            // Decode the field from the instruction; LLVM splits hwreg(...)
            // at the internal commas in its text representation.
            auto imm=p.code.at<u32>(i.pc)&65535u;
            if((imm&63u)==1u && ((imm>>6)&31u)==2u && (imm>>11)==1u){
                if(!read && i.size==8 && p.code.at<u32>(i.pc+4)==0){
                    write("fp16_64_round","0u",scalar);return true;
                }
                if(read && value("fp16_64_round").kind==Kind::number){
                    check_range(registers(a.at(0)),'s',1);write(a[0],"fp16_64_round",scalar);return true;
                }
                unsupported("unmapped FP16/64 rounding mode");
            }
            if(a.size()!=2)unsupported("partial hardware-register access");
            auto& hw=a[read?1:0];
            Str r=hw=="hwreg(HW_REG_FLAT_SCR_LO)"?"flat_scr_lo":hw=="hwreg(HW_REG_FLAT_SCR_HI)"?"flat_scr_hi":"";
            if(r.empty())unsupported("unsupported hardware register "+hw);
            // These are the wave's flat-scratch aperture registers, not user
            // memory. DXC can eliminate their setup when no instruction reads it.
            if(read) {
                if(value(r).kind!=Kind::number)unsupported("uninitialized scratch aperture");
                check_range(registers(a[0]),'s',1);write(a[0],r,scalar);
            } else write(r,std::format("{}u",number(a[1])),scalar);
            return true;
        }
        if(op=="s_waitcnt") {
            if(vm_wait && std::ranges::find(a,"vmcnt(0)")!=a.end()){
                if(selective_vm){vm_waits.insert(i.pc);line(std::format("XS_DEVICE_WAIT_{}();",i.pc));}
                else line("DeviceMemoryBarrier();");
            }
            return true;
        }
        if(p.stage==compute && (op=="s_waitcnt_vscnt" || op=="buffer_gl0_inv" || op=="buffer_gl1_inv")) {
            if(op=="s_waitcnt_vscnt" ? (a.size()!=2 || a[0]!="null" || number(a[1])!=0) :
                (!a.empty() || !i.modifiers.empty()))unsupported("extended cache/wait operand");
            // The memory barrier also prevents the host from reusing a load
            // across a guest cache invalidation. No workgroup rendezvous is
            // implied by any of these three instructions.
            device_sync=true;line("DeviceMemoryBarrier();");return true;
        }
        // These order the hardware's own register scoreboards / fetch cache.
        // Lowered register reads have data dependencies and code is immutable.
        if (op=="s_inst_prefetch" || op=="s_nop" || op=="v_nop" || op=="v_pipeflush" ||
            op=="s_clause" || op=="s_icache_inv" || op=="s_waitcnt_depctr") return true;
        if(p.stage==compute && (op=="s_dcache_inv"||op=="s_gl1_inv")){
            device_sync=true;line("DeviceMemoryBarrier();");return true;
        }
        if (op=="s_swappc_b64" && !i.pc && p.stage==vertex && !p.fetch.empty() &&
            a==std::vector<Str>{"s[4:5]","s[4:5]"}) {fetch();return true;}
        if (op=="s_wqm_b64" || op=="s_wqm_b32") {
            bool all=full(a.at(1));auto src=mask(a[1]);Str e;
            if(all)e="true";
            else if(p.stage==pixel)e=quad_any(src);
            else if(virtual_wave)e=std::format("((group_reduce({},lane,lane<32u?4u:5u) >> (lane&28u)) & 15u) != 0u",src);
            else unsupported("unmapped quad EXEC expansion");
            putmask(a.at(0),e,all,{Kind::immediate,i.pc+1,1});write("scc",mask(a[0])+" ? 1u : 0u",varying);return true;
        }
        if (op=="s_mov_b64") {putmask(a.at(0),mask(a.at(1)),full(a.at(1)),quad_scope(a.at(1)));return true;}
        if(op=="s_cselect_b64") {
            if(value("scc").kind!=Kind::number)unsupported("unbound mask select SCC");
            putmask(a.at(0),"("+scalar_condition()+" ? "+mask(a.at(1))+" : "+mask(a.at(2))+")");return true;
        }
        if (op=="s_and_saveexec_b64" || op=="s_and_saveexec_b32") {
            auto rhs=temp("bool",mask(a.at(1)));
            auto inherited=active_bary;
            if(p.stage==pixel && !dispatch_lowering && !loop_depth){
                auto next=decode(p.code,i.pc+i.size);
                if(next.op=="s_cbranch_execz" && target(next)>next.pc && target(next)<p.code.size()){
                    auto split=decode(p.code,target(next));
                    if(split.op=="s_andn2_b64" && split.args==std::vector<Str>{"exec",a[0],"exec"} &&
                       isolated_mask_arm(next.pc+next.size,split.pc,{registers(a[0])})){
                        auto known=inherited;
                         for(auto& [name,w]:state)if(w.kind==Kind::bary && vgpr(name))known[name]=w;
                        complementary_bary[split.pc]=std::move(known);
                    }
                }
            }
            auto id=masked?0:++mask_ids;
            if(id && !virtual_wave)for(const auto& [r,v]:state)if(vgpr(r) && v.kind==Kind::number && v.uniform){
                auto saved=std::format("incoming{}_{}",id,r);
                vars.insert(saved);line(saved+" = "+r+";");opposite_values[id][r]=saved;
            }
            auto old_quad=quad_scope("exec"),rhs_quad=quad_scope(a[1]);
            putmask(a.at(0),"active",!masked,old_quad);
            state[maskname(a[0])].id=id;
            putmask("exec","active && "+rhs,false,quad_logic(old_quad,rhs_quad,true));
            active_bary=std::move(inherited); // narrowing preserves existing active-lane facts
            arm=id;side=0;
            write("scc","active ? 1u : 0u",varying);return true;
        }
        if(op.contains("_saveexec_") || op.contains("_wrexec_")){
            auto rhs=temp("bool",mask(a.at(1)));auto prev=temp("bool","active");
            View fn=View(op).substr(2);fn=fn.substr(0,fn.find('_'));Str v;
            if(fn=="or")v=rhs+" || "+prev;
            else if(fn=="xor")v=rhs+" != "+prev;
            else if(fn=="andn1")v="!"+rhs+" && "+prev;
            else if(fn=="andn2")v=rhs+" && !"+prev;
            else if(fn=="orn1")v="!"+rhs+" || "+prev;
            else if(fn=="orn2")v=rhs+" || !"+prev;
            else if(fn=="nand")v="!("+rhs+" && "+prev+")";
            else if(fn=="nor")v="!("+rhs+" || "+prev+")";
            else if(fn=="xnor")v=rhs+" == "+prev;
            else unsupported("unknown EXEC mask operation");
            putmask(a[0],op.contains("_wrexec_")?v:prev,!masked && op.contains("_saveexec_"));
            putmask("exec",v);write("scc","active ? 1u : 0u",varying);return true;
        }
        if(op=="s_not_b64"||op=="s_nand_b64"||op=="s_nor_b64"||op=="s_xnor_b64"||op=="s_orn2_b64"){
            auto x=mask(a.at(1));Str e;
            if(op=="s_not_b64")e="!("+x+")";
            else{auto y=mask(a.at(2));e=op=="s_nand_b64"?"!("+x+" && "+y+")":op=="s_nor_b64"?"!("+x+" || "+y+")":
                    op=="s_xnor_b64"?"("+x+" == "+y+")":"("+x+" || !"+y+")";}
            putmask(a[0],e);write("scc",mask(a[0])+" ? 1u : 0u",varying);return true;
        }
        if (op=="s_and_b64" || op=="s_andn2_b64" || op=="s_or_b64" || op=="s_xor_b64") {
            auto x=mask(a.at(1)),y=mask(a.at(2));
            Str active_constant;
            if(!raw_sgprs && op=="s_and_b64" && x=="active" && a[2]!="0" && a[2]!="-1" && value(maskname(a[2])).uniform)
                active_constant=temp("bool",y);
            bool all=op=="s_and_b64"?(full(a[1]) && full(a[2])):op=="s_or_b64" && (full(a[1]) || full(a[2]));
            auto id=op=="s_andn2_b64" && maskname(a[0])=="active" && y=="active" && full(a[1]) && a[1]!="-1" &&
                value(maskname(a[1])).id==arm?arm:0;
            unsigned complement=1-side;
            auto quad=quad_logic(quad_scope(a[1]),quad_scope(a[2]),op=="s_and_b64" || op=="s_andn2_b64",op=="s_andn2_b64");
            putmask(a.at(0),x+(op=="s_and_b64"?" && ":op=="s_andn2_b64"?" && !":op=="s_or_b64"?" || ":" != ")+y,all,quad);
            if(auto it=complementary_bary.find(i.pc);it!=complementary_bary.end()){
                active_bary=std::move(it->second);complementary_bary.erase(it);
            }
            if(!active_constant.empty() && maskname(a[0])!="active") {
                auto r=maskname(a[0]);representatives.insert(r);vars.insert("uniform_"+r);
                line("uniform_"+r+" = "+active_constant+" ? 1u : 0u;");
            }
            if(id) {
                arm=id;side=complement;
                if(complement==1 && opposite_values.contains(id)) {
                    for(const auto& [r,saved]:opposite_values.at(id)){
                        representatives.insert(r);vars.insert("uniform_"+r);line("uniform_"+r+" = "+saved+";");
                    }
                    opposite_values.erase(id);
                }
            }
            write("scc",mask(a[0])+" ? 1u : 0u",varying);return true;
        }
        if (op=="s_cbranch_execz" || op=="s_cbranch_execnz" || op=="s_cbranch_vccnz" || op=="s_cbranch_vccz" ||
            op=="s_cbranch_scc0" || op=="s_cbranch_scc1") {
            auto arg=a.at(0);
            if (arg.starts_with('-')) unsupported("back edge");
            auto target=u64(i.pc)+i.size+4ull*number(arg);
            if (target>=p.code.size() || (!blocks.empty() && target>blocks.back().end))
                unsupported("unstructured forward edge");
            Str keep;
            if (op=="s_cbranch_execz" || op=="s_cbranch_execnz") keep=op.ends_with("execz")?"ANY(active)":"!ANY(active)";
            else if (op.starts_with("s_cbranch_vcc")) keep=(op.ends_with("nz")?"!":"")+Str("ANY(")+mask("vcc")+")";
            else {
                if(value("scc").kind!=Kind::number) unsupported("unbound SCC");
                keep=(op.ends_with('0')?"":"!")+scalar_condition();
            }
            line("if ("+keep+") {"); blocks.push_back({unsigned(target),state,masked,{}});
            if(op=="s_cbranch_execz") {blocks.back().guard=arm;blocks.back().side=side;blocks.back().epoch=epoch;}
            for(auto& u:accesses)blocks.back().definitions.push_back(u.defined);
            // A scalar branch restricts which waves reach the body, but does
            // not change EXEC. Representatives are invalidated on the join.
            return true;
        }
        return false;
    }
    State join(State a,const State& b) const {
        for (auto& [r,v]:a) {
            auto it=b.find(r);
            if(it!=b.end() && it->second==v) continue;
            auto numeric_rep=[](Value x){return x.kind==Kind::number||x.kind==Kind::table||x.kind==Kind::descriptor||x.kind==Kind::undef_word;};
            if(raw_sgprs && it!=b.end() && numeric_rep(v) && numeric_rep(it->second) &&
               (v.kind==Kind::descriptor || it->second.kind==Kind::descriptor || v.raw_set || it->second.raw_set)){
                auto source_set=merged_sources(v,it->second);
                v=scalar;v.raw_set=source_set;continue;
            }
            if(raw_sgprs && it!=b.end() && ((v.kind==Kind::table && (it->second.kind==Kind::number || it->second.kind==Kind::table)) ||
               (v.kind==Kind::number && it->second.kind==Kind::table))){
                v=scalar;continue; // raw bits retained; no longer a statically resolvable descriptor table
            }
            if(it!=b.end() && ((v.kind==Kind::undef_word && it->second.kind==Kind::number) ||
                                (it->second.kind==Kind::undef_word && v.kind==Kind::number))) {
                // A mutable word survives a masked write in every lane in
                // which it already had a value. A missing definition of a
                // *temporary* is SSA undef on that edge, not a new value.
                const auto num=v.kind==Kind::number?v:it->second;
                v=r.starts_with('s') && num.uniform?scalar:varying;
            }else if(it!=b.end() && v.kind==Kind::number && it->second.kind==Kind::number) {
                v.uniform &= it->second.uniform;
                if(it->second.part!=v.part)v.part=0;
            } else if(it!=b.end() && ((v.kind==Kind::bounded_relative && (it->second.kind==Kind::number||it->second.kind==Kind::undef_word)) ||
                                     (it->second.kind==Kind::bounded_relative && (v.kind==Kind::number||v.kind==Kind::undef_word)))){
                if(v.kind!=Kind::bounded_relative)v=it->second;
            }else if(it!=b.end() && v.kind==Kind::predicate && it->second.kind==Kind::predicate) {
                // Full-EXEC is a fact about the bits, independent of which
                // saveexec instruction supplied the value on an incoming edge.
                if(v.part!=it->second.part)v.part=0;
                if(v.id!=it->second.id)v.id=0;
                v.uniform&=it->second.uniform;
                if(v.mask_origin!=it->second.mask_origin)v.mask_origin=0;
            }else if(it!=b.end() && v.kind==Kind::mask_high && it->second.kind==Kind::mask_high && v.id==it->second.id){
                if(v.part!=it->second.part)v.part=0;
                if(v.mask_origin!=it->second.mask_origin)v.mask_origin=0;
            }
            else v={};
        }
        return a;
    }
    void close_blocks(unsigned pc,size_t min_depth=0) {
        while (blocks.size()>min_depth && blocks.back().end<=pc) {
            if (blocks.back().end!=pc) unsupported("branch into an operand");
            if(reachable) {
                const auto& b=blocks.back();
                auto merged=join(state,b.entry);
                // A VGPR can be assigned for the first time in complementary
                // EXEC arms. An execz bypass means *no* lane in that arm was
                // active, not that the register is undefined on its active lanes.
                if(b.guard && b.epoch==epoch)for(auto& [r,v]:state) {
                    if(r[0]!='v' || v.kind!=Kind::number || merged[r].kind==Kind::number)continue;
                    auto old=b.entry.find(r);
                    if(old!=b.entry.end() && old->second.kind==Kind::masked_number && old->second.id==b.guard &&
                       old->second.part!=b.side) merged[r]=varying;
                    else merged[r]={Kind::masked_number,b.guard,b.side};
                }
                state=std::move(merged);masked|=b.masked;
            }
            else {state=blocks.back().entry;masked=blocks.back().masked;reachable=true;}
            for(unsigned j=0;j<blocks.back().definitions.size();++j)
                accesses[j].defined=std::move(blocks.back().definitions[j]);
            representatives.clear();
            active_bary.clear();
            blocks.pop_back(); line("}");
        }
    }
    bool numeric(const Instruction& i) {
        auto& a=i.args; auto op=View(i.op);
        if(op=="v_writelane_b32" || op=="v_readlane_b32"){
            bool load=op=="v_readlane_b32";auto vg=a.at(load?1:0);
            // WRITELANE ignores EXEC and can change the saved complement.
            if(!load)for(auto& [id,values]:opposite_values)values.erase(vg);
            check_range(registers(vg),'v',1);auto ix=a.at(2);
            bool fixed=!ix.empty() && (std::isdigit(static_cast<unsigned char>(ix[0])) || ix[0]=='-');
            unsigned index=fixed?(ix[0]=='-'?0u-number(View(ix).substr(1)):number(ix))&(p.wave==32?31u:63u):0;
            // An SGPR spill at a constant lane can be restored without
            // relying on an identical host subgroup layout. Preserve SRD tags.
            Str spill=std::format("spill_{}_{}",vg,index);
            if(fixed && (index<32 || p.wave==64) && (!load || value(spill).kind!=Kind::unknown)){
                auto src=load?spill:a.at(1),dst=load?a[0]:spill;auto v=value(src);
                if(load && !virtual_wave && (dst=="vcc_lo" || dst=="vcc_hi")){
                    // Two fixed-lane spills can reconstruct a wave64 predicate
                    // on a host of any wave size. Require the same defining
                    // comparison for both halves; do not guess host lane IDs.
                    bool lo=dst=="vcc_lo";
                    if((lo && v.kind==Kind::predicate) || (!lo && v.kind==Kind::mask_high)){
                        state.erase("word_"+dst);set("vcc",{});
                        if(lo)write("split_vcc_lo",src,v);else set("split_vcc_hi",v);
                        auto low=peek("split_vcc_lo"),high=peek("split_vcc_hi");
                        if(low.kind==Kind::predicate && high.kind==Kind::mask_high && low.mask_origin && low.mask_origin==high.mask_origin)
                            putmask("vcc","split_vcc_lo!=0u");
                        return true;
                    }
                    if(v.kind==Kind::number){state.erase("split_"+dst);scalar_dest(dst,src);return true;}
                }
                if(!load && indexed_vgprs && (v.kind==Kind::descriptor || v.kind==Kind::table))unsupported("relative VGPR aliases a symbolic descriptor");
                // A spill may copy an unused physical SGPR (e.g. the high
                // half of an as-yet uninitialized table pointer). Keep it
                // opaque through moves; consumption still requires a real
                // definition. Do not synthesize a numeric zero for it.
                if(!load && !virtual_wave && v.kind==Kind::unknown && src.size()>1 && src[0]=='s' &&
                   View(src).find_first_not_of("0123456789",1)==View::npos)set(dst,{Kind::opaque_word});
                else if(v.kind==Kind::opaque_word || v.kind==Kind::descriptor || v.kind==Kind::table ||
                        (!virtual_wave && (v.kind==Kind::mask_high || v.kind==Kind::prim)))set(dst,v);
                else if(!virtual_wave && v.kind==Kind::predicate)write(dst,src,v);
                else {Str word=load?src:bits(src);write(dst,word,scalar);
                    if(v.kind==Kind::number)state[dst].part=v.part;
                    else if(!load && src=="0")state[dst].part=1;
                }
                if(!load){
                    if(virtual_wave && value(spill).kind==Kind::number){
                        vars.insert(vg);line(std::format("if(lane=={}u) {}={};",index,vg,spill));state[vg]=varying;
                    }else state[vg]={}; // only the known spilled lanes may be read
                }
                return true;
            }
            if(!virtual_wave)unsupported("lane transfer requires guest wave size");
            auto lane="("+bits(ix)+" & "+std::to_string(p.wave-1)+"u)";
            if(load)write(a[0],"group_lane("+bits(vg)+","+lane+",lane)",scalar);
            else{
                vars.insert(vg);set(vg,varying);line("if(lane=="+lane+") "+vg+" = "+bits(a[1])+";");
            }
            return true;
        }
        if(op=="s_getpc_b64") {
            auto r=registers(a.at(0));check_range(r,'s',2);auto pc=i.pc+i.size;
            write(reg('s',r.first),std::format("{}u",pc),{Kind::code_lo,pc,pc,true});
            write(reg('s',r.first+1),"0u",{Kind::code_hi,pc,0,true});return true;
        }
        if(op=="s_add_u32" || op=="s_addc_u32"){
            const bool hi=op=="s_addc_u32";auto kind=hi?Kind::code_hi:Kind::code_lo;
            for(unsigned j=1;j<=2;++j)if(value(a.at(j)).kind==kind){
                auto v=value(a[j]);auto& other=a[3-j];auto rhs=bits(other);
                if(!uniform(other))unsupported("nonuniform scalar PC index");
                auto left=temp("uint",a[j]);auto sum=temp("uint",left+" + "+rhs);
                auto carry="("+sum+" < "+left+")";
                if(hi){auto v2=temp("uint",sum+" + uint("+scalar_condition()+")");carry="("+carry+" || "+v2+" < "+sum+")";sum=v2;}
                else if(!other.empty() && std::isdigit(static_cast<unsigned char>(other[0])) && !other.contains('.')){
                    auto off=v.part+number(other);if(off>=p.code.size())unsupported("PC-relative address outside shader allocation");
                    v.part=off;
                }
                write("scc","uint("+carry+")",scalar);write(a[0],sum,v);return true;
            }
        }
        if(op=="v_lshlrev_b64") {
            auto src=registers(a.at(2)),dst=registers(a.at(0));check_range(dst,'v',2);
            if(src.count!=2)unsupported("invalid 64-bit shift");
            auto t=temp("uint2","shl64(uint2("+bits(reg(src.bank,src.first))+","+bits(reg(src.bank,src.first+1))+"),"+bits(a[1])+")");
            write(reg('v',dst.first),t+".x",varying,true);write(reg('v',dst.first+1),t+".y",varying,true);return true;
        }
        if(p.stage==pixel && op=="v_bfe_u32" && value(a.at(1)).kind==Kind::ancillary &&
           a.at(2)=="16" && a.at(3)=="13") {
            write(a[0],std::format("input.r{} & 8191u",value(a[1]).id),varying,true);return true;
        }
        if (op=="s_mov_b32") {
            auto dst=a.at(0), src=a.at(1);
            if (dst=="null") return true;
            if (dst=="m0" && value(src).kind==Kind::prim) {set("m0_constant",{});set(dst,{Kind::prim});return true;}
            if(dst=="m0" || dst=="exec_lo" || dst=="exec_hi" || dst=="vcc_lo" || dst=="vcc_hi"){
                if(dst=="m0"){
                    if(!src.empty() && std::isdigit(static_cast<unsigned char>(src[0])) && !src.contains('.'))
                        set("m0_constant",{Kind::immediate,number(src),0,true});
                    else set("m0_constant",{});
                }
                scalar_dest(dst,bits(src));return true;}
            check_range(registers(dst),'s',1);
            auto v=value(src);
            if(raw_sgprs && (v.kind==Kind::table || v.kind==Kind::descriptor))write(dst,src,v);
            else if (v.kind==Kind::table || v.kind==Kind::descriptor || v.kind==Kind::prim || v.kind==Kind::opaque_word) set(dst,v);
            else {
                write(dst,bits(src),scalar);
                // A 32-bit table pointer is paired with a zero high dword.
                if (src=="0") state[dst].part=1;
                else if(v.kind==Kind::number) state[dst].part=v.part;
            }
            return true;
        }
        if (op=="v_readfirstlane_b32") {
            if (!uniform(a.at(1)) && !representatives.contains(a[1])) {
                if(pixel_wave_election){
                    // Scalar branches already elect/vote over the native
                    // graphics wave. Keep the same participants for a
                    // first-lane broadcast, including the guest EXEC filter.
                    // Lane numbers and numeric ballot words are not observable
                    // in this shader, and PSV specifies no fixed wave size.
                    if(masked){masked_pixel_election=true;scalar_dest(a[0],"ps_first_enabled("+bits(a[1])+",active)");}
                    else scalar_dest(a[0],"WaveReadLaneFirst("+bits(a[1])+")");
                    return true;
                }
                if(p.stage==pixel && p.flexible_pixel_wave && !masked && blocks.empty() && !loop_depth &&
                   value("flat_input_"+a[1]).kind==Kind::immediate){
                    // A provoking-vertex attribute is flat within a primitive,
                    // not necessarily throughout a pixel wave. Actually elect
                    // its first active lane. PSV permits any native wave width;
                    // this path never treats the attribute itself as uniform.
                    scalar_dest(a[0],"WaveReadLaneFirst("+bits(a[1])+")");return true;
                }
                if(!virtual_wave)unsupported("wave-size-dependent readfirstlane");
                if(physical_waves() && !masked)scalar_dest(a[0],"WaveReadLaneFirst("+bits(a[1])+")");
                else scalar_dest(a[0],std::format("group_first_value({},active,lane)",bits(a[1])));
                return true;
            }
            scalar_dest(a[0],representatives.contains(a[1])?"uniform_"+a[1]:bits(a[1]));return true;
        }
        if(op=="s_bcnt1_i32_b64" || op=="s_ff1_i32_b64") {
            if(!virtual_wave)unsupported("unmapped guest wave ballot");
            auto m=mask(a.at(1));
            write(a[0],std::format("group_reduce({},lane,{})",m,op=="s_bcnt1_i32_b64"?0:1),scalar);
            if(op=="s_bcnt1_i32_b64")write("scc",a[0]+" != 0u ? 1u : 0u",scalar);
            return true;
        }
        if(op=="v_mbcnt_lo_u32_b32" || op=="v_mbcnt_hi_u32_b32") {
            if(!virtual_wave)unsupported("unmapped guest wave prefix count");
            // SRC0 is a 32-bit word per invocation, not necessarily EXEC.
            // emit() materializes SGPR mask halves from the guest wave; a
            // VGPR may supply a different mask for each destination lane.
            bool hi=op=="v_mbcnt_hi_u32_b32";
            auto lower=hi?"(lane<32u ? 0u : ((1u<<(lane&31u))-1u))":
                          "(lane>=32u ? ~0u : ((1u<<(lane&31u))-1u))";
            auto n=temp("uint","countbits("+bits(a.at(1))+" & "+lower+")");
            write(a[0],n+" + "+bits(a.at(2)),varying,true);return true;
        }
        if(op=="v_add_co_u32" || op=="v_add_co_ci_u32") {
            auto numeric_kind=op=="v_add_co_u32"?Kind::code_lo:Kind::code_hi;
            auto v=varying;
            auto operand=[&](const Str& n){auto k=value(n);if(k.kind==numeric_kind){v=k;v.uniform=false;return n;}return bits(n);};
            auto lhs=temp("uint",operand(a.at(2))),sum=temp("uint",lhs+" + "+operand(a.at(3)));
            auto carry=temp("bool",sum+" < "+lhs);auto result=sum;
            if(op=="v_add_co_ci_u32") {
                result=temp("uint",sum+" + uint("+mask(a.at(4))+")");
                carry="("+carry+" || "+result+" < "+sum+")";
            }
            putmask(a.at(1),"active && "+carry);write(a[0],result,v,true);return true;
        }
        if (op=="s_cselect_b32") {
            if (value("scc").kind!=Kind::number) unsupported("unbound SCC");
            write(a.at(0),scalar_condition()+" ? "+bits(a.at(1))+" : "+bits(a.at(2)),scalar);return true;
        }
        if(op=="s_max_u32" || op=="s_min_u32" || op=="s_max_i32" || op=="s_min_i32") {
            auto x=bits(a.at(1)),y=bits(a.at(2));
            if(op.ends_with("i32")){x="asint("+x+")";y="asint("+y+")";}
            bool max=op.starts_with("s_max");
            write("scc",x+(max?" > ":" < ")+y+" ? 1u : 0u",scalar);
            write(a[0],std::format("asuint({}({},{}))",max?"max":"min",x,y),scalar);return true;
        }
        if(compare_alu(i) || scalar_alu(i) || half_alu(i) || vector_alu(i))return true;
        if (op=="v_cndmask_b32") {
            auto r=maskname(a.at(3));auto m=mask(a[3]);
            bool same=uniform(a[1]) && uniform(a[2]) && ((value(r).kind==Kind::predicate && value(r).uniform) || representatives.contains(r));
            if(same && representatives.contains(r))m="(uniform_"+r+" != 0u)";
            auto src=value(a[2]);auto yes=(src.kind==Kind::bounded_relative && value(r).mask_origin==src.id)?a[2]:bits(a[2]);
            write(a[0],m+" ? "+yes+" : "+bits(a[1]),same?scalar:varying,true);return true;
        }
        if(op=="v_cvt_f32_u32_sdwa") {
            auto mod=View(i.modifiers);
            constexpr View prefix="dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:";
            if(!mod.starts_with(prefix))unsupported("extended subword conversion");
            mod.remove_prefix(prefix.size());unsigned shift=0,width=0;
            if(mod=="WORD_0" || mod=="WORD_1") {shift=16*(mod.back()-'0');width=16;}
            else if(mod.size()==6 && mod.starts_with("BYTE_") && mod[5]>='0' && mod[5]<='3') {shift=8*(mod[5]-'0');width=8;}
            else unsupported("subword selector");
            write(a.at(0),std::format("asuint(float(({} >> {}) & {}u))",bits(a.at(1)),shift,(1u<<width)-1),
                uniform(a[1])?scalar:varying,true);return true;
        }
        if(op=="v_min_i32_sdwa" && i.modifiers=="dst_sel:BYTE_3 dst_unused:UNUSED_PAD src0_sel:DWORD src1_sel:DWORD") {
            write(a.at(0),"asuint(min(asint("+bits(a.at(1))+"),asint("+bits(a.at(2))+"))) << 24u",varying,true);return true;
        }
        if (!op.starts_with("v_") && !op.starts_with("s_")) return false;
        if (a.empty() || a[0].empty() || (a[0][0]!='s' && a[0][0]!='v')) return false;
        bool vec=op.front()=='v';
        // For lane-local arithmetic, a uniform representative is valid for
        // every executing lane and can feed another masked ALU definition.
        // Cross-lane transfers above continue to use the actual VGPR file.
        auto x=[&](unsigned n){return vec?active_bits(a.at(n)):bits(a.at(n));};
        auto f=[&](unsigned n){return "asfloat("+x(n)+")";};
        auto bin=[&](View o){return "("+x(1)+" "+Str(o)+" "+x(2)+")";};
        Str e,fp;
        if (op=="v_mov_b32") e=x(1);
        else if (op=="v_add_nc_u32" || op=="s_add_i32" || op=="s_add_u32") e=bin("+");
        else if (op=="v_sub_nc_u32" || op=="s_sub_i32" || op=="s_sub_u32") e=bin("-");
        else if (op=="v_subrev_nc_u32") e=x(2)+" - "+x(1);
        else if (op=="v_and_b32" || op=="s_and_b32") e=bin("&");
        else if (op=="s_andn2_b32") e=x(1)+" & ~"+x(2);
        else if (op=="v_or_b32" || op=="s_or_b32") e=bin("|");
        else if (op=="v_xor_b32" || op=="s_xor_b32") e=bin("^");
        else if (op=="v_not_b32" || op=="s_not_b32") e="~"+x(1);
        else if (op=="s_brev_b32" || op=="v_bfrev_b32") e="reversebits("+x(1)+")";
        else if (op=="v_bcnt_u32_b32") e="countbits("+x(1)+") + "+x(2);
        else if (op=="s_flbit_i32_b32") e=x(1)+" == 0u ? 0xffffffffu : 31u-firstbithigh("+x(1)+")";
        else if (op=="v_max_u32" || op=="v_min_u32") e=Str(op=="v_max_u32"?"max(":"min(")+x(1)+","+x(2)+")";
        else if (op=="v_and_or_b32") e=bin("&")+" | "+x(3);
        else if (op=="s_lshl_b32" || op=="s_lshr_b32") e=x(1)+(op=="s_lshl_b32"?" << ":" >> ")+"("+x(2)+" & 31u)";
        else if (op=="v_lshlrev_b32" || op=="v_lshrrev_b32") e=x(2)+(op=="v_lshlrev_b32"?" << ":" >> ")+"("+x(1)+" & 31u)";
        else if (op=="v_ashrrev_i32") e="asuint(asint("+x(2)+") >> ("+x(1)+" & 31u))";
        else if (op=="v_lshl_add_u32") e="("+x(1)+" << ("+x(2)+"&31u)) + "+x(3);
        else if (op=="v_lshl_or_b32") e="("+x(1)+" << ("+x(2)+"&31u)) | "+x(3);
        else if (op=="v_or3_b32") e=bin("|")+" | "+x(3);
        else if (op=="v_add3_u32") e=bin("+")+" + "+x(3);
        else if (op=="v_add_lshl_u32") e=bin("+")+" << ("+x(3)+" & 31u)";
        else if (op=="v_mul_lo_u32" || op=="s_mul_i32") e=bin("*");
        else if (op=="v_mul_u32_u24") e="("+x(1)+" & 0xffffffu) * ("+x(2)+" & 0xffffffu)";
        else if (op=="v_mul_hi_u32" || op=="s_mul_hi_u32") e="mulhi("+x(1)+","+x(2)+")";
        else if (op=="v_bfe_u32") e="("+x(1)+" >> ("+x(2)+"&31u)) & ((1u << ("+x(3)+"&31u))-1u)";
        else if (op=="s_bfe_u32") e="scalar_bfe("+x(1)+","+x(2)+")";
        else if (op=="v_cvt_f32_ubyte0" || op=="v_cvt_f32_ubyte1" || op=="v_cvt_f32_ubyte2" || op=="v_cvt_f32_ubyte3")
            fp=std::format("float(({} >> {}) & 255u)",x(1),8*(op.back()-'0'));
        else if (op=="v_cvt_f32_u32") fp="float("+x(1)+")";
        else if (op=="v_cvt_f32_i32") fp="float(asint("+x(1)+"))";
        else if (op=="v_cvt_u32_f32") e="cvt_u32("+f(1)+")";
        else if (op=="v_cvt_i32_f32") e="cvt_i32("+f(1)+")";
        else if (op=="v_min_i32" || op=="v_max_i32")
            e=Str(op.contains("min")?"asuint(min(asint(":"asuint(max(asint(")+x(1)+"),asint("+x(2)+")))";
        else if(op=="v_max3_i32" || op=="v_min3_i32" || op=="v_max3_u32" || op=="v_min3_u32") {
            Str fn=op.contains("max")?"max":"min";
            auto arg=[&](unsigned j){return op.ends_with("i32")?"asint("+x(j)+")":x(j);};
            e=std::format("asuint({0}({0}({1},{2}),{3}))",fn,arg(1),arg(2),arg(3));
        }
        else if (op=="v_cvt_pkrtz_f16_f32") e="half_rtz("+f(1)+") | (half_rtz("+f(2)+")<<16u)";
        else if(op=="v_cubesc_f32" || op=="v_cubetc_f32" || op=="v_cubema_f32" || op=="v_cubeid_f32") {
            char part=op=="v_cubesc_f32"?'x':op=="v_cubetc_f32"?'y':op=="v_cubema_f32"?'z':'w';
            fp=std::format("cube_alu({},{},{}).{}",f(1),f(2),f(3),part);
        }
        else if (op=="v_add_f32") fp=f(1)+" + "+f(2);
        else if (op=="v_sub_f32") fp=f(1)+" - "+f(2);
        else if (op=="v_subrev_f32") fp=f(2)+" - "+f(1);
        else if (op=="v_min_f32" || op=="v_max_f32") fp=Str(op=="v_max_f32"?"max(":"min(")+f(1)+","+f(2)+")";
        else if (op=="v_max3_f32" || op=="v_min3_f32") {
            Str fn=op=="v_max3_f32"?"max(":"min(";
            fp=fn+fn+f(1)+","+f(2)+"),"+f(3)+")";
        }
        else if (op=="v_med3_f32") fp="max(min("+f(1)+","+f(2)+"),min(max("+f(1)+","+f(2)+"),"+f(3)+"))";
        else if (op=="v_mul_f32") fp=f(1)+" * "+f(2);
        else if (op=="v_mul_legacy_f32") {
            bool optimized=native_math();
            native_mulz|=optimized;
            fp=Str(optimized?"xs_mulz(":"legacy_mul(")+f(1)+","+f(2)+")";
        }
        else if (op=="v_fma_f32" || op=="v_fmaak_f32" || op=="v_fmamk_f32") fp="mad("+f(1)+","+f(2)+","+f(3)+")";
        else if (op=="v_fmac_legacy_f32") fp="legacy_fma("+f(1)+","+f(2)+","+f(0)+")";
        else if (op=="v_fma_legacy_f32") fp="legacy_fma("+f(1)+","+f(2)+","+f(3)+")";
        else if (op=="v_fmac_f32") fp="mad("+f(1)+","+f(2)+","+f(0)+")";
        else if (op=="v_mad_f32" || op=="v_madak_f32") fp="unfused("+f(1)+","+f(2)+","+f(3)+")";
        else if (op=="v_mac_f32") fp="unfused("+f(1)+","+f(2)+","+f(0)+")";
        else if (op=="v_rcp_f32" || op=="v_rcp_iflag_f32") fp="rcp("+f(1)+")";
        else if (op=="v_sqrt_f32") fp="sqrt("+f(1)+")";
        else if (op=="v_rsq_f32") fp="rsqrt("+f(1)+")";
        else if (op=="v_log_f32") fp="log2("+f(1)+")";
        else if (op=="v_exp_f32") fp="exp2("+f(1)+")";
        else if (op=="v_floor_f32") fp="floor("+f(1)+")";
        else if (op=="v_ceil_f32") fp="ceil("+f(1)+")";
        else if (op=="v_rndne_f32") fp="round("+f(1)+")";
        else if (op=="v_ldexp_f32") fp="ldexp("+f(1)+",asint("+x(2)+"))";
        else if (op=="v_fract_f32") fp="frac("+f(1)+")";
        else if (op=="v_trunc_f32") fp="trunc("+f(1)+")";
        else if (op=="v_sin_f32" || op=="v_cos_f32") fp=Str(op=="v_sin_f32"?"sin(":"cos(")+f(1)+" * 6.283185307179586)";
        else return false;
        if(!i.modifiers.empty()) {
            if(fp.empty()) unsupported("integer arithmetic modifier");
            auto m=View(i.modifiers);
            bool clamp=m.starts_with("clamp");
            if(clamp) {m.remove_prefix(5);if(m.starts_with(' '))m.remove_prefix(1);}
            if(m.starts_with("div:2")) {fp="(("+fp+") * 0.5)";m.remove_prefix(5);}
            else if(m.starts_with("mul:2") || m.starts_with("mul:4")) {fp="(("+fp+") * "+Str(1,m[4])+".0)";m.remove_prefix(5);}
            if(m.starts_with(' ')) m.remove_prefix(1);
            if(m=="clamp") clamp=true;
            else if(!m.empty()) unsupported("arithmetic modifier");
            if(clamp) fp="saturate("+fp+")";
        }
        if (!fp.empty()) e="asuint("+fp+")";
        bool uni=std::all_of(a.begin()+1,a.end(),[&](auto& a){return vec?active_uniform(a):uniform(a);});
        if(op=="v_fmac_f32" || op=="v_mac_f32" || op=="v_fmac_legacy_f32") uni &= active_uniform(a[0]);
        if(!vec && (a[0]=="vcc_lo" || a[0]=="vcc_hi")){scalar_dest(a[0],e);return true;}
        check_range(registers(a[0]),vec?'v':'s',1);
        // Flag-producing SALU instructions are handled by scalar_alu above.
        // BREV and MUL here leave the architectural SCC untouched.
        write(a[0],e,uni?scalar:varying,vec);return true;
    }

    unsigned descriptor(const Str& a) {
        auto r=registers(a); auto v=value(reg('s',r.first));
        if (r.bank!='s' || v.kind!=Kind::descriptor || r.count!=p.resources.at(v.id).words)
            unsupported("unresolved resource descriptor");
        for (unsigned k=0;k<r.count;++k)
            if (value(reg('s',r.first+k))!=Value{Kind::descriptor,v.id,k})
                unsupported("modified descriptor words");
        used.insert(v.id);
        if(check_buffer_inputs)resolved_buffers.insert(emit_pc);
        if((selective_vm || check_buffer_inputs) && p.resources[v.id].type>=6)vm_access.insert(emit_pc);
        return v.id;
    }
    bool image_descriptor_alu(const Instruction& i) {
        // Instructions which inspect the image SRD's extent and base mip are
        // common in GetDimensions lowering. The dimensions of the backing
        // image can differ from those of the view, especially for odd sizes.
        // Specialize only fields whose bits are specified by the desktop API.
        const auto& a=i.args;
        if(a.size()!=3 || (i.op!="s_lshr_b32" && i.op!="s_lshl_b32" && i.op!="s_bfe_u32" && i.op!="s_and_b32"))return false;
        auto d=value(a[1]);if(d.kind!=Kind::descriptor || a[2].empty() || !std::isdigit(static_cast<unsigned char>(a[2][0])))return false;
        auto& rv=p.resources.at(d.id);if(rv.type!=3 || (rv.kind!=2 && rv.kind!=4 && rv.kind!=5 && rv.kind!=7))return false;
        unsigned imm=number(a[2]),shift=imm&31u,need=0;
        if(i.op=="s_bfe_u32"){auto width=(imm>>16)&127u;need=(width>=32?~0u:((1u<<width)-1u))<<shift;}
        else if(i.op=="s_lshr_b32")need=~0u<<shift;
        else if(i.op=="s_lshl_b32")need=~0u>>shift;
        else need=imm;
        auto field=[&](unsigned k){
            image_fields.emplace(d.id,k);return std::format("XS_FIELD_{}_{}",d.id,k);
        };
        auto extent=[&](unsigned k){return "("+field(k)+" | ("+field(k+1)+" << 8u))";};
        Str word;
        if(d.part==1 && !(need&0x3fffffffu))word="("+extent(view::width_lo)+" << 30u)";
        else if(d.part==2) {
            word="0x80000000u";
            if(need&0x3fffu)word+=" | ("+extent(view::width_lo)+" >> 2u)";
            if(need&0x3fffc000u)word+=" | ("+extent(view::height_lo)+" << 14u)";
        }else if(d.part==3 && !(need&~0xf000u))word="("+field(view::base_mip)+" << 12u)";
        else return false;
        auto x="("+word+")";Str result=i.op=="s_lshr_b32"?x+" >> "+std::to_string(shift)+"u":
            i.op=="s_lshl_b32"?x+" << "+std::to_string(shift)+"u":
            i.op=="s_and_b32"?x+" & "+std::to_string(imm)+"u":std::format("scalar_bfe({},{}u)",x,imm);
        auto out=temp("uint",result);scalar_dest(a[0],out);write("scc","uint("+out+" != 0u)",scalar);return true;
    }
    bool smem(const Instruction& i) {
        auto& a=i.args;auto op=View(i.op);
        bool cb=op.starts_with("s_buffer_load_dword");
        if (!cb && !op.starts_with("s_load_dword")) return false;
        if (!i.modifiers.empty() || a.size()!=3) unsupported("extended scalar load");
        unsigned n=lane_count(op,cb?"s_buffer_load_dword":"s_load_dword");
        bool vcc=a[0]=="vcc" || a[0]=="vcc_lo" || a[0]=="vcc_hi";
        Range dst{};
        if(vcc){if(n!=(a[0]=="vcc"?2u:1u))unsupported("invalid VCC load width");}
        else{dst=registers(a[0]);check_range(dst,'s',n);}
        auto output=[&](unsigned k,const Str& e){if(vcc)scalar_dest(a[0]=="vcc"?(k?"vcc_hi":"vcc_lo"):a[0],e);
            else write(reg('s',dst.first+k),e,scalar);};
        if (cb) {
            auto id=descriptor(a[1]);auto& r=p.resources[id];
            if((r.type==4 || r.type==7) && r.kind==11){
                if(!uniform(a[2]))unsupported("nonuniform scalar buffer address");
                auto addr=temp("uint",bits(a[2]));
                for(unsigned k=0;k<n;++k)output(k,std::format("res{}.Load({}+{}u)",id,addr,4*k));
                return true;
            }
            if(r.type==5 && r.kind==12 && r.stride && !(r.stride%4) && r.stride<=2048) {
                if(!uniform(a[2]))unsupported("nonuniform scalar load");
                auto at=temp("uint","("+bits(a[2])+" >> 2)");
                for(unsigned k=0;k<n;++k) {
                    auto word=std::format("({}+{}u)",at,k);
                    output(k,std::format("res{}[{} / {}u].w[{} % {}u]",id,word,r.stride/4,word,r.stride/4));
                }
                return true;
            }
            if(r.type!=2 || r.kind!=13)unsupported("scalar load from an unsupported buffer view");
            if(a[2].starts_with('s') || a[2]=="vcc_lo" || a[2]=="vcc_hi"){
                if(!uniform(a[2]))unsupported("nonuniform CBV address");
                auto base=temp("uint",bits(a[2])+" >> 2");
                cb_end[id]=16384; // maximum D3D CBV span; the bound descriptor gives the actual extent
                for(unsigned k=0;k<n;++k) {
                    auto at=std::format("({}+{}u)",base,k);
                    output(k,std::format("cb{}[{} >> 2][{} & 3u]",r.reg,at,at));
                }
                return true;
            }
            unsigned off=a[2]=="null"?0:number(a[2]);
            if(off%4 || off>65536-4*n) unsupported("unaligned or out-of-range CBV load");
            cb_end[id]=std::max(cb_end[id],off/4+n);
            for(unsigned k=0;k<n;++k)
                output(k,std::format("cb{}[{}].{}",r.reg,(off/4+k)/4,xyzw[(off/4+k)%4]));
        } else {
            auto base=registers(a[1]); check_range(base,'s',2);
            auto lo=value(reg('s',base.first)),hi=value(reg('s',base.first+1));
            if(lo.kind==Kind::code_lo && hi.kind==Kind::code_hi && lo.id==hi.id){
                if(pool_start==~0u){auto g=decode_graph(p.code);const auto& last=g.ins.rbegin()->second;pool_start=last.pc+last.size;}
                if(lo.part<pool_start || !uniform(a[2]))unsupported("unbounded scalar literal-table base");
                auto lower=temp("uint",reg('s',base.first));auto upper=temp("uint",reg('s',base.first+1));
                auto addr=temp("uint",lower+" + "+bits(a[2]));auto high=temp("uint",upper+" + uint("+addr+" < "+lower+")");
                for(unsigned j=0;j<n;++j){
                    auto at=temp("uint",std::format("{}+{}u",addr,j*4));
                    output(j,std::format("literal_load({0},{1}+uint({0}<{2}))",at,high,addr));
                }
                return true;
            }
            unsigned off=a[2]=="null"?0:number(a[2]);
            if(vcc)unsupported("descriptor loaded into VCC without numeric field provenance");
            if (lo.kind!=Kind::table || hi.kind!=Kind::number || hi.part!=1) unsupported("dynamic descriptor address");
            auto high=reg('s',base.first+1);
            for(auto& u:accesses)if(!u.defined.contains(high))u.zero_high.insert(high);
            auto it=std::ranges::find_if(p.resources,[&](auto& r){return r.table==lo.id && r.offset==off && r.words==n;});
            if (it==p.resources.end()) unsupported("unmapped descriptor offset/width");
            unsigned id=it-p.resources.begin();
            for(unsigned k=0;k<n;++k){
                auto word=reg('s',dst.first+k);
                if(ray_mode && p.resources[id].kind==16 && p.resources[id].type==4){
                    set(word,{Kind::ray_address,id,k,true});continue;
                }
                set(word,{Kind::descriptor,id,k});
                if(raw_sgprs){
                    unsigned source=8*id+k;raw_writes.insert(source);vars.insert(word);
                    line(std::format("XS_SRD_WRITE_{}({});",source,word));
                }
            }
        }
        return true;
    }
    Str vector_bits(Range r) const {
        if (r.bank!='v' || r.count>4) unsupported("unsupported data vector");
        Str v="uint"+suffix(r.count)+"(";
        for(unsigned k=0;k<r.count;++k) v+=(k?",":"")+bits(reg('v',r.first+k));
        return v+")";
    }
    void load_values(Range dst,Str expr,bool same=false) {
        if(dst.bank!='v' || dst.count>4) unsupported("unsupported load destination");
        // Materialize once before overwriting any aliased address VGPR.
        if(masked && !same) line("if (active) {");
        auto t=temp("uint"+suffix(dst.count),std::move(expr));
        for(unsigned k=0;k<dst.count;++k)
            write(reg('v',dst.first+k),t+(dst.count==1?"":Str(".")+xyzw[k]),same?scalar:varying,same);
        if(masked && !same) line("}");
    }
    void store(const Str& s) { line((masked?"if (active) ":"")+s+";"); }
    void view_width(unsigned id,unsigned n,bool exact) {
        auto [it,added]=view_widths.emplace(id,n);
        if(!added){
            if((fixed_widths.contains(id) && (n>it->second || (exact && n!=it->second))) || (exact && it->second>n))
                unsupported("incompatible read/write widths on a typed view");
            it->second=std::max(it->second,n);
        }
        if(exact)fixed_widths.insert(id);
    }
    Str mubuf_addressing(const Instruction& i,const Resource& resource){
        Str addressing,token;std::istringstream modifiers(i.modifiers);
        while(modifiers>>token){
            if(token=="glc" || token=="dlc" || token=="slc"){
                if(resource.type==6 || resource.type==7 || resource.type==8)device_sync=vm_wait=true;
            }else{if(!addressing.empty())addressing+=' ';addressing+=token;}
        }
        return addressing;
    }
    void prepare_device_sync(const Graph& graph){
        if(p.stage==compute && std::ranges::any_of(graph.ins,[](auto& e){auto& op=e.second.op;
            return op.starts_with("buffer_atomic_") || op.starts_with("image_atomic_") ||
                   op=="buffer_gl0_inv" || op=="buffer_gl1_inv";}))device_sync=vm_wait=true;
    }
    void resolve_vm_waits(const Graph& graph,unsigned start){
        if(vm_waits.empty())return;
        std::map<unsigned,bool> incoming{{start,false}};std::deque<unsigned> work{start};
        while(!work.empty()){
            auto head=work.front();work.pop_front();bool dirty=incoming.at(head);
            for(auto it=graph.ins.find(head);it!=graph.ins.end();++it){
                const auto& [pc,i]=*it;
                if(vm_waits.contains(pc)){if(dirty)vm_required.insert(pc);dirty=false;}
                // Read-only SRV/CBV loads already have SSA data dependencies.
                // Keep the device fence if *any* path can carry UAV traffic
                // (including reads), a raw global load, or a coherent access.
                if(vm_access.contains(pc) || i.op.starts_with("global_") || i.op.starts_with("flat_") ||
                   i.modifiers.contains("glc") || i.modifiers.contains("slc") || i.modifiers.contains("dlc"))dirty=true;
                if(i.op=="s_waitcnt_vscnt" || i.op=="buffer_gl0_inv" || i.op=="buffer_gl1_inv" ||
                   i.op=="s_dcache_inv" || i.op=="s_gl1_inv")dirty=false;
                if(i.op=="s_branch" || i.op.starts_with("s_cbranch_") || i.op=="s_endpgm" || graph.edges.contains(pc+i.size))break;
            }
            for(auto to:graph.edges.at(head)){auto [it,new_head]=incoming.try_emplace(to,dirty);
                if(new_head || (dirty && !it->second)){it->second=dirty;work.push_back(to);}}
        }
    }
    bool memory(const Instruction& i) {
        auto& a=i.args; auto op=View(i.op);
        if(op.starts_with("buffer_") && subword_buffer(i))return true;
        if(op.starts_with("ds_") && lds_io(i))return true;
        if(op=="ds_nop" && a.empty() && i.modifiers.empty())return true;
        if(op.starts_with("ds_") && wide_lds_atomic(i))return true;
        if(op.starts_with("global_load_dword")) {
            if(a.size()!=3 || a[2]!="off" || !i.modifiers.empty())unsupported("non-literal global load");
            auto address=registers(a[1]);check_range(address,'v',2);auto lo=reg('v',address.first),hi=reg('v',address.first+1);
            auto low=value(lo),high=value(hi);
            if(low.kind!=Kind::code_lo || high.kind!=Kind::code_hi || low.id!=high.id)unsupported("global address not based on shader PC");
            if(pool_start==~0u) {
                auto g=decode_graph(p.code);auto& last=g.ins.rbegin()->second;pool_start=last.pc+last.size;
            }
            if(low.part<pool_start)unsupported("literal overlaps executable code");
            unsigned count=lane_count(op,"global_load_dword");auto dst=registers(a[0]);check_range(dst,'v',count);
            Str vals;for(unsigned j=0;j<count;++j) vals+=(j?",":"")+std::format("literal_load({}+{}u,{})",lo,4*j,hi);
            load_values(dst,count==1?vals:"uint"+suffix(count)+"("+vals+")");return true;
        }
        if(op.starts_with("buffer_atomic_")) {
            if(a.size()!=4)unsupported("atomic buffer operand count");
            auto id=descriptor(a[2]);auto& r=p.resources[id];Str index="0u",bytes=bits(a[3]);
            bool idx=false,voff=false,ret=false;std::istringstream m(i.modifiers);Str t;
            while(m>>t){
                if(t=="idxen")idx=true;else if(t=="offen")voff=true;else if(t=="glc")ret=true;
                else if(t.starts_with("offset:"))bytes+=" + "+std::to_string(number(View(t).substr(7)))+"u";
                else unsupported("atomic MUBUF modifier");
            }
            if(idx||voff){
                auto v=registers(a[1]);check_range(v,'v',unsigned(idx)+unsigned(voff));
                if(idx)index=bits(reg('v',v.first));
                if(voff)bytes+=" + "+bits(reg('v',v.first+unsigned(idx)));
            }else if(a[1]!="off")unsupported("unmapped atomic address");
            Str addr,raw;
            if(r.type==8 && r.kind==12 && r.stride && !(r.stride%4)){
                addr=std::format("res{}[{}].w[({})>>2u]",id,index,bytes);
            }else if(r.type==7 && r.kind==11 && !idx){addr=bytes;raw=std::format("res{}.",id);}
            else if(r.type==6 && r.kind==10 && r.stride==4 && bytes=="0u"){
                addr=std::format("res{}[{}]",id,index);
            }else unsupported("atomic buffer layout");
            return atomic_op(op.substr(14),addr,raw,r.type==6?id:~0u,registers(a[0]),ret);
        }
        if(op.starts_with("ds_") && (op.ends_with("_u32")||op.ends_with("_i32")||op.ends_with("_b32")||op.ends_with("_f32")) &&
           !op.starts_with("ds_read") && !op.starts_with("ds_write") && op!="ds_swizzle_b32" &&
           op!="ds_permute_b32" && op!="ds_bpermute_b32") {
            bool ret=op.contains("_rtn_");
            auto name=op.substr(3,op.find('_',3)-3);bool cas=name=="cmpst",extra=name=="mskor"||name=="wrap";
            if(!virtual_wave || !p.lds_bytes || a.size()!=2u+ret+(cas||extra))unsupported("unmapped LDS atomic");
            auto mod=View(i.modifiers);unsigned off=0;
            if(!mod.empty()){if(!mod.starts_with("offset:"))unsupported("LDS atomic modifier");off=number(mod.substr(7));}
            Str operation=cas?"cmpswap":name=="wrxchg"?"swap":Str(name);
            if(name=="min"||name=="max")operation=Str(1,op[op.rfind('_')+1])+Str(name);
            if(name=="add" && op.ends_with("_f32"))operation="fadd";
            if(operation=="imin"||operation=="imax")operation[0]='s';
            if(cas && op.ends_with("f32"))operation="fcmpswap";
            auto addr=temp("uint",bits(a[ret]));
            begin_lds(false);
            atomic32(operation,std::format("lds[({}+{}u)>>2]",addr,off),"",~0u,bits(a[1+ret+cas]),
                cas?bits(a[1+ret]):extra?bits(a[2+ret]):"",ret?a[0]:"");
            end_lds();
            return true;
        }
        if(op=="ds_swizzle_b32") {
            unsigned code=p.code.at<u32>(i.pc)&0xffffu;
            if(code>=0xc000)unsupported("rotate/FFT lane swizzle");
            unsigned an=code&31,orr=(code>>5)&31,xor_=(code>>10)&31;
            auto select=[&](unsigned l){return code&0x8000?(l&~3u)|((code>>(2*(l&3)))&3):((l&an)|orr)^xor_;};
            auto v="(active ? "+bits(a.at(1))+" : 0u)";Str e;
            bool quad=true;for(unsigned l=0;l<32;++l)quad &= select(l)/4==l/4;
            if((p.stage==pixel || physical_waves()) && quad){
                // Preserve guest quad lane selection; expose broadcasts and
                // xor permutations as native constant quad operations.
                auto target=select(0)&3u;bool broadcast=true,exchange=true;
                for(unsigned i=0;i<32;++i){broadcast&=(select(i)&3)==target;exchange&=select(i)==(i^target);}
                if(broadcast)e=std::format("QuadReadLaneAt({},{}u)",v,target);
                else if(exchange)e=target?Str(target==1?"QuadReadAcrossX(":target==2?"QuadReadAcrossY(":"QuadReadAcrossDiagonal(")+v+")":v;
                else{
                Str values;for(unsigned j=0;j<4;++j)values+=std::format("{}QuadReadLaneAt({},{})",j?",":"",v,select(j));
                e="uint4("+values+")[WaveGetLaneIndex() & 3u]";
                }
            }else if(virtual_wave){
                auto lane=code&0x8000?std::format("(lane & ~3u) | (({}u >> (2u*(lane&3u))) & 3u)",code):
                    std::format("(lane & ~31u) | (((lane & {}u) | {}u) ^ {}u)",an,orr,xor_);
                e="group_lane("+v+","+lane+",lane)";
            }else unsupported("cross-quad graphics swizzle needs fixed wave width");
            auto t=temp("uint",e);write(a.at(0),t,varying,true);return true;
        }
        if(op=="ds_bpermute_b32"||op=="ds_permute_b32"){
            if(!virtual_wave)unsupported("lane permutation requires guest wave size");
            auto off=p.code.at<u32>(i.pc)&0xffffu;
            auto target=std::format("((({}+{}u)>>2u)&{}u)",bits(a.at(1)),off,p.wave-1);
            auto t=temp("uint",op=="ds_bpermute_b32"?"group_lane(active?"+bits(a.at(2))+":0u,"+target+",lane)":
                        "group_scatter("+bits(a.at(2))+","+target+",active,lane)");
            write(a.at(0),t,varying,true);return true;
        }
        if(op.starts_with("buffer_load_format_")) {
            auto mask=op.substr(View("buffer_load_format_").size());
            if(mask.empty() || !xyzw.starts_with(mask) || a.size()!=4 || a[3]!="0")
                unsupported("extended typed buffer load");
            auto id=descriptor(a[2]);const auto& r=p.resources[id];auto dst=registers(a[0]);
            if((r.type!=3 && r.type!=6) || r.kind!=10) unsupported("incompatible typed buffer load");
            bool off=a[1]=="off";
            if(mubuf_addressing(i,r)!=(off?"":"idxen"))unsupported("extended typed buffer load addressing");
            check_range(dst,'v',mask.size());view_width(id,mask.size(),false);
            load_values(dst,std::format("asuint(res{}.Load({}).{})",id,off?"0u":bits(a[1]),mask));return true;
        }
        if ((op.starts_with("buffer_store_format_") && xyzw.starts_with(op.substr(20))) || op.starts_with("buffer_store_dword") || op.starts_with("buffer_load_dword")) {
            if (a.size()!=4) unsupported("MUBUF operand count");
            auto id=descriptor(a[2]);auto& r=p.resources[id];auto data=registers(a[0]);
            bool format=op.starts_with("buffer_store_format_"),load=op.starts_with("buffer_load");
            unsigned n=format?unsigned(op.substr(View("buffer_store_format_").size()).size()):lane_count(op,load?"buffer_load_dword":"buffer_store_dword");
            check_range(data,'v',n);
            auto addressing=mubuf_addressing(i,r);
            if(load && r.type==2 && r.kind==13 && n<=4){
                // Constant-buffer indices designate 16-byte HLSL rows; OFFEN
                // and both immediate and scalar offsets are byte quantities.
                std::istringstream m(addressing);Str t;bool idx=false,voff=false;unsigned off=0;
                while(m>>t){if(t=="idxen")idx=true;else if(t=="offen")voff=true;else if(t.starts_with("offset:"))off=number(View(t).substr(7));
                    else unsupported("CBV vector load modifier");}
                auto addr=bits(a[3])+" + "+std::to_string(off)+"u";
                if(idx||voff){auto v=registers(a[1]);check_range(v,'v',unsigned(idx)+unsigned(voff));
                    if(idx)addr+=" + 16u * "+bits(reg('v',v.first));
                    if(voff)addr+=" + "+bits(reg('v',v.first+unsigned(idx)));
                }else if(a[1]!="off")unsupported("CBV vector address");
                cb_end[id]=16384;auto word=temp("uint","("+addr+") >> 2u");Str e;
                for(unsigned k=0;k<n;++k)e+=std::format("{}cb{}[({}+{}u)>>2u][({}+{}u)&3u]",k?",":"",r.reg,word,k,word,k);
                load_values(data,n==1?e:"uint"+suffix(n)+"("+e+")");return true;
            }
            if(!format && r.kind==12 && (r.type==8 || (r.type==5 && load))) {
                auto mod=View(addressing); unsigned off=0;
                bool indexed=a[1]!="off";
                if(indexed && mod.starts_with("idxen offset:"))off=number(mod.substr(13));
                else if(!indexed && mod.starts_with("offset:"))off=number(mod.substr(7));
                else if(mod!=(indexed?"idxen":""))unsupported("extended structured buffer addressing");
                if(a[3]!="0" || !r.stride || r.stride>2048 || r.stride%4 || off%4 || u64(off)+4*n>r.stride || n>4)
                    unsupported("structured access outside one element");
                auto index=indexed?bits(a[1]):"0u";Str expr;
                for(unsigned k=0;k<n;++k) {
                    auto field=std::format("res{}[{}].w[{}]",id,index,off/4+k);
                    if(load)expr+=(k?",":"")+field;
                    else store(field+" = "+bits(reg('v',data.first+k)));
                }
                if(load)load_values(data,n==1?expr:"uint"+suffix(n)+"("+expr+")");
                return true;
            }
            bool off=a[1]=="off";
            auto mod=View(addressing);unsigned imm=0;
            if(auto pos=mod.find("offset:");pos!=mod.npos){imm=number(mod.substr(pos+7));mod=mod.substr(0,pos);if(mod.ends_with(' '))mod.remove_suffix(1);}
            if (n>4 || (off?!mod.empty():(format?mod!="idxen":mod!="offen"))) unsupported("extended buffer addressing");
            auto addr=off?"0u":bits(a[1]),name=std::format("res{}",id);
            if(format) {
                if (a[3]!="0" || imm || r.type!=6 || r.kind!=10) unsupported("incompatible typed buffer store");
                view_width(id,n,true);
                store(std::format("{}[{}] = BITS{}({})",name,addr,id,vector_bits(data)));
            } else {
                if ((r.type!=7 && !(load && r.type==4)) || r.kind!=11) unsupported("non-raw MUBUF access");
                addr=std::format("({}+{}+{}u)",addr,bits(a[3]),imm);
                if(load) load_values(data,name+".Load"+suffix(n)+"("+addr+")");
                else store(name+".Store"+suffix(n)+"("+addr+","+vector_bits(data)+")");
            }
            return true;
        }
        bool offset=op.ends_with("_o");
        if(offset)op.remove_suffix(2);
        bool cmp=op=="image_sample_c" || op=="image_sample_c_lz",grad=op=="image_sample_d";
        bool gather_l=op=="image_gather4_l";
        bool gather=gather_l || op=="image_gather4_lz" || op=="image_gather4",bias=op=="image_sample_b";
        bool mip=op=="image_load_mip",load_op=op=="image_load" || mip;
        bool atomic=op.starts_with("image_atomic_");
        bool store_op=op=="image_store" || atomic;
        if (!gather && !load_op && !bias && !cmp && !grad && op!="image_sample" && op!="image_sample_l" && op!="image_sample_lz" && !store_op) return false;
        if (a.size()!=((store_op || load_op)?3u:4u)) unsupported("extended image operands");
        auto mod=View(i.modifiers);bool volume=mod.contains(" dim:SQ_RSRC_IMG_3D"),cube=mod.contains(" dim:SQ_RSRC_IMG_CUBE");
        bool array=mod.contains(" dim:SQ_RSRC_IMG_2D_ARRAY"),ms=mod.contains(" dim:SQ_RSRC_IMG_2D_MSAA");
        View dim=cube?" dim:SQ_RSRC_IMG_CUBE":volume?" dim:SQ_RSRC_IMG_3D":array?" dim:SQ_RSRC_IMG_2D_ARRAY":ms?" dim:SQ_RSRC_IMG_2D_MSAA":" dim:SQ_RSRC_IMG_2D";
        if(!mod.starts_with("dmask:") || !mod.contains(dim) || (volume && gather)) unsupported("image dimension/modifiers");
        auto pos=mod.find(dim);
        auto mask=number(mod.substr(6,pos-6));
        auto flags=mod.substr(pos+dim.size());
        bool d16=flags.ends_with(" d16");if(d16)flags.remove_suffix(4);
        bool glc=flags.ends_with(" glc");
        if(glc)flags.remove_suffix(4);
        bool a16=flags.ends_with(" a16");
        if(a16)flags.remove_suffix(4);
        if (!mask || mask>15 || flags!=((store_op || load_op)?" unorm":"") ||
           (a16 && (offset || bias || mip || grad || cmp || array || ms || op=="image_sample_l")) || (d16 && atomic))
            unsupported("extended image modifiers");
        if(gather && !std::has_single_bit(mask))unsupported("multiple gather channels");
        unsigned channels=gather?4:std::popcount(mask);
        auto data=registers(a[0]);check_range(data,'v',d16?(channels+1)/2:channels);
        if(cube && (load_op || store_op || offset || gather || a16))unsupported("extended cube access");
        if(ms && (!load_op || mip))unsupported("multisampled operation other than load");
        if(offset && (volume || load_op || store_op))unsupported("extended volume/load offset");
        unsigned dims=volume||cube||array?3:2,spatial=volume?3:2;
        unsigned first=unsigned(offset)+unsigned(bias)+unsigned(cmp)+(grad?2*spatial:0);
        unsigned naddr=dims+(op=="image_sample_l" || gather_l || mip || ms)+first;
        unsigned packed=a16?(naddr+1)/2:naddr;
        std::vector<Str> addr,names;
        // A constant assigned under EXEC is still constant for every lane
        // executing this MIMG. Expose its unpredicated representative to DXC
        // so SM6.0 texture offsets remain immediates after SSA construction.
        auto coord=[&](const Str& n){names.push_back(n);return representatives.contains(n)?"uniform_"+n:bits(n);};
        if(a[1].starts_with('[') && a[1].ends_with(']')) {
            auto list=View(a[1]).substr(1,a[1].size()-2);
            for (;;) {
                auto pos=list.find(',');auto name=list.substr(0,pos);
                check_range(registers(name),'v',1);addr.push_back(coord(Str(name)));
                if(pos==list.npos)break;
                list.remove_prefix(pos+1);if(list.starts_with(' '))list.remove_prefix(1);
            }
        } else {
            auto coords=registers(a[1]);check_range(coords,'v',packed);
            for(unsigned j=0;j<packed;++j)addr.push_back(coord(reg('v',coords.first+j)));
        }
        if(addr.size()!=packed) unsupported("incorrect image address count");
        if(a16){
            auto words=std::move(addr);addr.clear();
            for(unsigned j=0;j<naddr;++j) {
                auto c="(("+words[j/2]+" >> "+std::to_string((j%2)*16)+"u) & 65535u)";
                addr.push_back(store_op||load_op?c:"asuint(f16tof32("+c+"))");
            }
        }
        Str position="uint"+suffix(dims)+"("+addr[first]+","+addr[1+first]+(dims==3?","+addr[2+first]:"")+")";
        auto id=descriptor(a[2]);auto& r=p.resources[id];
        auto name=std::format("res{}",id);
        bool cube_array=r.kind==9 && r.type==3 && (array || cube);
        if(cube_array && (offset || a16 || !(op=="image_sample_l" || op=="image_sample_lz" ||
                                            op=="image_sample" || bias || grad)))
            unsupported("extended cube-array sample");
        if (!cube_array && r.kind!=(cube?5u:volume?4u:array?7u:ms?3u:2u)) unsupported("image instruction/resource mismatch");
        if(glc && !atomic){
            if(r.type!=6 || (!store_op && !load_op))unsupported("coherent non-UAV image access");
            // D3D12 UAV visibility across groups, with the guest's VM wait
            // ordering subsequent accesses. GLC on an atomic means return-old.
            device_sync=true;vm_wait=true;
        }
        if(d16)half_images.insert(id);
        // A Load of the same texel in an immutable SRV has one value
        // throughout EXEC. coord() already supplies the uniform source
        // representative, including when the packed A16 word is masked.
        bool same_read=load_op && r.type==3 && std::ranges::all_of(names,[&](const Str& n){return active_uniform(n);});
        auto image_result=[&](Str expr,bool quad_read=false){
            if(quad_read && masked){
                // DXC may sink even a precomputed ddx/ddy into a varying
                // if(active) around its only consumer. Sample the immutable
                // SRV across the reconverged quad and select only the VGPR
                // writes. Keep the prior bits in all inactive lanes.
                // A completely disabled quad must not issue a texture read.
                line("if ("+quad_any("active")+") {");
                auto t=temp("uint"+suffix(channels),expr);
                for(unsigned j=0;j<data.count;++j){
                    auto elem=[&](unsigned k){return t+(channels>1?"."+Str(1,xyzw[k]):"");};
                    auto pack=[&](unsigned k){return std::format("PACK16_{}({})",id,elem(k));};
                    auto src=d16?pack(2*j)+(2*j+1<channels?" | ("+pack(2*j+1)+" << 16u)":""):elem(j);
                    auto dst=reg('v',data.first+j);
                    write(dst,"active ? ("+src+") : "+dst,varying);
                }
                line("}");
                return;
            }
            if(!d16){load_values(data,expr,same_read);return;}
            if(masked && !same_read)line("if(active){");
            auto words=temp("uint"+suffix(channels),expr);
            auto pack=[&](unsigned j){return std::format("PACK16_{}({}{})",id,words,channels>1?"."+Str(1,xyzw[j]):"");};
            for(unsigned j=0;j<data.count;++j)write(reg('v',data.first+j),pack(j*2)+(j*2+1<channels?" | ("+pack(j*2+1)+" << 16u)":""),same_read?scalar:varying,same_read);
            if(masked && !same_read)line("}");
        };
        if(atomic) {
            if(r.type!=6)unsupported("atomic requires an image UAV");
            bool cas=op=="image_atomic_cmpswap";
            if(mask==(cas?15u:3u)){
                // GFX10 MIMG: DMASK 3 (15 for CMPSWAP) is a *single* 64-bit
                // atomic. Two independent 32-bit maxima corrupt Nanite's
                // depth/visibility pair when different fragments win.
                auto fn=op.substr(13);
                Str hlsl=fn=="umax"?"Max":fn=="umin"?"Min":fn=="add"||fn=="sub"?"Add":
                         fn=="and"?"And":fn=="or"?"Or":fn=="xor"?"Xor":fn=="swap"?"Exchange":
                         cas?"CompareExchange":"";
                if(hlsl.empty())unsupported("unsupported 64-bit image atomic "+Str(fn));
                view_width(id,2,true);atomic64_images.insert(id);
                Str v="rd_u64("+vector_bits({'v',data.first,2})+")";
                if(fn=="sub")v="(uint64_t(0)-"+v+")";
                if(masked)line("if(active){");
                auto old=temp("uint64_t","0"); // snapshot operands before overwriting VDATA
                Str args=cas?"rd_u64("+vector_bits({'v',data.first+2,2})+"),"+v:v;
                line(std::format("Interlocked{}({}[{}],{},{});",hlsl,name,position,args,old));
                if(glc) {
                    write(reg('v',data.first),"uint("+old+")",varying);
                    write(reg('v',data.first+1),"uint("+old+">>32u)",varying);
                }
                if(masked)line("}");
                return true;
            }
            if(mask!=(cas?3u:1u))unsupported("invalid image atomic dmask");
            return atomic_op(op.substr(13),std::format("res{}[{}]",id,position),"",id,data,glc);
        } else if (store_op) {
            if (r.type!=6 || (mask&(mask+1))) unsupported("non-contiguous image store");
            view_width(id,channels,true);
            Str v;
            if(d16){
                v="uint"+suffix(channels)+"(";
                for(unsigned j=0;j<channels;++j)v+=std::format("{}UNPACK16_{}(({} >> {}u) & 65535u)",j?",":"",id,bits(reg('v',data.first+j/2)),16*(j%2));
                v+=")";
            }else v=vector_bits(data);
            store(std::format("UAV_STORE{}({},{})",id,position,v));
        } else if(load_op) {
            Str swizzle;for(unsigned k=0;k<4;++k)if(mask&(1u<<k))swizzle+=xyzw[k];
            if(r.type==3){
                loaded_textures.insert(id);
                if(ms)image_result(std::format("asuint({}.Load(int2({}),{}).{})",name,position,addr[dims],swizzle));
                else image_result(std::format("asuint({}.Load(int{}({},{})).{})",name,dims+1,position,mip?addr[dims]:"0",swizzle));
            }else if(r.type==6 && !ms && !mip){
                view_width(id,std::bit_width(mask),false);
                image_result(std::format("UAV_LOAD{}({}).{}",id,position,swizzle));
            }else unsupported("incompatible image load");
        } else {
            auto samp=descriptor(a[3]);
            if (p.resources[samp].type!=1 || r.type!=3) unsupported("unknown sample resource");
            if(gather_l){
                if(dims!=2 || d16 || a16)unsupported("explicit LOD gather requires a full-width 2D texture");
                loaded_textures.insert(id);
                auto sp=[&](unsigned k){sampler_fields.emplace(samp,k);return std::format("XS_SAMP_{}_{}",samp,k);};
                auto im=[&](unsigned k){image_fields.emplace(id,k);return std::format("XS_FIELD_{}_{}",id,k);};
                auto word=[&](auto&& field,unsigned first){
                    return std::format("({}|({}<<8u)|({}<<16u)|({}<<24u))",field(first),field(first+1),field(first+2),field(first+3));
                };
                auto mode=sp(view::sampler_addr),minimum=word(sp,view::sampler_min),maximum=word(sp,view::sampler_max);
                auto bias_bits=word(sp,view::sampler_bias),view_min=word(im,view::min_lod0);
                unsigned channel=std::countr_zero(mask);auto border=word(sp,view::sampler_border+4*channel);
                auto w=temp("uint","0u"),h=temp("uint","0u"),levels=temp("uint","0u");
                line(std::format("{}.GetDimensions(0u,{},{},{});",name,w,h,levels));
                // One mip even for a trilinear sampler. On gfx1030 the
                // sampler bias is applied first; half-integer LOD selects
                // the coarser mip. Fetch, never filter across mip levels.
                auto level=temp("float",std::format("max(asfloat({}),clamp(asfloat({})+asfloat({}),asfloat({}),asfloat({})))",
                    view_min,addr[first+2],bias_bits,minimum,maximum));
                auto mipnum=temp("uint",std::format("uint(clamp(floor({}+0.5),0.0,float(max({} ,1u)-1u)))",level,levels));
                auto dim=temp("int2",std::format("int2(max(1u,{}>>{}),max(1u,{}>>{}))",w,mipnum,h,mipnum));
                Str off=offset?std::format(" + int2(asint({0}<<26u)>>26,asint({0}<<18u)>>26)",addr[0]):"";
                auto base=temp("int2","int2(floor(asfloat("+position+")*float2("+dim+")-0.5))"+off);
                Str values;
                for(auto [x,y]:{std::pair{0u,1u},{1u,1u},{1u,0u},{0u,0u}}){
                    auto xy=temp("int2",std::format("int2(xs_address({0}.x+{1},{2}.x,({3}&7u)),xs_address({0}.y+{4},{2}.y,(({3}>>3u)&7u)))",base,x,dim,mode,y));
                    values+=(values.empty()?"":",")+std::format("(any({0}<0) ? {1} : asuint({2}.Load(int3(max({0},0),{3})).{4}))",xy,border,name,mipnum,xyzw[channel]);
                }
                image_result("uint4("+values+")");return true;
            }
            sampled_textures.insert(id);
            (cmp?comparison_samplers:regular_samplers).insert(samp);
            if(comparison_samplers.contains(samp) && regular_samplers.contains(samp))unsupported("mixed comparison sampler");
            Str off=offset?std::format(",int2(asint({0}<<26)>>26,asint({0}<<18)>>26)",addr[0]):"";
            auto coords="asfloat("+position+")";
            if(cube_array)coords="cube_array_direction("+coords+")";
            else if(cube)coords="cube_direction("+coords+")";
            if(cmp){
                if(channels!=1 || (op=="image_sample_c" && (masked || p.stage!=pixel)))unsupported("extended comparison sample");
                image_result(std::format("asuint({}.{}(res{},{},asfloat({}){}))",name,op.ends_with("lz")?"SampleCmpLevelZero":"SampleCmp",
                                         samp,coords,addr[unsigned(offset)],off));return true;
            }
            if(gather) {
                constexpr const char* channels[]{"Red","Green","Blue","Alpha"};
                image_result(std::format("asuint({}.Gather{}(res{},{}{}))",name,channels[std::countr_zero(mask)],samp,coords,off));
                return true;
            }
            bool explicit_grad=(op=="image_sample" || bias) && (masked || !blocks.empty() || loop_depth);
            if((op=="image_sample" || bias) && p.stage!=pixel)unsupported("implicit derivatives outside pixel stage");
            std::string swizzle;for(unsigned k=0;k<4;++k) if(mask&(1u<<k)) swizzle+=xyzw[k];
            Str lod;
            auto spatial_grad=[&](Str g){
                if(cube || cube_array){cube_gradients=true;return "cube_face_gradient(asfloat("+addr[first+2]+"),"+g+")";}
                return g;
            };
            if(grad){
                for(unsigned axis=0;axis<2;++axis){Str g="asfloat(uint"+suffix(spatial)+"(";
                    for(unsigned j=0;j<spatial;++j)g+=(j?",":"")+addr[unsigned(offset)+axis*spatial+j];
                    lod+=","+spatial_grad(g+"))");}
            }else if(op=="image_sample_lz")lod=",0";
            else if(op!="image_sample")lod=",asfloat("+addr[bias?unsigned(offset):dims+first]+")";
            if(explicit_grad){
                if(a16)unsupported("implicit derivatives require complete projected quad coordinates");
                auto grad=[&](View axis){
                    Str s="float"+suffix(spatial)+"(";
                    for(unsigned j=0;j<spatial;++j){auto& n=names.at(first+j);
                         Str g;
                         if(value("grad_"+n).kind==Kind::number)g=std::format("asfloat(dd{}_{})",axis,n);
                           else if(!loop_depth && quad_uniform && (value("quadword_"+n).kind==Kind::immediate ||
                                   (peek("quaddef_"+n).kind==Kind::immediate && quad_scope("exec").kind==Kind::immediate &&
                                    peek("quaddef_"+n).id==quad_scope("exec").id))){
                             // The enclosing guest scalar branches are wave votes.
                             // Evaluate the four live register values *before* the
                             // per-lane guard on the sample. Inactive lanes retain
                             // the last value written to their physical VGPR.
                             g=temp("float",std::format("dd{}_fine(asfloat({}))",axis,n));
                          }else if(quad_uniform && !loop_depth && physical_quad_temps.contains(n) && value(n).kind==Kind::number){
                              // A scratch VGPR first written under a partial
                              // EXEC has its actual previous word in inactive
                              // lanes, or ISA undef if never written. Its
                              // read has no symbolic rasterizer input/SRD.
                              // Keep that undef at entry; differentiate the
                              // physical per-pixel register before predication.
                              undef_quad_temps.insert(n);
                              g=temp("float",std::format("dd{}_fine(asfloat({}))",axis,n));
                          }else unsupported("implicit derivatives without dominating full-quad coordinates: "+n);
                         s+=(j?",":"")+g;
                    } s+=")";
                     return spatial_grad(bias?"("+s+" * exp2(asfloat("+addr[unsigned(offset)]+")))":s);
                };
                lod=","+grad("x")+","+grad("y");
            }
            image_result("asuint("+name+(explicit_grad||grad?".SampleGrad(":op=="image_sample"?".Sample(":bias?".SampleBias(":".SampleLevel(")+std::format("res{},",samp)+
                 coords+lod+off+")."+swizzle+")",explicit_grad);
        }
        return true;
    }
    bool interpolate(const Instruction& i) {
        bool flat=i.op=="v_interp_mov_f32";
        if(!flat && i.op!="v_interp_p1_f32" && i.op!="v_interp_p2_f32") return false;
        auto& a=i.args;bool second=i.op=="v_interp_p2_f32";
        if(p.stage!=pixel || a.size()!=3 || !i.modifiers.empty() || value("m0").kind!=Kind::prim)unsupported("unknown barycentric input");
        auto coord=value(a[1]);
        if(coord.kind!=Kind::bary && active_bary.contains(a[1]))coord=active_bary.at(a[1]);
        if(flat?a[1]!="p0":coord.kind!=Kind::bary || coord.id!=unsigned(second))unsupported("unknown barycentric input");
        auto attr=View(a[2]);auto dot=attr.find('.');
        if(!attr.starts_with("attr") || dot==attr.npos || attr.size()!=dot+2) unsupported("unknown interpolant");
        unsigned k=number(attr.substr(4,dot-4)),c=xyzw.find(attr.back());
        if (!p.interpolants.contains(k) || c>=4) unsupported("unmapped interpolant");
        auto& s=semantic(p.inputs,p.interpolants.at(k),1u<<c);
        if (!(s.used&(1u<<c)) || (flat?s.interpolation!=1:s.type!=3 ||
             s.interpolation!=coord.part)) unsupported("unmatched interpolation qualifier");
        check_range(registers(a[0]),'v',1);
        Value partial{Kind::partial,k,c|(coord.part<<2)};
        if (!second && !flat) {
            auto it=paired_interp.find(i.pc);
            bool keep=it!=paired_interp.end() && peek("quadword_"+a[0]).kind==Kind::immediate;
            set(a[0],partial);
            if(keep)state["interp_base_"+a[0]]={Kind::immediate,it->second};
        }
        else {
            if(!flat && value(a[0])!=partial) unsupported("unpaired interpolation phases");
            if(!flat && peek("interp_base_"+a[0])==Value{Kind::immediate,i.pc}){
                // Only the inactive lanes inherit the old word. write()
                // materializes the completed input in the active lanes.
                state["quadword_"+a[0]]={Kind::immediate,1};
            }
            write(a[0],std::format("asuint(input.{}{})",field(s),component(s.mask,c)),varying,true);
            if(flat && !masked)state["flat_input_"+a[0]]={Kind::immediate,k,c};
        }
        return true;
    }
    bool export_value(const Instruction& i) {
        if (i.op!="exp") return false;
        auto& a=i.args;
        if(a.size()!=5)unsupported("extended export operands");
        // A depth/UAV-only pixel shader ends with a null export carrying
        // the coverage mask. It does not kill the surviving fragments.
        if(p.stage==pixel && p.outputs.empty() && a==std::vector<Str>{"null","off","off","off","off"} &&
           i.modifiers=="done vm"){
            if(masked)line("if(!active) discard;");
            return true;
        }
        if(!blocks.empty()) {
            // All bypass edges must terminate in a no-color kill epilog.
            // Otherwise an export in one arm does not define the other arm.
            auto end=blocks.front().end;
            if(p.stage!=pixel || !std::ranges::all_of(blocks,[&](const Block& b){return b.end==end;}) ||
               end<4 || p.code.at<u32>(end-4)!=0xbf810000)unsupported("conditional export without a terminal kill path");
            auto tail=decode(p.code,end);
            // Different mask allocations restore the killed arm with one
            // move or a short sequence of EXEC-only mask ops. They are
            // still emitted on that path; inspect through them to validate
            // that its one export is the null terminal.
            while(tail.op=="s_nop" || (!tail.args.empty() && tail.args[0]=="exec" &&
                (tail.op=="s_mov_b64" || tail.op=="s_and_b64" || tail.op=="s_andn2_b64")))
                tail=decode(p.code,tail.pc+tail.size);
            if(tail.op!="exp" || tail.args!=std::vector<Str>{"null","off","off","off","off"} ||
               decode(p.code,tail.pc+tail.size).op!="s_endpgm")unsupported("alternate pixel exit");
        }
        std::vector<const Semantic*> targets;
        auto layer=std::ranges::find(p.outputs,4u,&Semantic::system);
        if(p.stage==vertex && a[0].starts_with("pos") && a[0]!="pos0" &&
           !p.clip_parameters.empty() && (a[0]!="pos1" || layer==p.outputs.end())){
            unsigned pos=number(View(a[0]).substr(3)),first=layer==p.outputs.end()?1:2;
            if(pos<first)unsupported("unmapped position export");
            for(auto& s:p.outputs)if(s.system==2 && s.index==pos-first)targets.push_back(&s);
        }
        if(p.stage==vertex && a[0]=="pos1" && layer!=p.outputs.end()){
            // POS1.z carries the array layer. The duplicate PARAM export
            // forwards the same semantic to the rasterizer on Xbox.
            auto it=std::ranges::find(p.outputs,4u,&Semantic::system);
            if(it==p.outputs.end() || a[3]=="off")unsupported("missing vertex layer");
            if(!exported.insert(unsigned(it-p.outputs.begin())).second)unsupported("repeated vertex layer");
            line(std::format("output.{} = {} & 8191u;",field(*it),bits(a[3])));return true;
        }
        if(p.stage==pixel && a[0]=="mrtz"){
            if(i.modifiers.contains("compr"))unsupported("compressed depth/stencil export");
            if(masked)line("if(!active)discard;");
            for(unsigned j=0;j<p.outputs.size();++j){
                auto& s=p.outputs[j];unsigned c;
                if(s.system==65 || s.system==67 || s.system==68)c=0;
                else if(s.system==69)c=1;
                else if(s.system==66)c=2;
                else continue;
                if(a[c+1]=="off" || !exported.insert(j).second)unsupported("incomplete or repeated depth/stencil export");
                line(std::format("output.{} = {};",field(s),as_output(s,bits(a[c+1]))));
            }
            return true;
        }
        if (p.stage==vertex && a[0]=="pos0") {
            auto it=std::ranges::find(p.outputs,1u,&Semantic::system);
            if(it!=p.outputs.end())targets.push_back(&*it);
        } else if(p.stage==vertex && a[0].starts_with("param")) {
            unsigned j=number(View(a[0]).substr(5));
            if(p.layer_parameters.contains(j) || p.clip_parameters.contains(j))return true;
            if(p.parameters.contains(j)) {
                for(const auto& v:p.outputs)if(v.reg==p.parameters.at(j) && !v.system)targets.push_back(&v);
                if(targets.empty() && std::ranges::find(p.dead_parameter_regs,p.parameters.at(j))!=p.dead_parameter_regs.end())return true;
            }
            else if(j==0 && p.parameters.empty() && p.outputs.size()==1 && p.outputs[0].system==1)
                return true; // hardware dummy parameter for a position-only pipeline
        } else if(p.stage==pixel && a[0].starts_with("mrt")) {
            unsigned index=number(View(a[0]).substr(3));
            for (auto& v:p.outputs) if(v.system==64 && v.index==index)targets.push_back(&v);
        }
        if(targets.empty())unsupported("unmapped export");
        if (masked) {
            if(p.stage!=pixel) unsupported("masked vertex export");
            line("if (!active) discard;");
        }
        bool packed=i.modifiers=="done compr vm";
        if (i.modifiers!="" && i.modifiers!="vm" && i.modifiers!="done" && i.modifiers!="done vm" && !packed)
            unsupported("export modifier");
        for(auto s:targets) {
            if(!exported.insert(unsigned(s-p.outputs.data())).second)unsupported("repeated export");
            if(packed && (p.stage!=pixel || s->type!=3 || a[1]!=a[2] || a[3]!=a[4]))
                unsupported("unknown compressed export");
            for(unsigned c=0;c<4;++c) if(s->mask&(1u<<c)) {
                if (a[1+c]=="off") unsupported("partially disabled export");
                auto v=bits(a[1+c]);
                auto e=packed?"f16tof32(("+v+(c%2?" >> 16u)":" )")+" & 65535u)":as_output(*s,v);
                line(std::format("output.{}{} = {};",field(*s),component(s->mask,c),e));
            }
        }
        return true;
    }
    Str declarations() const {
        std::ostringstream o;
        for(auto pc:vm_waits)o<<std::format("#define XS_DEVICE_WAIT_{}() {}\n",pc,vm_required.contains(pc)?"DeviceMemoryBarrier()":"");
        if(pool_start!=~0u){
            auto words=(p.code.size()-pool_start)/4;
            if(!words || words>16384)unsupported("unsupported literals span");
            o<<"static const uint literals["<<words<<"]={";
            for(size_t j=0;j<words;++j)o<<std::format("{}0x{:08x}u",j?",":"",p.code.at<u32>(pool_start+j*4));
            o<<std::format("}};\nuint literal_load(uint a,uint h) {{ uint i=(a-{}u)>>2; return !h && i<{}u ? literals[i] : 0u; }}\n",pool_start,words);
        }
        if(virtual_wave) {
            if(half_lds)o<<std::format("groupshared uint16_t xs_half_lds[{}];\n",p.lds_bytes/2);
            else o<<std::format("groupshared uint lds[{}];\n",std::max(1u,p.lds_bytes/4));
            o<<std::format("#define LANES {}u\n",std::min(p.wave,p.threads[0]*p.threads[1]*p.threads[2]));
            if(half_lds)o<<R"(
uint xs_lds_word(uint b){
    uint i=(b>>2u)*2u;
    return uint(xs_half_lds[i])|(uint(xs_half_lds[i+1u])<<16u);
}
uint xs_lds_half(uint b){
    return uint(xs_half_lds[b>>1u]);
}
void xs_lds_store_half(uint b,uint v,bool enabled,uint lane){
    if(enabled)xs_half_lds[b>>1u]=(uint16_t)v;
}
void xs_lds_store_word(uint b,uint v,bool enabled,uint lane){
    uint i=(b>>2u)*2u;
    // A mixed-width kernel has a single wave. Pick one writer per dword
    // before splitting it, so even colliding lanes cannot tear its halves.
    uint2 match=WaveMatch(enabled?i:~0u).xy;
    uint first=match.x?uint(firstbitlow(match.x)):32u+uint(firstbitlow(match.y));
    if(enabled && lane==first){xs_half_lds[i]=(uint16_t)v;xs_half_lds[i+1u]=(uint16_t)(v>>16u);}
}
)";
            if(p.native_compute_wave)o<<R"(
uint group_reduce(bool b,uint lane,uint mode){
    uint2 vote=WaveActiveBallot(b).xy;
    if(mode==0u)return countbits(vote.x)+countbits(vote.y);
    if(mode==1u)return vote.x!=0u?uint(firstbitlow(vote.x)):vote.y!=0u?32u+uint(firstbitlow(vote.y)):~0u;
    uint prefix=(1u<<(lane&31u))-1u;
    if(mode==2u)return countbits(vote.x&(lane<32u?prefix:~0u));
    if(mode==3u)return lane<32u?0u:countbits(vote.y&prefix);
    return mode==4u?vote.x:vote.y;
}
uint group_lane(uint x,uint which,uint lane){
    uint v=WaveReadLaneAt(x,min(which,LANES-1u));return which<LANES?v:0u;
}
uint group_first_value(uint x,bool enabled,uint lane){
    return group_lane(x,group_reduce(enabled,lane,1u),lane);
}
uint group_scatter(uint x,uint to,bool enabled,uint lane){
    uint v=0u;
    [loop]for(uint j=0u;j<LANES;++j){
        uint addr=WaveReadLaneAt(enabled?to:~0u,j),word=WaveReadLaneAt(x,j);
        if(addr==lane)v=word;
    }
    return v;
}
#define ANY(x) WaveActiveAnyTrue(x)
)";
            else {
            o<<R"(groupshared uint ballots[LANES], lane_values[LANES];
uint group_reduce(bool b,uint lane,uint mode) {
    ballots[lane]=b?1u:0u;
    GroupMemoryBarrierWithGroupSync();
    uint v=mode==1u?0xffffffffu:0u;
    uint start=mode==3u || mode==5u?32u:0u;
    uint end=mode==4u?min(LANES,32u):mode==5u?LANES:mode<2u?LANES:mode==2u?min(lane,32u):lane;
    for(uint j=start;j<end;++j) if(ballots[j]) {
        if(mode>=4u) v|=1u<<(j&31u); else if(mode==1u) v=min(v,j); else ++v;
    }
    GroupMemoryBarrierWithGroupSync();
    return v;
}
uint group_first_value(uint x,bool enabled,uint lane) {
    lane_values[lane]=x;
    uint first=group_reduce(enabled,lane,1u);
    uint v=first<LANES?lane_values[min(first,LANES-1u)]:0u;
    GroupMemoryBarrierWithGroupSync();
    return v;
}
uint group_lane(uint x,uint which,uint lane) {
    lane_values[lane]=x;GroupMemoryBarrierWithGroupSync();
    uint v=which<LANES?lane_values[min(which,LANES-1u)]:0u;
    GroupMemoryBarrierWithGroupSync();return v;
}
uint group_scatter(uint x,uint to,bool enabled,uint lane){
    ballots[lane]=enabled?to:~0u;lane_values[lane]=x;GroupMemoryBarrierWithGroupSync();
    uint v=0;for(uint j=0;j<LANES;++j)if(ballots[j]==lane)v=lane_values[j];
    GroupMemoryBarrierWithGroupSync();return v;
}
)";
            // Exactly one virtual XS wave per workgroup. Every lane follows
            // the same block PC, including lanes whose VGPR writes are masked.
            // This also works where the host wave has fewer than 64 lanes.
            o<<R"(groupshared uint vote;
bool group_any(bool b, uint lane) {
    GroupMemoryBarrierWithGroupSync();
    if (!lane) vote=0u;
    GroupMemoryBarrierWithGroupSync();
    if (b) InterlockedOr(vote,1u);
    GroupMemoryBarrierWithGroupSync();
    return vote!=0u;
}
#define ANY(x) group_any((x),lane)
)";
            }
        } else o<<"#define ANY(x) WaveActiveAnyTrue(x)\n";
        if(masked_pixel_election)o<<R"(
uint ps_first_enabled(uint value,bool enabled){
    uint4 vote=WaveActiveBallot(enabled);
    uint lane=vote.x?uint(firstbitlow(vote.x)):vote.y?32u+uint(firstbitlow(vote.y)):
              vote.z?64u+uint(firstbitlow(vote.z)):vote.w?96u+uint(firstbitlow(vote.w)):0u;
    return WaveReadLaneAt(value,lane);
}
)";
        unsigned slot=0;
        if(!table_words.empty())o<<std::format("cbuffer XsShaderTables : register(b0, space{}) {{ uint4 xs_tables[{}]; }};\n",view::table_space,view::table_count/4);
        if(table_words.size()+raw_reads.size()>view::table_count)unsupported("dynamic scalar descriptor words exceed root ABI");
        for(auto d:raw_writes){
            o<<std::format("#define XS_SRD_WRITE_{}(dst) ",d);
            if(raw_reads.contains(d)){
                size_t slot=table_words.size()+std::distance(raw_reads.begin(),raw_reads.find(d));
                o<<std::format("dst = xs_tables[{}].{}",slot/4,xyzw[slot%4]);
            }
            o<<"\n";
        }
        for(auto [id,width]:view_widths) {
            o<<std::format("#if FROSTY_VIEW_{} {} \n#define TYPE{} uint{}\n#define BITS{}(x) (x)\n",slot,integer_views.contains(id)?"!= 2":"== 1",id,suffix(width),id)
             <<std::format("#elif FROSTY_VIEW_{} == 2\n#define TYPE{} int{}\n#define BITS{}(x) asint(x)\n",slot,id,suffix(width),id)
             <<std::format("#else\n#define TYPE{} float{}\n#define BITS{}(x) asfloat(x)\n#endif\n",id,suffix(width),id);
            ++slot;
        }
        for(auto [id,k]:image_fields)o<<std::format("#define XS_FIELD_{}_{} uint(FROSTY_VIEW_{})\n",id,k,slot++);
        for(auto [id,k]:sampler_fields)o<<std::format("#define XS_SAMP_{}_{} uint(FROSTY_VIEW_{})\n",id,k,slot++);
        if(!sampler_fields.empty())o<<R"(
int xs_address(int x,int n,uint mode) {
    // D3D address mode minus one: wrap, mirror, clamp, border, mirror-once.
    if(mode==0u){int a=x%n;return a<0?a+n:a;}
    if(mode==1u){int a=x%(2*n);if(a<0)a+=2*n;return a<n?a:2*n-1-a;}
    if(mode==3u)return x<0 || x>=n ? -1:x;
    if(mode==4u && x<0)x=-(x+1);
    return clamp(x,0,n-1);
}
)";
        for(auto id:half_images){
            if(view_widths.contains(id)){
                auto k=std::distance(view_widths.begin(),view_widths.find(id));
                o<<std::format("#if FROSTY_VIEW_{0} == 1 || FROSTY_VIEW_{0} == 2\n#define PACK16_{1}(v) ((v)&65535u)\n",k,id)
                 <<std::format("#if FROSTY_VIEW_{} == 2\n#define UNPACK16_{}(v) asuint(asint((v)<<16u)>>16)\n#else\n#define UNPACK16_{}(v) (v)\n#endif\n#else\n",k,id,id);
            }
            o<<std::format("#define PACK16_{}(v) f32tof16(asfloat(v))\n#define UNPACK16_{}(v) asuint(f16tof32(v))\n",id,id);
            if(view_widths.contains(id))o<<"#endif\n";
        }
        for (unsigned id:used) {
            const auto& r=p.resources[id];auto reg=r.reg;
            if(r.type==2) {
                o<<std::format("cbuffer B{} : register(b{}) {{ uint4 cb{}[{}]; }};\n",reg,reg,reg,(cb_end.at(id)+3)/4);
            } else {
                Str ty;char bind;
                if(r.type==1 && r.kind==14) {ty=comparison_samplers.contains(id)?"SamplerComparisonState":"SamplerState";bind='s';}
                else if(r.type==3 && (r.kind==2 || r.kind==3 || r.kind==4 || r.kind==5 || r.kind==7 || r.kind==9)) {
                    ty=r.kind==2?"Texture2D":r.kind==3?"Texture2DMS":r.kind==4?"Texture3D":r.kind==7?"Texture2DArray":r.kind==9?"TextureCubeArray":"TextureCube";
                    ty+=view_widths.contains(id)?std::format("<TYPE{}>",id):"<float4>";bind='t';
                }
                else if(r.type==3 && r.kind==10) {ty=std::format("Buffer<TYPE{}>",id);bind='t';}
                else if(r.type==4 && r.kind==11) {ty="ByteAddressBuffer";bind='t';}
                else if(ray_mode && r.type==4 && r.kind==16){ty="RaytracingAccelerationStructure";bind='t';}
                else if(r.type==7 && r.kind==11) {ty="RWByteAddressBuffer";bind='u';}
                else if((r.type==5 || r.type==8) && r.kind==12) {
                    if(!r.stride || r.stride>2048 || r.stride%4)unsupported("invalid structured stride");
                    o<<std::format("struct Element{} {{uint w[{}];}};\n",id,r.stride/4);
                    ty=std::format("{}StructuredBuffer<Element{}>",r.type==8?"RW":"",id);bind=r.type==8?'u':'t';
                }
                else if(r.type==6 && (r.kind==2 || r.kind==4 || r.kind==7 || r.kind==10)) {
                    auto wide=atomic64_images.contains(id);
                    if(wide && (half_images.contains(id) || integer_views.contains(id)))unsupported("mixed-width atomics on the same image");
                    ty=std::format("{}<{}>",r.kind==2?"RWTexture2D":r.kind==4?"RWTexture3D":r.kind==7?"RWTexture2DArray":"RWBuffer",
                                   wide?"uint64_t":std::format("TYPE{}",id));bind='u';
                    if(r.kind!=10)o<<std::format("#define UAV_LOAD{0}(p) {1}(res{0}[p])\n#define UAV_STORE{0}(p,v) res{0}[p] = {2}(v)\n",
                        id,wide?"rd_words":"asuint",wide?"rd_u64":std::format("BITS{}",id));
                }
                else unsupported("unknown desktop resource type");
                o<<std::format("{}{} res{} : register({}{});\n",device_sync && bind=='u'?"globallycoherent ":"",ty,id,bind,reg);
            }
        }
        if(triangle_queries)o<<xs_fetch_source;
        if(ray_mode)o<<xs_visibility_source;
        o<<R"(
uint mulhi(uint a, uint b) {
    uint al=a&65535u, ah=a>>16, bl=b&65535u, bh=b>>16;
    uint t=ah*bl+((al*bl)>>16);
    return ah*bh+(t>>16)+(((t&65535u)+al*bh)>>16);
}
uint cvt_u32(float f) { return !(f>0.0) ? 0u : f>=4294967296.0 ? 0xffffffffu : uint(f); }
int rd_s16(uint x){return asint(x<<16u)>>16;}
int rd_s8(uint x){return asint(x<<24u)>>24;}
uint rd_clz(uint x){return x!=0u?31u-firstbithigh(x):~0u;}
uint rd_clz_sign(uint x){return rd_clz(asint(x)<0?~x:x);}
uint rd_smulhi(uint a,uint b){return mulhi(a,b)-(asint(a)<0?b:0u)-(asint(b)<0?a:0u);}
uint rd_ibfe(uint x,uint off,uint w){
    off&=31u;w&=31u;if(w==0u)return 0u;
    if(off+w>=32u)return asuint(asint(x)>>off);
    return asuint(asint(x<<(32u-off-w))>>(32u-w));
}
uint rd_ibs(uint x,uint spec){
    uint off=spec&31u,w=(spec>>16u)&127u;
    return w>=32u?asuint(asint(x)>>off):rd_ibfe(x,off,w);
}
uint rd_align(uint hi,uint lo,uint shift){shift&=31u;return shift==0u?lo:(lo>>shift)|(hi<<(32u-shift));}
uint rd_alignbyte(uint hi,uint lo,uint n){n&=31u;if(n>=8u)return 0u;if(n>=4u)return hi>>(8u*(n-4u));return rd_align(hi,lo,8u*n);}
uint rd_quadmask(uint a){uint d=0u;[unroll]for(uint i=0;i<8u;++i)d|=uint(((a>>(4u*i))&15u)!=0u)<<i;return d;}
uint64_t rd_spread(uint s){uint64_t r=0;[unroll]for(uint i=0;i<32u;++i)r|=uint64_t(3u*((s>>i)&1u))<<(2u*i);return r;}
uint rd_lerp8(uint a,uint b,uint c){uint r=0;[unroll]for(uint j=0;j<32u;j+=8u)
    r|=((((a>>j)&255u)+((b>>j)&255u)+((c>>j)&1u))>>1u)<<j;return r;}
uint rd_msad(uint a,uint b){uint r=0;[unroll]for(uint j=0;j<32u;j+=8u){int x=(a>>j)&255u,y=(b>>j)&255u;if(y!=0)r+=abs(x-y);}return r;}
uint2 rd_frexp(uint b){
    uint e=(b>>23u)&255u,m=b&0x7fffffu,sign=b&0x80000000u;
    if(e==255u || (e|m)==0u)return uint2(b,0u);
    if(e!=0u)return uint2(sign|0x3f000000u|m,asuint(int(e)-126));
    int k=23-int(firstbithigh(m));return uint2(sign|0x3f000000u|((m<<k)&0x7fffffu),asuint(-125-k));
}
uint rd_perm(uint hi,uint lo,uint sel) {
    uint outv=0;
    [unroll] for(uint j=0;j<4u;++j){
        uint s=(sel>>(8u*j))&255u, ix=s<8u?s:2u*(s-8u)+1u;
        uint v=((ix<4u?lo:hi)>>(8u*(ix&3u)))&255u;
        outv|=(s<8u?v:s<12u?0u-(v>>7u):s==12u?0u:255u)<<(8u*j);
    }
    return outv;
}
uint rd_sad(uint a,uint b,uint w){uint d=0,mask=(1u<<w)-1u;for(uint i=0;i<32u;i+=w)d+=abs(int((a>>i)&mask)-int((b>>i)&mask));return d;}
uint rd_dot(uint a,uint b,uint z,uint w,bool sg){
    for(uint i=0;i<32u;i+=w){uint x=a>>i,y=b>>i,k=32u-w;
        z+=sg?asuint((asint(x<<k)>>k)*(asint(y<<k)>>k)):(x&((1u<<w)-1u))*(y&((1u<<w)-1u));}return z;
}
uint64_t rd_u64(uint2 x){return (uint64_t(x.y)<<32u)|x.x;}
uint2 rd_words(uint64_t x){return uint2(uint(x),uint(x>>32u));}
double rd_double(uint2 x){return asdouble(x.x,x.y);}
uint2 rd_double_words(double x){uint lo,hi;asuint(x,lo,hi);return uint2(lo,hi);}
uint64_t rd_bfe64(uint64_t x,uint spec,bool sg){
    uint off=spec&63u,w=min((spec>>16u)&127u,64u-off);
    if(w==0u)return 0u;
    if(sg)return uint64_t(int64_t(x<<(64u-off-w))>>(64u-w));
    return w==64u?x:(x>>off)&((uint64_t(1)<<w)-1u);
}
uint rd_class_parts(bool neg,uint expv,uint expmax,bool frac,bool quiet){
    if(expv==expmax)return frac?uint(quiet):(neg?2u:9u);
    if(expv==0u)return frac?(neg?4u:7u):(neg?5u:6u);
    return neg?3u:8u;
}
uint rd_class(uint x,uint signbit,uint mantissa,uint expmax){
    return rd_class_parts(((x>>signbit)&1u)!=0u,(x>>mantissa)&expmax,expmax,
                          (x&((1u<<mantissa)-1u))!=0u,((x>>(mantissa-1u))&1u)!=0u);
}
uint rd_class64(uint2 x){
    return rd_class_parts((x.y>>31u)!=0u,(x.y>>20u)&2047u,2047u,((x.y&0xfffffu)|x.x)!=0u,(x.y&0x80000u)!=0u);
}
uint rd_fp_relation(uint2 a,uint2 b,uint exponent) {
    uint am=a.y&0x7fffffffu,bm=b.y&0x7fffffffu;
    if(am>exponent || (am==exponent && a.x!=0u) || bm>exponent || (bm==exponent && b.x!=0u))return 8u;
    if(all(a==b) || ((am|a.x|bm|b.x)==0u))return 2u;
    bool an=(a.y>>31u)!=0u,bn=(b.y>>31u)!=0u;
    bool small=a.y<b.y || (a.y==b.y && a.x<b.x);
    return (an!=bn?an:(small!=an))?1u:4u;
}
// V_CUBE{SC,TC,MA,ID}: tied major axes have Z, then Y, then X priority.
float4 cube_alu(float x,float y,float z) {
    if(abs(z)>=abs(x) && abs(z)>=abs(y))return float4(z<0?-x:x,-y,2.0*z,z<0?5.0:4.0);
    if(abs(y)>=abs(x))return float4(x,y<0?-z:z,2.0*y,y<0?3.0:2.0);
    return float4(x<0?z:-z,-y,2.0*x,x<0?1.0:0.0);
}
// MIMG cube coordinates use [1,2] face coordinates and a floating face
// index. Reconstruct a direction for the desktop cube view and its sampler.
float3 cube_direction(float3 a) {
    float2 st=clamp(2.0*(a.xy-1.5),-asfloat(0x3f7fffffu),asfloat(0x3f7fffffu));
    switch(uint(a.z)){
    case 0u:return float3(1,-st.y,-st.x);
    case 1u:return float3(-1,-st.y,st.x);
    case 2u:return float3(st.x,1,st.y);
    case 3u:return float3(st.x,-1,-st.y);
    case 4u:return float3(st.x,-st.y,1);
    default:return float3(-st.x,-st.y,-1);
    }
}
uint2 shl64(uint2 v,uint n) {
    n &= 63u;
    if(n==0u) return v;
    return n>=32u ? uint2(0u,v.x<<(n-32u)) : uint2(v.x<<n,(v.y<<n)|(v.x>>(32u-n)));
}
uint cvt_i32(float f) {return isnan(f)?0u:f<=-2147483648.0?0x80000000u:f>=2147483648.0?0x7fffffffu:asuint(int(f));}
uint scalar_bfe(uint v,uint spec) {
    uint x=v>>(spec&31u), w=(spec>>16u)&127u;
    return w>=32u ? x : x&((1u<<w)-1u);
}
uint half_rtz(float f) {
    uint h=f32tof16(f);
    return abs(f16tof32(h))>abs(f) ? h-1u : h;
}
float legacy_mul(float a,float b) { return a==0.0 || b==0.0 ? 0.0 : a*b; }
float legacy_fma(float a,float b,float c) { return a==0.0 || b==0.0 ? 0.0+c : mad(a,b,c); }
float unfused(float a,float b,float c) { precise float product=a*b; return product+c; }
)";
        // DXIL FMin returns the numeric operand for a single NaN. The test
        // therefore catches precisely the original ±zero cases, including
        // zero * infinity/NaN. RADV's open-coded fmulz recognizer can map
        // this form back to a single RDNA legacy multiply.
        if(native_mulz)o<<"float xs_mulz(float a,float b){return min(abs(a),abs(b))==0.0 ? 0.0 : a*b;}\n";
        if(half_vectors)o<<"uint xs_pack2(float16_t2 v){return uint(asuint16(v.x)) | (uint(asuint16(v.y))<<16u);}\n";
        if(native_fcmp_pair)o<<fcmp32_helper;
        if(cube_gradients)o<<R"(
// The MIMG address contains face-space S,T. D3D SampleGrad expects a
// derivative of the direction, with neither the face nor layer differentiated.
float3 cube_face_gradient(float packed,float2 delta){
    float2 d=2.0*delta;
    switch(uint(round(packed))&7u){
    case 0u:return float3(0,-d.y,-d.x);
    case 1u:return float3(0,-d.y,d.x);
    case 2u:return float3(d.x,0,d.y);
    case 3u:return float3(d.x,0,-d.y);
    case 4u:return float3(d.x,-d.y,0);
    default:return float3(-d.x,-d.y,0);
    }
}
)";
        if(std::ranges::any_of(used,[&](unsigned id){return p.resources[id].kind==9;}))o<<R"(
// Xbox cube-array samplers pack cube index * 8 + face into the third
// coordinate. D3D takes a reconstructed direction and a separate layer.
float4 cube_array_direction(float3 a){
    float packed=round(a.z),layer=floor(packed*0.125);
    return float4(cube_direction(float3(a.xy,packed-8.0*layer)),layer);
}
)";
        if(soft_double)o<<soft64;
        if(native_group){
            auto n=p.threads[0]*p.threads[1]*p.threads[2];
            // Assign a distinct guest wave ID to each full hardware wave.
            // Do not assume that SV_GroupIndex selects a Vulkan subgroup.
            if(p.native_group_id){
                bool retire=body.view().find("xs_live")!=View::npos;
                if(!triangle_queries)o<<"struct XsFetchCommand{uint opcode;uint pad0[18];uint query;uint pad1[44];};"
                    "RWStructuredBuffer<XsFetchCommand> xs_fetch_ext:register(u0,space31282);\n";
                o<<"uint xs_group_id(){uint i=xs_fetch_ext.IncrementCounter();xs_fetch_ext[i].opcode=19u;"
                    "xs_fetch_ext[i].query=0x46534749u;return xs_fetch_ext.IncrementCounter();}\n";
                if(retire)o<<"groupshared uint xs_live;\n";
                o<<std::format("[WaveSize({})][numthreads({},1,1)] void main(uint3 group:SV_GroupID,uint physical:SV_GroupIndex) {{\n",p.wave,n);
                if(retire)o<<std::format("    if(physical==0u)xs_live={}u; GroupMemoryBarrierWithGroupSync();\n",n/p.wave);
                o<<"    uint waveID=xs_group_id();\n";
            }else{
                o<<"groupshared uint xs_wave_alloc,xs_live;\n";
                o<<std::format("[WaveSize({})][numthreads({},1,1)] void main(uint3 group:SV_GroupID,uint physical:SV_GroupIndex) {{\n",p.wave,n);
                o<<std::format("    if(physical==0u){{xs_wave_alloc=0u;xs_live={}u;}} GroupMemoryBarrierWithGroupSync();\n",n/p.wave)
                 <<"    uint elected=0u;if(WaveIsFirstLane()){InterlockedAdd(xs_wave_alloc,1u,elected);}uint waveID=WaveReadLaneFirst(elected);\n";
            }
            o<<"    uint lane=WaveGetLaneIndex(), guest_index=waveID*LANES+lane;\n"
             <<std::format("    uint3 local=uint3(guest_index%{}u,(guest_index/{}u)%{}u,guest_index/{}u);\n",
                p.threads[0],p.threads[0],p.threads[1],p.threads[0]*p.threads[1]);
        }else if (multi_wave){
            o<<"struct WaveState {uint pc;bool active,waiting;\n";
            if(indexed_vgprs)o<<"uint vgpr_file[256];\n";
            for(const auto& r:vars)if(!indexed_vgprs || !vgpr(r))o<<"uint "<<r<<";\n";
            o<<"};\n";
            if(p.native_compute_wave)o<<std::format("[WaveSize({})][numthreads(LANES,1,1)] void main(uint3 group:SV_GroupID) {{ uint lane=WaveGetLaneIndex();\n",p.wave);
            else o<<"[numthreads(LANES,1,1)] void main(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){\n";
        }else if(virtual_wave && p.native_compute_wave){
            // Exactly one requested physical wave in this workgroup. Map
            // the guest local IDs from its lane, not a presumed Vulkan
            // subgroup-to-SV_GroupIndex ordering.
            o<<std::format("[WaveSize({})][numthreads(LANES,1,1)] void main(uint3 group:SV_GroupID) {{ uint lane=WaveGetLaneIndex();\n",p.wave);
            o<<std::format("    uint3 local=uint3(lane%{}u,(lane/{}u)%{}u,lane/{}u);\n",p.threads[0],p.threads[0],p.threads[1],p.threads[0]*p.threads[1]);
        }else if (p.stage==compute)
            o<<std::format("[numthreads({},{},{})]\nvoid main(uint3 group:SV_GroupID, uint3 local:SV_GroupThreadID, uint lane:SV_GroupIndex) {{\n",
                p.threads[0],p.threads[1],p.threads[2]);
        else {
            for(bool input:{true,false}) {
                o<<"struct "<<(input?"Input":"Output")<<" {\n";
                std::map<unsigned,unsigned> regs;
                for(auto& s:input?p.inputs:p.outputs) {
                    if(s.reg!=~0u){
                        if (regs[s.reg]&s.mask)unsupported("overlapping signature components");
                        regs[s.reg]|=s.mask;
                    }
                    if ((s.system!=0 && s.system!=1 && s.system!=2 && s.system!=4 && s.system!=6 && s.system!=8 &&
                        !(s.system==9 && input && p.stage==pixel) && !(p.stage==pixel && !input && s.system>=64 && s.system<=69)) || s.name.empty())
                        unsupported("unsupported system semantic");
                    // Preserve semantic indices/linkage; do not assign Vulkan
                    // locations here. vkd3d owns root and graphics interfaces.
                    constexpr View modes[]{"","nointerpolation ","","centroid ","noperspective ",
                        "noperspective centroid ","sample ","noperspective sample "};
                    if(s.interpolation>=std::size(modes)) unsupported("unknown interpolation mode");
                    o<<std::format("    {}{} {} : {}{};\n",modes[s.interpolation],type(s),field(s),s.name,s.index);
                }
                o<<"};\n";
            }
            if(p.stage==geometry)
                o<<std::format("[maxvertexcount({})] void main(triangle Input input[3], inout TriangleStream<Output> stream) {{\n",p.max_vertices);
            else {
                if(p.stage==pixel && p.depth_before_pixel)o<<"[earlydepthstencil]\n";
                o<<"Output main(Input input) {\n";
            }
            o<<"    Output output;\n";
        }
        o<<"    bool active = true;\n";
        if(indexed_vgprs){
            o<<"    uint vgpr_file[256];\n";
            for(unsigned j=0;j<256;++j)o<<"#define v"<<j<<" vgpr_file["<<j<<"]\n";
        }
        Names bank_regs;for(auto [base,width]:local_banks){
            o<<"    uint relbank"<<base<<"["<<width<<"];\n";
            for(unsigned k=0;k<width;++k){auto name=reg('v',base+k);bank_regs.insert(name);o<<"#define "<<name<<" relbank"<<base<<"["<<k<<"]\n";}
        }
        for(auto& r:vars)if((!indexed_vgprs || !vgpr(r)) && !bank_regs.contains(r))o<<"    uint "<<r<<
            (initial_undef.contains(r)||undef_quad_temps.contains(r)?";\n":" = 0u;\n");
        return o.str();
    }
    bool ray_address_op(const Instruction& i){
        if(!ray_mode || i.args.size()<2)return false;
        const auto& a=i.args;const auto& op=i.op;
        if(hit_post){
            if(auto it=hit_post->reads.find(i.pc);it!=hit_post->reads.end()){
                auto dst=registers(a[0]);Str args;
                for(auto w:it->second)args+=(args.empty()?"":",")+
                    (w.field==HitWord::literal?std::format("{}u",w.n):bits(hit_name(*hit_post,w)));
                load_values(dst,dst.count==1?args:"uint"+suffix(dst.count)+"("+args+")");
                return true;
            }
            return false;
        }
        if((op=="v_mov_b32" || op=="s_mov_b32") && value(a[1]).kind==Kind::ray_node){
            auto v=value(a[1]);if(v.part)write(a[0],a[1],v,i.op[0]=='v');else set(a[0],v);return true;
        }
        bool mem=op.starts_with("global_load_dword");
        if(!mem && op!="v_mov_b32" && op!="s_mov_b32" && op!="s_and_b32" && op!="s_or_b32" && op!="s_lshr_b64")return false;
        std::optional<unsigned> as;
        for(unsigned k=1;k<a.size();++k){
            const auto& n=a[k];auto v=peek(n);
            if(n.size()>2 && n[1]=='[' && (n[0]=='s'||n[0]=='v'))v=peek(reg(n[0],registers(n).first));
            if(v.kind==Kind::ray_address){if(as && *as!=v.id)unsupported("ambiguous ray address");as=v.id;}
        }
        if(!as)return false;
        if(mem && (a.size()!=3 || a[2]!="off"))unsupported("unmapped ray header load");
        auto dest=registers(a[0]);for(unsigned k=0;k<dest.count;++k)
            set(reg(dest.bank,dest.first+k),{mem?Kind::ray_node:Kind::ray_address,*as,0,dest.bank=='s'});
        if(op=="s_and_b32" || op=="s_or_b32" || op=="s_lshr_b64")set("scc",{});
        return true;
    }
    static Str hit_name(const InlineTrace& t,HitWord w){return std::format("tmpHit{}_{}_{}",t.entry,unsigned(w.field),w.n);}
    void visibility(const InlineTrace& t){
        auto resource=value(t.as_page);
        if(resource.kind!=Kind::ray_address)unsupported("missing TLAS provenance at ray loop");
        auto id=resource.id;used.insert(id);
        auto v3=[&](unsigned k){return std::format("asfloat(uint3({},{},{}))",bits(t.world[k]),bits(t.world[k+1]),bits(t.world[k+2]));};
        auto old=bits(t.tmax);
        bool closest=!t.leaf.empty();
        auto result=closest?temp("XsTriangle","(XsTriangle)0"):temp("uint3","uint3(0u,"+old+",0u)");
        line(std::format("if(active) {}={}(res{},{}u,{},{},asfloat({}),asfloat({}),{},{});",
             result,closest?"xs_closest":"xs_visibility",id,t.mask,v3(0),v3(3),bits(t.tmin),old,bits(t.limit),
             t.facing>0?"RAY_FLAG_CULL_BACK_FACING_TRIANGLES":t.facing<0?"RAY_FLAG_CULL_FRONT_FACING_TRIANGLES":"RAY_FLAG_NONE"));
        if(closest){
            for(auto& r:{t.base_lo,t.base_hi})if(auto base=value(r);base.kind!=Kind::ray_address || base.id!=id)
                unsupported("committed instance uses a different TLAS base");
            std::set<Str> written;
            for(auto& [_,words]:t.reads)for(auto w:words){
                auto name=hit_name(t,w);if(w.field==HitWord::literal || !written.insert(name).second)continue;
                Str expr;
                switch(w.field){
                case HitWord::instance_id:expr=result+".instanceID";break;
                case HitWord::instance_index:expr=result+".instanceIndex";break;
                case HitWord::sbt_offset:expr=result+".contribution";break;
                case HitWord::geometry_word:expr="("+result+".geometry<<8u)";break;
                case HitWord::transform:expr=std::format("asuint({}.transform[{}][{}])",result,w.n/4,w.n%4);break;
                case HitWord::vertex:expr=std::format("asuint({}.vertices[{}][{}])",result,w.n/3,w.n%3);break;
                default:unsupported("invalid committed field");
                }
                write(name,expr,varying,true);
            }
        }
        // Dead temporary *values* cannot escape the recognized ray walker.
        // If a successor needs another result (e.g. triangle attributes), it
        // must be explicitly represented rather than inheriting a stale VGPR.
        for(auto& r:t.clobbers)if(r!="exec_lo")set(r,{});
        set("scc",{});set("vcc",{});state.erase("word_vcc_lo");
        if(closest){
            write(t.committed,result+".hit*14u",varying,true);write(t.leaf,result+".hit*8u",varying,true);
            if(t.hit_t!=t.tmax)write(t.hit_t,result+".t",varying,true);
            write(t.tmax,result+".t",varying,true);
            if(!t.status.empty())write(t.status,result+".status",varying,true);
        }else{
            write(t.committed,result+".x",{Kind::ray_node,id,1},true);
            write(t.tmax,result+".y",varying,true);write(t.status,result+".z",varying,true);
        }
    }
    bool mask_logic32(const Instruction& i){
        if(!physical_waves() || p.wave!=32 || raw_sgprs || !i.modifiers.empty() || i.args.size()!=3)return false;
        View op=i.op;const auto& a=i.args;
        if(op!="s_or_b32" && op!="s_and_b32" && op!="s_xor_b32" && op!="s_andn2_b32")return false;
        auto gpr=[](const Str& s){return s.size()>1 && s[0]=='s' && s.find_first_not_of("0123456789",1)==s.npos;};
        if(!gpr(a[0]))return false;
        auto pred=[&](const Str& s){
            return s=="exec_lo" || (s=="vcc_lo" && value("vcc").kind==Kind::predicate) ||
                (gpr(s) && (mask_bits.contains(s) || value(s).kind==Kind::predicate));
        };
        if(!(pred(a[1]) || pred(a[2])))return false;
        for(unsigned j:{1u,2u})if(!pred(a[j]) && a[j]!="0" && a[j]!="-1")return false;
        auto x=mask(a[1]),y=mask(a[2]);
        auto bit=temp("bool","("+x+")"+(op=="s_or_b32"?" || ":op=="s_xor_b32"?" != ":op=="s_andn2_b32"?" && !":" && ")+"("+y+")");
        write("scc","uint(ANY("+bit+"))",scalar);
        scalar_dest(a[0],"group_reduce("+bit+",lane,4u)");
        // Keep an ordinary SGPR across edges and integer uses. A subsequent
        // lane-select in this block can consume the equivalent bit directly.
        mask_bits.emplace(a[0],bit);return true;
    }
    void emit(const Instruction& i) {
        emit_pc=i.pc;
        if(i.field_decode)unsupported("format-aware image ABI required: "+i.op);
        if(p.stage==geometry && gs_instruction(i))return;
        if(ray_address_op(i))return;
        raw_words.clear();
        if(mask_logic32(i))return;
        if(virtual_wave && !i.op.starts_with("s_cbranch") && !i.op.contains("saveexec") && !i.op.contains("wrexec")) {
            for(size_t k=i.op.starts_with("s_cmp")?0:1;k<i.args.size();++k){
                 auto& a=i.args[k];auto v=value(a);
                 if((a=="vcc_lo" || a=="vcc_hi") && value("word_"+a).kind==Kind::number)continue;
                bool lo=a=="vcc_lo" || a=="exec_lo" || v.kind==Kind::predicate,hi=a=="vcc_hi" || a=="exec_hi" || v.kind==Kind::mask_high;
                if((lo||hi) && !a.contains(':') && a!="vcc" && !i.op.ends_with("_b64") &&
                   i.op!="v_cndmask_b32" && !i.op.contains("_co_ci_")){
                    auto pred=a.starts_with("vcc")?"vcc":a.starts_with("exec")?"active":a;
                    if(pred!="active" && value(pred).kind!=Kind::predicate && value(pred).kind!=Kind::mask_high)continue;
                    vars.insert("raw_"+a);raw_words.insert(a);
                    line("raw_"+a+" = group_reduce("+pred+(pred=="active"?"":" != 0u")+",lane,"+(hi?"5u":"4u")+");");
                }
            }
        }
        if(i.op.ends_with("_sdwa") && subword(i))return;
        if((i.op.ends_with("_dpp") || i.op.starts_with("v_permlane")) && permute_alu(i))return;
        // Only arithmetic lowering accepts clamp/scale; memory, swizzle and
        // exports have their own validated modifier grammars.
        if(!i.modifiers.empty() && ((i.op.starts_with("s_") && i.op!="s_waitcnt") ||
           i.op.starts_with("v_cmp_") || i.op=="v_cndmask_b32" || i.op=="v_readfirstlane_b32"))
            unsupported("unhandled instruction modifiers");
        if (!(relative_move(i) || wide_alu(i) || control(i) || smem(i) || image_descriptor_alu(i) || interpolate(i) || memory(i) || export_value(i) || numeric(i)))
            unsupported("unsupported "+i.op);
    }
    Translation finish() {
        for(auto id:loaded_textures)if(!sampled_textures.contains(id))view_widths[id]=4;
        auto code=body.str();
        if(!reference_queue && (p.native_compute_wave || p.stage==pixel || p.stage==vertex))code=active_regions(std::move(code));
        Translation out{p.stage,(bit_transforms.result.empty()?Str{}:Str(BitTransforms::helper))+declarations()+code,{},virtual_wave && p.native_compute_wave?p.wave:0u,ray_mode,half_vectors||half_lds};
        for(auto [id,width]:view_widths) {
            const auto& r=p.resources[id];
            out.views.push_back(r.reg|(r.type==6?0x100u:0u)|(integer_views.contains(id)?0x200u:0u)|
                                (atomic64_images.contains(id)?view::atomic64:0u));
        }
        for(auto [id,k]:image_fields)out.views.push_back(view::key(p.resources[id].reg,k));
        for(auto [id,k]:sampler_fields)out.views.push_back(view::sampler_key(p.resources[id].reg,k));
        for(auto [sg,pair]:table_words)out.views.push_back(view::table_key(pair.first,pair.second));
        unsigned raw_slot=table_words.size();
        for(auto d:raw_reads){auto& r=p.resources[d/8];out.views.push_back(view::buffer_key(raw_slot++,d%8,r.reg|(r.type>=6?0x100u:0u)));}
        return out;
    }
    static unsigned target(const Instruction& i) {
        // SOPP's immediate is a signed dword displacement. LLVM may print the
        // underlying unsigned 16-bit field for backward branches.
        const auto& a=i.args.at(0);
        int off=a.starts_with('-')?-int(number(View(a).substr(1))):int(number(a));
        if(off < -32768 || off>65535) unsupported("invalid branch displacement");
        if(off>32767)off-=65536;
        return unsigned(std::int64_t(i.pc)+i.size+4ll*off);
    }
    void check_loop_values(const State& entry,const LoopUse& usage){
        for(auto& r:usage.live_in){
            auto initial=entry.find(r);
            if(initial==entry.end())unsupported("loop-carried word has no entry definition: "+r);
            auto v=initial->second,next=peek(r);
            if(v.kind==Kind::number && next.kind==Kind::number && (!v.uniform || next.uniform) &&
               (!usage.zero_high.contains(r) || next.part==1))continue;
            if(v.kind==next.kind && (v.kind==Kind::predicate || v.kind==Kind::mask_high) &&
               (!v.part || v.part==next.part) && (!v.uniform || next.uniform) &&
               (v.id==next.id || (v.kind==Kind::predicate && !v.id)) &&
               (!v.mask_origin || v.mask_origin==next.mask_origin))continue;
            if(v!=next)throw Unsupported(std::format("incompatible loop-carried register provenance {}: kind/id/part/uniform {}/{}/{}/{} -> {}/{}/{}/{}",
                r,int(v.kind),v.id,v.part,v.uniform,int(next.kind),next.id,next.part,next.uniform));
        }
    }
    void region(unsigned begin,unsigned end) {
        for(unsigned pc=begin;pc<end;) {
            try {
                close_blocks(pc);
                if(!reachable)unsupported("unreachable code before a branch target");
                auto i=decode(p.code,pc);
                if(pc+i.size>end) unsupported("branch target inside instruction");
                if(auto found=counted_latches.find(pc);found!=counted_latches.end() && !emitting_loops.contains(pc)){
                    auto latch=found->second;auto& c=*counted_graph;const auto& br=c.ins.at(latch);
                    if(latch+br.size>end || loop_depth>=8 || (!blocks.empty() && blocks.back().end<=latch))unsupported("counted loop crosses enclosing region");
                    // Mutable data can stop being uniform/constant on the
                    // second trip. Preserve descriptor identities only if
                    // the checked back edge proves them invariant.
                    for(auto it=c.ins.lower_bound(pc);it!=c.ins.end() && it->first<latch;++it){
                        const auto& ins=it->second;if(ins.args.empty())continue;
                        auto& d=ins.args[0];
                        if(!(vgpr(d)||d.starts_with("v[")||d.starts_with("s[")||(d.size()>1&&d[0]=='s'&&std::isdigit(static_cast<unsigned char>(d[1])))))continue;
                        if(ins.op.starts_with("s_cmp")||ins.op.contains("store")||ins.op.starts_with("s_cbranch"))continue;
                        auto range=registers(d);for(unsigned k=0;k<range.count;++k){auto n=reg(range.bank,range.first+k);auto at=state.find(n);
                            if(at==state.end())continue;
                            if(at->second.kind==Kind::number){if(range.bank=='v')at->second.uniform=false;at->second.part=0;}
                            if(range.bank=='v'){state.erase("grad_"+n);state.erase("quadword_"+n);}
                        }
                    }
                    const auto entry=state;size_t depth=blocks.size();
                    representatives.clear();accesses.emplace_back();++loop_depth;emitting_loops.insert(pc);
                    line("[loop] do {");region(pc,latch);close_blocks(latch,depth);
                    if(!reachable || blocks.size()!=depth || value("scc").kind!=Kind::number)unsupported("unresolved counted loop latch");
                    line(br.op=="s_cbranch_scc0"?"} while(!ANY(scc != 0u));":"} while(ANY(scc != 0u));");
                    auto use=std::move(accesses.back());accesses.pop_back();check_loop_values(entry,use);
                    emitting_loops.erase(pc);--loop_depth;representatives.clear();pc=latch+br.size;continue;
                }
                if(!loop_exits.empty() && i.op.starts_with("s_cbranch_") && target(i)==loop_exits.back().pc){
                    auto op=View(i.op);Str cond;
                    if(op=="s_cbranch_scc0" || op=="s_cbranch_scc1"){
                        if(value("scc").kind!=Kind::number)unsupported("unbound early loop exit SCC");
                        cond="ANY(scc!=0u)";if(op.ends_with('0'))cond="!"+cond;
                    }else if(op=="s_cbranch_execz" || op=="s_cbranch_execnz")cond=op.ends_with("execz")?"!ANY(active)":"ANY(active)";
                    else if(op=="s_cbranch_vccz" || op=="s_cbranch_vccnz")cond=(op.ends_with("vccz")?"!ANY(":"ANY(")+mask("vcc")+")";
                    else unsupported("unknown early loop exit condition");
                    auto& ex=loop_exits.back();ex.state=ex.state?join(*ex.state,state):state;ex.masked|=masked;
                    line("if ("+cond+") break;");pc+=i.size;continue;
                }
                if(i.op=="s_endpgm" && p.stage==pixel && exported.size()==p.outputs.size()) {
                    line("return output;");reachable=false;pc+=i.size;continue;
                }
                if(p.stage==pixel && !p.outputs.empty() && i.op=="exp" && i.args==std::vector<Str>{"null","off","off","off","off"} &&
                   i.modifiers=="done vm" && pc+i.size==end) {line("discard;");pc+=i.size;continue;}
                if(i.op=="s_branch") {
                    // A natural loop can enter at its body on the first trip
                    // and at a mask-restoration block on subsequent trips:
                    //      branch body
                    // back: restore loop mask
                    // body: ...; cbranch <SCC>, back
                    // Reorder the restore block onto the taken back edge.
                    auto head=target(i),back=pc+i.size;
                    if(!blocks.empty() && blocks.back().end==back && head>back && head<=end &&
                       (blocks.size()<2 || head<=blocks[blocks.size()-2].end)) {
                        // A jump over the alternate arm immediately before
                        // the conditional target is the then/else diamond.
                        auto b=std::move(blocks.back());blocks.pop_back();
                        auto then=std::move(state);bool then_mask=masked;
                        state=std::move(b.entry);masked=b.masked;
                        for(unsigned j=0;j<b.definitions.size();++j)accesses[j].defined=b.definitions[j];
                        representatives.clear();
                        line("} else {");
                        auto outer=blocks.size();
                        region(back,head);
                        // Only close nested conditionals ending here; an
                        // enclosing conditional may share the same merge PC.
                        close_blocks(head,outer);
                        if(blocks.size()!=outer)unsupported("jump crosses else region");
                        state=join(state,then);masked|=then_mask;
                        for(unsigned j=0;j<b.definitions.size();++j)accesses[j].defined=std::move(b.definitions[j]);
                        representatives.clear();line("}");pc=head;continue;
                    }
                    if(head<=back || head>=end || loop_depth>=8) unsupported("unstructured jump");
                     auto latch=head,restore_begin=back;
                    Instruction br{};
                    for(;latch<end;latch+=br.size) {
                        br=decode(p.code,latch);
                         if(br.op=="s_cbranch_scc1" || br.op=="s_cbranch_scc0"){
                             auto to=target(br);
                             if(back<=to && to<head){restore_begin=to;break;}
                         }
                        if(br.op=="s_endpgm")break;
                    }
                     if(latch>=end || !br.op.starts_with("s_cbranch_") || target(br)!=restore_begin)
                         unsupported("forward jump without a structured loop latch");
                     if(restore_begin!=back){
                         // A dword of dead alignment padding may follow the
                         // forward jump. Only the target of the actual back
                         // edge executes the EXEC restoration.
                         auto g=decode_graph(p.code);auto at=g.ins.lower_bound(back);
                         if(at!=g.ins.end() && at->first<restore_begin)unsupported("live edge into loop alignment gap");
                     }
                     if(!blocks.empty() && latch>=blocks.back().end) unsupported("loop crosses an enclosing branch");
                     // A VGPR accumulator can start from a uniform clear and
                     // become varying on the first back edge. Lower the body
                     // using the invariant fact (numeric, not uniform), even
                     // on its first trip, so READFIRSTLANE and scalar loads
                     // cannot specialize themselves to that initial value.
                     auto loop_graph=decode_graph(p.code);
                     for(auto it=loop_graph.ins.lower_bound(head);it!=loop_graph.ins.end() && it->first<latch;++it){
                         const auto& ins=it->second;
                         if(!ins.args.empty() && ins.args[0].starts_with("s[") && !View(ins.op).starts_with("s_cmp")){
                             auto rr=registers(ins.args[0]);
                             for(unsigned k=0;k<rr.count;++k)if(auto v=state.find(reg('s',rr.first+k));v!=state.end() &&
                                 (v->second.kind==Kind::predicate || v->second.kind==Kind::mask_high)){
                                 // A loop's live-lane mask commonly starts
                                 // full and loses lanes at its exit test.
                                 v->second.part=0;v->second.uniform=false;v->second.mask_origin=0;
                                 if(v->second.kind==Kind::predicate)v->second.id=0;
                             }
                         }
                         if(ins.args.empty() || !(ins.op.starts_with("v_") || ins.op.starts_with("buffer_load") ||
                            ins.op.starts_with("image_") || ins.op.starts_with("global_load") || ins.op.starts_with("ds_read")))continue;
                         auto& dest=ins.args[0];if(!vgpr(dest) && !dest.starts_with("v["))continue;
                         auto rr=registers(dest);
                         for(unsigned k=0;k<rr.count;++k){auto n=reg('v',rr.first+k);
                             if(auto v=state.find(n);v!=state.end() && v->second.kind==Kind::number){
                                 v->second.uniform=false;v->second.part=0;
                                 state.erase("grad_"+n);
                             }}
                     }
                     const auto entry=state;
                    auto nesting=blocks.size();
                    representatives.clear();++loop_depth;masked=true;
                     accesses.emplace_back();
                     loop_exits.push_back({latch+br.size,{},false});
                    line("[loop] while (true) {");
                    region(head,latch);
                    close_blocks(latch);
                    if(blocks.size()!=nesting || value("scc").kind!=Kind::number) unsupported("unresolved loop latch");
                    line(br.op=="s_cbranch_scc1"?"if (!ANY(scc != 0u)) break;":"if (ANY(scc != 0u)) break;");
                    auto exit=state;bool exit_mask=masked;
                     for(unsigned q=restore_begin;q<head;) {
                        auto restore=decode(p.code,q);
                         if(restore.op!="s_nop" && restore.op!="s_and_b64" && restore.op!="s_mov_b64" &&
                            restore.op!="v_mov_b32" && restore.op!="s_mov_b32" && restore.op!="s_and_b32")
                            unsupported("complex loop continue block");
                        emit(restore);q+=restore.size;
                        if(q>head) unsupported("loop target inside instruction");
                    }
                    // Code is statically specialized to descriptor provenance.
                    // Do not carry a changed table/SRD into code that was
                    // generated assuming a different resource on the first trip.
                     auto usage=std::move(accesses.back());accesses.pop_back();
                     check_loop_values(entry,usage);
                     auto early=std::move(loop_exits.back());loop_exits.pop_back();
                     state=early.state?join(std::move(exit),*early.state):std::move(exit);
                     masked=exit_mask||early.masked;--loop_depth;
                    representatives.clear();line("}");
                    pc=latch+br.size;continue;
                }
                emit(i);pc+=i.size;
            } catch(const Unsupported& e) {
                if (View(e.what()).contains("+0x")) throw;
                throw Unsupported(std::format("{}+0x{:x}: {}",stage_name(p.stage),pc,e.what()));
            } catch(const std::out_of_range&) {
                throw Unsupported(std::format("{}+0x{:x}: unsupported operand layout",stage_name(p.stage),pc));
            }
        }
    }
public:
    explicit Lower(const Program& p,bool canonical=false,bool counted=false):p(p),raw_sgprs(canonical) {
        if(counted){
            counted_graph=decode_graph(p.code);auto& g=*counted_graph;
            for(auto& [latch,br]:g.ins)if(br.op=="s_cbranch_scc0"||br.op=="s_cbranch_scc1"){
                auto head=target(br);if(head>=latch)continue;bool safe=true;
                for(auto& [pc,i]:g.ins){
                    bool inside=pc>=head && pc<=latch;
                    if(inside && (needs_quad_lod(i.op)||i.op=="s_endpgm"||i.op.contains("saveexec")||i.op.starts_with("v_cmpx")||
                        (!i.args.empty() && (i.args[0]=="exec" || i.args[0]=="exec_lo" || i.args[0]=="exec_hi")))){safe=false;break;}
                    if(pc==latch || (i.op!="s_branch" && !i.op.starts_with("s_cbranch_")))continue;
                    auto to=target(i);
                    if(inside?(to<head || to>latch):(to>head && to<=latch)){safe=false;break;}
                }
                if(safe)counted_latches.emplace(head,latch);
            }
            if(counted_latches.empty())unsupported("no structured counted loop");
        }
        if(p.stage==pixel){
            // Only capture gradients for VGPRs which feed an implicit-LOD
            // operation. Definitions outside full-quad execution invalidate
            // that snapshot rather than manufacturing a zero LOD.
            std::vector<Str> gaps;
             auto graph=decode_graph(p.code,&gaps);
             if(gaps.empty() && std::ranges::any_of(graph.ins,[](auto& i){return i.second.op.starts_with("v_movrel");})){
                  relative_windows=pixel_relative_windows(graph,p.code);
                 for(auto& [_,w]:relative_windows){auto& n=local_banks[w.base];n=std::max(n,w.width);}
                 unsigned end=0;for(auto [base,n]:local_banks){if(base<end)unsupported("overlapping relative VGPR banks");end=base+n;}
             }
             pixel_wave_election=p.flexible_pixel_wave && gaps.empty() && std::ranges::none_of(graph.ins,[](auto& pair){
                 auto& i=pair.second;auto op=View(i.op);
                 bool lane=op=="v_readlane_b32" || op=="v_writelane_b32";
                 if(op.starts_with("v_mbcnt")||(lane && (i.args.size()!=3 || i.args[2].empty() || !std::isdigit(static_cast<unsigned char>(i.args[2][0]))))||
                    op.starts_with("v_permlane")||op.ends_with("_dpp")||op.ends_with("_dpp8")||
                    op.starts_with("s_bcnt")||op.starts_with("s_ff")||op.starts_with("s_quadmask"))return true;
                 return std::ranges::any_of(i.args,[](auto& a){return a=="exec_lo"||a=="exec_hi"||a=="vcc_lo"||a=="vcc_hi";});
             });
            split_lane_predicates=std::ranges::any_of(graph.ins,[](auto& e){auto& i=e.second;
                return i.op=="v_readlane_b32" && (i.args[0]=="vcc_lo" || i.args[0]=="vcc_hi");});
            for(auto& [_,i]:graph.ins)if(i.op=="image_sample" || i.op=="image_sample_o" || i.op=="image_sample_b"){
                auto s=View(i.args.at(1));
                if(s.starts_with('[')){
                    s=s.substr(1,s.size()-2);while(!s.empty()){
                        auto n=s.find(',');gradient_regs.emplace(s.substr(0,n));if(n==s.npos)break;
                        s.remove_prefix(n+1);while(s.starts_with(' '))s.remove_prefix(1);
                    }
                 }else{auto r=registers(s);for(unsigned j=0;j<r.count;++j)gradient_regs.insert(reg(r.bank,r.first+j));}
             }
              if(gaps.empty() && !gradient_regs.empty()){
                  // The audit decoder returns instructions, not CFG edges.
                  std::set<unsigned> targets;track_quad_scopes=true;
                  for(auto& [pc,i]:graph.ins)if(i.op=="s_branch" || i.op.starts_with("s_cbranch_")){
                      auto to=target(i);targets.insert(to);
                      if(to<=pc)track_quad_scopes=false;
                  }
                  paired_interp=paired_quad_interpolants(graph,targets,gradient_regs);
              }
              if(gaps.empty() && !(p.ps_input&~0x3fffu) && std::ranges::none_of(graph.ins,[](const auto& e){
                 return e.second.op.starts_with("v_movrel") || e.second.op.starts_with("v_swap");})){
                 unsigned input_end=2*std::popcount(p.ps_input&0x77u)+std::popcount(p.ps_input&0x3f00u);
                 for(auto& r:gradient_regs)if(registers(r).first>=input_end)physical_quad_temps.insert(r);
                 for(auto& [_,i]:graph.ins)if(i.op=="v_writelane_b32" || i.op=="v_interp_p1_f32")
                     physical_quad_temps.erase(i.args[0]);
             }
        }
    }
    Translation group_graph(unsigned end,unsigned start=0,const std::set<unsigned>& dead_spills={},bool proved_inputs=false) {
        bool gfx=p.stage==pixel || p.stage==vertex;
        auto count=u64(p.threads[0])*p.threads[1]*p.threads[2];
        if(!gfx && (p.stage!=compute || !count || count>1024 || (p.wave!=64 && p.wave!=32) ||
           p.lds_bytes%4 || p.lds_bytes>32768-4-2*p.wave*4)) unsupported("unsupported group size or LDS allocation");
        auto graph=decode_graph(p.code.slice(0,end));
        // Buffer-producing kernels include GPU dispatch generators, page
        // transcoders and persistent device work queues. Their VM waits and
        // inter-wave producer/consumer ordering must not be changed merely
        // because the adapter supports the requested wave size. The native
        // path covers image passes and independent, write-only buffer output.
        bool stores_buffer=false,reads_buffer=false,device_queue=false,reduces=false,coherent_load=false,allocates=false;
        for(auto& [pc,i]:graph.ins){
            auto op=View(i.op);
            stores_buffer|=op.starts_with("buffer_store") || op.starts_with("global_store") || op.starts_with("flat_store");
            reads_buffer|=op.starts_with("buffer_load") || op.starts_with("global_load") || op.starts_with("flat_load");
            bool reduction=op=="buffer_atomic_add" || op=="buffer_atomic_or" || op=="buffer_atomic_xor";
            reduces|=reduction;
            coherent_load|=op.starts_with("buffer_load") && (i.modifiers.contains("glc") || i.modifiers.contains("slc"));
            allocates|=op.starts_with("buffer_atomic") && i.modifiers.contains("glc");
            device_queue|=(op.starts_with("buffer_atomic") && !reduction) || op.starts_with("global_atomic") || op.starts_with("flat_atomic") ||
                op.starts_with("image_bvh") || op=="buffer_gl0_inv" || op=="buffer_gl1_inv" ||
                i.modifiers.contains("glc") || i.modifiers.contains("slc");
        }
        // Cross-workgroup queues poll coherent words and obtain work indices
        // from returned atomics. Retain their validated PC loop, votes and
        // LDS ordering; restructuring that loop can starve its producers.
        reference_queue=!proved_inputs && ((coherent_load && allocates) || (count>p.wave && stores_buffer && (reads_buffer || reduces)));
        selective_vm=p.native_compute_wave && !reference_queue && !gfx && !proved_inputs;
        bool independent=proved_inputs || (p.native_compute_wave && !reference_queue && !device_queue && !(stores_buffer && (reads_buffer || reduces)));
        // First use the reference path's fixed-point descriptor analysis.
        // Only repartition a group if *every* buffer load on *every* incoming
        // edge resolves to an SRV/CBV. A returned atomic, a coherence flag,
        // or a mutable input rules out this proof. Keep the VM fences for
        // newly parallel output producers; bit inserts still use real atomics.
        check_buffer_inputs=!gfx && reference_queue && p.native_compute_wave && count>p.wave && count%p.wave==0 &&
            stores_buffer && std::ranges::any_of(graph.ins,[](auto& e){return e.second.op=="s_barrier";}) &&
            std::ranges::none_of(graph.ins,[](auto& e){auto& i=e.second;auto op=View(i.op);
                return i.modifiers.contains("glc") || i.modifiers.contains("slc") || i.modifiers.contains("dlc") ||
                    op.starts_with("global_") || op.starts_with("flat_") || op.starts_with("image_") ||
                    (op.starts_with("buffer_") && !op.starts_with("buffer_load") && !op.starts_with("buffer_store") &&
                     op!="buffer_atomic_or" && op!="buffer_atomic_and" && op!="buffer_atomic_xor" && op!="buffer_atomic_add") ||
                    (op.starts_with("ds_") && !op.starts_with("ds_read") && !op.starts_with("ds_write") &&
                     (op.ends_with("64") || op.starts_with("ds_wrxchg2")));});
        // Wide/pair LDS read-modify-writes use a single-wave lane-owner
        // loop to implement atomicity. Keep their reference scheduler until
        // the device can supply an equivalent native atomic operation.
        native_group=!gfx && independent && count>p.wave && count%p.wave==0 &&
            std::ranges::none_of(graph.ins,[](const auto& e){auto& op=e.second.op;
                return op.starts_with("ds_") && !op.starts_with("ds_read") && !op.starts_with("ds_write") &&
                       (op.ends_with("64") || op.starts_with("ds_wrxchg2"));});
        multi_wave=!gfx && count>p.wave && !native_group;
        // The physical 16-bit LDS layout needs no emulated dword CAS for
        // aligned halfword stores. Restrict it to one actual wave and a
        // shared allocation accessed solely by ordinary half/word transfers.
        if(!gfx && p.native_half && p.native_compute_wave && count==p.wave && !reference_queue){
            unsigned halves=0,wide_stores=0;bool supported=true;
            for(auto& [pc,i]:graph.ins){auto op=View(i.op);if(!op.starts_with("ds_"))continue;
                bool load=op.starts_with("ds_read"),store=op.starts_with("ds_write");
                bool half=op.contains("b16")||op.contains("u16")||op.contains("i16");
                bool wide=op.ends_with("_b32")||op.ends_with("_b64")||op.ends_with("_b96")||op.ends_with("_b128");
                supported&=(load||store) && (half||wide) && !op.contains("addtid");
                halves+=store&&half;wide_stores+=store&&wide;}
            half_lds=supported && halves>=8 && halves>4*wide_stores && even_half_addresses(graph,start);
        }
        virtual_wave=!gfx;masked=!gfx && !physical_waves();inputs();
        if(start) {
            if(!gfx)unsupported("non-graphics CFG prefix");
            region(0,start);close_blocks(start);
            if(!blocks.empty() || !reachable)unsupported("invalid CFG prefix boundary");
        }
        Str init=body.str();
        dispatch_lowering=true;active_bary.clear();complementary_bary.clear();
        // A full-quad sampling prefix may end inside an otherwise linear
        // block. Give its successor an actual node in both traversal orders.
        if(start && !graph.edges.contains(start)){
            auto after=graph.edges.upper_bound(start);
            if(after==graph.edges.begin() || !graph.ins.contains(start))unsupported("invalid split CFG entry");
            auto from=std::prev(after)->first;
            graph.edges[start]=std::move(graph.edges[from]);graph.edges[from]={start};
            if(!graph.order.empty()){
                auto it=std::ranges::find(graph.order,from);graph.order.insert(std::next(it),start);
            }
        }
        auto traces=inline_visibility(p,graph);
        ray_mode=!traces.empty();
        triangle_queries=std::ranges::any_of(traces,[](auto& e){return !e.second.leaf.empty();});
        const auto& ins=graph.ins;
        indexed_vgprs=!gfx && std::ranges::any_of(ins,[](const auto& e){
            return e.second.op.starts_with("v_movrel") || e.second.op=="v_swaprel_b32";});
        for(const auto& [pc,i]:ins){
            // A loop exit edge can be visited before a predecessor carrying
            // its numeric value. Seed only architecturally defined temporary
            // destinations. Descriptor/table provenance must still agree at
            // every join. Unresolved reads are checked after convergence.
            if(i.args.empty())continue;
            auto& dstname=i.args[0];
            bool vec=dstname.starts_with('v') && !dstname.starts_with("vcc") &&
                (i.op.starts_with("v_") || i.op.starts_with("ds_read") || i.op.starts_with("image_load") ||
                 i.op.starts_with("image_sample") || i.op.starts_with("buffer_load") || i.op.starts_with("global_load"));
            bool sg=!gfx && dstname.starts_with('s') && dstname.size()>1 && (std::isdigit(static_cast<unsigned char>(dstname[1]))||dstname[1]=='[') &&
                (i.op=="v_readfirstlane_b32" || i.op=="v_readlane_b32" || i.op=="s_mov_b32" || i.op.starts_with("s_buffer_load_dword"));
            if(!vec && !sg)continue;
            auto dst=registers(i.args[0]);
            for(unsigned j=0;j<dst.count;++j){auto r=reg(dst.bank,dst.first+j);
                if(!state.contains(r)){state[r]={Kind::undef_word};initial_undef.insert(r);}}
        }
        prepare_device_sync(graph);
        std::set<unsigned> heads;
        for(const auto& [h,edges]:graph.edges)heads.insert(h);
        bool ordered=gfx && !graph.order.empty();
        bool quad_regions=false;
        std::set<unsigned> repeated;
        if(gfx && !ordered){
            // A loop before or after a texture sample does not make that
            // sample part of the loop. Keep the acyclic regions outside the
            // repeated CFG, so original implicit derivatives reconverge.
            for(auto& c:components(graph.edges,start))if(c.size()>1 ||
                std::ranges::find(graph.edges.at(c[0]),c[0])!=graph.edges.at(c[0]).end())
                repeated.insert(c.begin(),c.end());
            for(auto& [pc,i]:ins)if(pc>=start && needs_quad_lod(i.op)){
                if(repeated.contains(*std::prev(heads.upper_bound(pc))))unsupported("implicit derivatives inside a cyclic graphics region");
                quad_regions=true;
            }
        }
        // Deep transmittance filters contain hundreds of short unrolled
        // EXEC diamonds plus real back edges. The fixed-point propagation
        // below still limits visits relative to the actual graph size.
        constexpr unsigned compact_cfg_limit=512;
        if(heads.size()>(gfx?compact_cfg_limit:2048u))unsupported("CFG exceeds block limit");
        for(auto h:heads)if(!ins.contains(h))unsupported("branch target inside a machine instruction");
        std::optional<MaskEdges> threads;
        if(!gfx)threads.emplace(graph,p.wave,end);
        if(!gfx && p.native_compute_wave && !reference_queue && !ray_mode){
            for(auto h:heads){std::vector<const Instruction*> block;
                for(auto it=ins.find(h);it!=ins.end();++it){const auto& i=it->second;block.push_back(&i);
                    if(i.op=="s_endpgm" || i.op=="s_branch" || i.op.starts_with("s_cbranch_") || heads.contains(i.pc+i.size))break;}
                bit_transforms.analyze(block);}
        }
        // Meet the abstract register types at each block. The actual numeric
        // values are ordinary mutable registers spanning the switch; we can
        // specialize loads to an SRD only when every incoming edge agrees.
        std::map<unsigned,State> states{{start,state}};
        std::map<unsigned,bool> may_mask{{start,masked}};
        std::map<unsigned,std::set<unsigned>> outputs{{start,exported}};
        std::map<unsigned,Str> cases;
        // A single 500-way loop/switch makes every live word appear to
        // survive every back edge in the desktop compiler. Only genuine
        // cycles need loops when a group is one guest wave.
        // Vertex shaders have no implicit quad derivatives. Close their
        // actual loops at the resolved back edges instead of carrying every
        // live vertex register around one switch (e.g. material tile scans).
        const bool vertex_loops=p.stage==vertex && !ordered;
        const bool partition=vertex_loops || quad_regions || (!gfx && !multi_wave && (physical_waves() || heads.size()>compact_cfg_limit));
        const bool phases=native_group && std::ranges::any_of(ins,[](auto& e){return e.second.op=="s_barrier";});
        std::map<unsigned,unsigned> barrier_edges;
        std::map<unsigned,std::vector<unsigned>> resolved_edges;
        std::map<unsigned,Str> resolved_predicates;
        std::map<unsigned,Names> missing;
        std::deque<unsigned> todo{start};
        unsigned iterations=0;
        while(!todo.empty()) {
            if(++iterations>(heads.size()+(threads?threads->clones.size():0))*16)unsupported("CFG did not converge");
            unsigned h=todo.front();todo.pop_front();
            unsigned original=h;std::optional<unsigned> known;
            if(threads)if(auto it=threads->clones.find(h);it!=threads->clones.end()){
                original=it->second.first;known=it->second.second;
            }
            auto dest=[&](unsigned to){return threads?threads->dest(original,to):to;};
            state=states.at(h);representatives.clear();mask_bits.clear();provisional_reads.clear();masked=may_mask.at(h);exported=outputs[h];arm=0;lds_read_sequence=false;
            // Each graphics PC is chosen by a whole-wave scalar vote. An
            // acyclic block is visited once with the quad reconverged, before
            // any per-lane guard on the memory operation. A masked MOV keeps
            // inactive lanes' old register bits; differentiate the actual
            // merged VGPR, not just its new active-lane operand.
            quad_uniform=gfx?(ordered || !repeated.contains(original)):h==0;
            body.str({});body.clear();
            if(partition)resolved_edges[h].clear();
            auto edge=[&](unsigned to) {
                if(partition)resolved_edges[h].push_back(to);
                auto [it,added]=states.try_emplace(to,state);
                if(!added) {
                    auto merged=join(it->second,state);
                    std::set<unsigned> common;
                    std::set_intersection(outputs[to].begin(),outputs[to].end(),exported.begin(),exported.end(),std::inserter(common,common.end()));
                    if(merged==it->second && (!masked || may_mask[to]) && common==outputs[to])return;
                    it->second=std::move(merged);
                    outputs[to]=std::move(common);may_mask[to]|=masked;
                } else {outputs[to]=exported;may_mask[to]=masked;}
                if(std::ranges::find(todo,to)==todo.end())todo.push_back(to);
            };
            for(unsigned pc=original;;) {
                const auto& i=ins.at(pc);unsigned next=pc+i.size;
                try {
                    hit_post=nullptr;
                    for(auto& [_,t]:traces)if(t.post_begin<=pc && pc<t.post_end){hit_post=&t;break;}
                    if(auto trace=traces.find(pc);trace!=traces.end()){
                        visibility(trace->second);auto to=dest(trace->second.end);edge(to);
                        line(std::format("pc={}u; break;",to));break;
                    }
                    if(i.op=="s_endpgm") {
                        if(gfx && exported.size()!=p.outputs.size())unsupported("incomplete graphics CFG exports");
                        line(gfx?"return output;":multi_wave?"pc=0xffffffffu; break;":phases?"resume=0xffffffffu; pc=0xffffffffu;":"return;");break;
                    }
                    if(p.stage==pixel && !p.outputs.empty() && i.op=="exp" && i.args==std::vector<Str>{"null","off","off","off","off"} &&
                       ins.contains(next) && ins.at(next).op=="s_endpgm"){line("discard; return (Output)0;");break;}
                    if(i.op=="s_branch") {
                        unsigned to=dest(target(i));edge(to);line(std::format("pc = {}u;{}",to,ordered?"":" break;"));break;
                    }
                     if(i.op.starts_with("s_cbranch_")) {
                         if(hit_post)if(auto e=hit_post->post_edges.find(pc);e!=hit_post->post_edges.end()){
                             auto to=dest(e->second);edge(to);line(std::format("pc = {}u; break;",to));break;
                         }
                        if(known){
                            auto to=dest(*known);edge(to);line(std::format("pc = {}u; break;",to));break;
                        }
                        Str cond;
                        if(i.op=="s_cbranch_execz" || i.op=="s_cbranch_execnz")cond="ANY(active)";
                        else if(i.op=="s_cbranch_vccz" || i.op=="s_cbranch_vccnz")cond="ANY("+mask("vcc")+")";
                        else if(i.op=="s_cbranch_scc0" || i.op=="s_cbranch_scc1") {
                            if(value("scc").kind!=Kind::number)unsupported("unbound SCC at branch");
                            cond=scalar_condition();
                        } else unsupported("unsupported scalar branch condition");
                         if(i.op.ends_with("execz") || i.op.ends_with("vccz") || i.op.ends_with('0'))cond="!"+cond;
                         if(partition)resolved_predicates[h]=cond;
                        unsigned to=dest(target(i));
                        if(next>=end)unsupported("conditional jump falls off allocation");
                        auto fall=dest(next);edge(to);edge(fall);
                        line(std::format("pc = {} ? {}u : {}u;{}",cond,to,fall,ordered?"":" break;"));break;
                    }
                    if(i.op=="s_barrier") {
                        if(gfx)unsupported("workgroup barrier in graphics");
                        if(phases){auto to=dest(next);barrier_edges[h]=to;edge(to);line(std::format("pc = {}u; break;",to));break;}
                        if(multi_wave){auto to=dest(next);edge(to);line(std::format("waiting=true; pc={}u; break;",to));break;}
                        line("GroupMemoryBarrierWithGroupSync();");
                    }
                    else if(!dead_spills.contains(pc)){
                        if(auto it=bit_transforms.saved.find(pc);it!=bit_transforms.saved.end())for(auto& s:it->second)
                            line("uint "+s.name+" = "+bits(reg('v',s.vg))+";");
                        emit(i);
                        if(auto it=bit_transforms.result.find(pc);it!=bit_transforms.result.end())write(i.args[0],it->second,varying,true);
                    }
                    if(heads.contains(next)) {auto to=dest(next);edge(to);line(std::format("pc = {}u;{}",to,ordered?"":" break;"));break;}
                    if(next>=end)unsupported("no end of program on reachable path");
                    pc=next;
                } catch(const Unsupported& e) {throw Unsupported(std::format("{}+0x{:x}: {}",stage_name(p.stage),pc,e.what()));}
            }
            cases[h]=body.str();missing[h]=provisional_reads;
        }
        for(const auto& [h,reads]:missing)if(!reads.empty()){
            if(!gfx && dead_spills.empty()){
                auto dead=dead_spill_transfers(graph,p.wave);
                if(!dead.empty())return Lower(p,raw_sgprs).group_graph(end,start,dead,proved_inputs);
            }
            unsupported(std::format("block +0x{:x} reads undefined temporary {}",h,*reads.begin()));
        }
        if(check_buffer_inputs){
            bool readonly=true,resolved=true;std::set<unsigned> mutable_inputs;
            for(auto& [pc,i]:graph.ins)if(i.op.starts_with("buffer_load") || i.op.starts_with("s_buffer_load")){
                if(!resolved_buffers.contains(pc))resolved=false;
                if(resolved_buffers.contains(pc) && !vm_access.contains(pc))continue;
                mutable_inputs.insert(pc);
                if(std::getenv("FROSTY_TRACE_LOWER"))std::fprintf(stderr,"group proof: mutable or unresolved input at +0x%x\n",pc);
                readonly=false;
            }
            bool bit_inserts=std::ranges::any_of(graph.ins,[](auto& e){return e.second.op=="buffer_atomic_and";}) &&
                             std::ranges::any_of(graph.ins,[](auto& e){return e.second.op=="buffer_atomic_or";});
            if(readonly || (p.native_group_id && resolved && bit_inserts && independent_output_loops(graph,start,resolved_buffers,mutable_inputs)))
                try{return Lower(p,raw_sgprs).group_graph(end,start,dead_spills,true);}
            catch(const Unsupported& why){if(std::getenv("FROSTY_TRACE_LOWER"))std::fprintf(stderr,"group proof: %s\n",why.what());}
        }
        if(selective_vm)resolve_vm_waits(graph,start);
        body.str({});body.clear();
        if(multi_wave){
            // All physical lanes execute one guest wave at a time. Interleave
            // blocks fairly: a spin on another wave does not starve its owner.
            // Park at S_BARRIER until all live waves reach that generation.
            unsigned waves=unsigned((count+p.wave-1)/p.wave);
            body<<std::format("    WaveState bank[{}];\n    [loop] for(uint w=0;w<{}u;++w) {{\n",waves,waves)
                <<std::format("    uint guest_index=w*LANES+lane; uint3 local=uint3(guest_index%{}u,(guest_index/{}u)%{}u,guest_index/{}u);\n",
                    p.threads[0],p.threads[0],p.threads[1],p.threads[0]*p.threads[1])
                <<init<<"    bank[w]=(WaveState)0;\n"<<std::format("    bank[w].active=guest_index<{}u;\n",count);
            for(auto& r:vars)body<<"    bank[w]."<<r<<"="<<r<<";\n";
            body<<"    }\n    [loop] for(;;) { bool live=false,ready=false;\n"
                <<std::format("    [loop] for(uint w=0;w<{}u;++w) {{\n",waves)
                <<"    uint pc=bank[w].pc; bool waiting=bank[w].waiting;\n"
                <<std::format("    bool member=w*LANES+lane<{}u;\n",count)
                <<"    if(pc==0xffffffffu)continue;\n    if(!waiting){ active=bank[w].active;\n";
            if(indexed_vgprs)body<<"    [loop] for(uint k=0u;k<256u;++k)vgpr_file[k]=bank[w].vgpr_file[k];\n";
            for(auto& r:vars)if(!indexed_vgprs || !vgpr(r))body<<"    "<<r<<"=bank[w]."<<r<<";\n";
            body<<"    switch(pc){\n";
            for(auto& [pc,text]:cases)body<<"    case "<<pc<<"u:{\n"<<text<<"    }\n";
            body<<"    default: return;\n    }\n    bank[w].pc=pc; bank[w].active=active; bank[w].waiting=waiting;\n";
            if(indexed_vgprs)body<<"    [loop] for(uint k=0u;k<256u;++k)bank[w].vgpr_file[k]=vgpr_file[k];\n";
            for(auto& r:vars)if(!indexed_vgprs || !vgpr(r))body<<"    bank[w]."<<r<<"="<<r<<";\n";
            body<<"    }\n    live|=pc!=0xffffffffu; ready|=(pc!=0xffffffffu && !waiting);\n    }\n"
                <<"    if(!live)return;\n    if(!ready){GroupMemoryBarrierWithGroupSync();\n"
                <<std::format("    [loop] for(uint w=0;w<{}u;++w) bank[w].waiting=false;\n",waves)<<"    }\n    }\n}\n";
            return finish();
        }
        body<<init;
        if(phases){
            emit_wave_phases(body,cases,resolved_edges,resolved_predicates,barrier_edges,start,device_sync,graph,*threads,
                !ray_mode && !raw_sgprs && !reference_queue && dead_spills.empty());body<<"}\n";return finish();
        }
        body<<std::format("    uint pc={}u;\n",start);
        if(partition){
            emit_flow_regions(body,cases,resolved_edges,resolved_predicates,start,vertex_loops || (!gfx && !reference_queue));
            if(gfx)body<<"    return (Output)0; // unreachable after terminal export\n";
            body<<"}\n";
        }else if(ordered) {
            // Acyclic CFG: walk blocks in topological order. Each
            // scalar branch is voted by the wave, including texture regions.
            for(auto pc:graph.order)if(cases.contains(pc))body<<"    if(pc=="<<pc<<"u) {\n"<<cases.at(pc)<<"    }\n";
            body<<"    return (Output)0; // unreachable after terminal exports\n}\n";
        } else if(!gfx && reference_queue && p.native_compute_wave && count==p.wave && cases.size()>=48){
            emit_pc_tree(body,cases);
        } else {
            body<<"    [loop] for (;;) {\n      switch (pc) {\n";
            for(auto& [pc,text]:cases)body<<"      case "<<pc<<"u: {\n"<<text<<"      }\n";
            body<<"      default: "<<(gfx?"return (Output)0;":"return;")<<"\n      }\n    }\n}\n";
        }
        return finish();
    }
    Translation run() {
        auto end=unsigned(p.code.size());
        if(p.stage==compute) {
            // The allocation can end with an inline data table, duplicate
            // ENDPGMs and CODE_END padding. Decode only control-flow-reachable
            // words, but retain the full allocation for PC-relative loads.
            auto graph=decode_graph(p.code);
            if(graph.ins.empty() || std::ranges::none_of(graph.ins,[](auto& e){return e.second.op=="s_endpgm";}))
                unsupported("no reachable s_endpgm");
            const auto& last=graph.ins.rbegin()->second;end=last.pc+last.size;
            bool needs_graph=p.lds_bytes!=0;
            unsigned expected=0;
            for(const auto& [pc,i]:graph.ins) {
                if(needs_graph)break;
                needs_graph=pc!=expected || (i.op=="s_endpgm" && pc!=last.pc) ||
                    ((i.op.starts_with("s_cbranch_") || i.op=="s_branch") && target(i)<=pc) ||
                    i.op.starts_with("ds_") || i.op=="s_barrier" || i.op.starts_with("v_mbcnt") ||
                    i.op.ends_with("_dpp") || i.op.starts_with("v_permlane") ||
                    i.op.starts_with("v_movrel") || i.op=="v_swaprel_b32" ||
                    i.op=="v_readfirstlane_b32" || i.op=="v_readlane_b32" || i.op=="v_writelane_b32" ||
                    i.op.starts_with("s_bcnt1_i32_b64") || i.op.starts_with("s_ff1_i32_b64") ||
                    (i.op.starts_with("s_") && std::ranges::any_of(i.args,[](const Str& r){
                    return r=="vcc_lo" || r=="vcc_hi" || r=="exec_lo" || r=="exec_hi";}));
                expected=pc+i.size;
            }
            if(needs_graph)return group_graph(end);
            prepare_device_sync(graph);
        } else {
            // Graphics allocations may also put PC-relative literal tables
            // and padding between the true return and a duplicate ENDPGM.
            // Keep p.code intact for literal loads; stop decoding at the
            // last control-flow-reachable instruction, not the allocation end.
            auto graph=decode_graph(p.code);auto& last=graph.ins.rbegin()->second;
            if(last.op=="s_endpgm")end=last.pc+last.size;
            else {
                // An out-of-line arm can end in a jump *back* to the common
                // export epilogue. Keep the allocation bound for that CFG.
                while(end>=4 && p.code.at<u32>(end-4)==0xbf9f0000)end-=4;
                if(end<4 || p.code.at<u32>(end-4)!=0xbf810000)unsupported("no terminal s_endpgm");
            }
        }
        inputs();
        region(0,end-4);close_blocks(end-4);
        if(!blocks.empty())unsupported("branch past end of program");
        if(p.stage==geometry && (!gs_emitted || !(gs_flags&16) || !gs_values.empty()))unsupported("incomplete geometry stream");
        if((p.stage==vertex || p.stage==pixel) && (exported.size()!=p.outputs.size() || (!p.fetch.empty()&&!fetch_called)))
            unsupported("incomplete stage exports or fetch");
        if(p.stage==vertex || p.stage==pixel)line("return output;");
        body<<"}\n";return finish();
    }
};
#include "alu_families.inl"
#include "quad_mask_fold.inl"
}

void emit_resolved_flow(std::ostream& o,const std::map<unsigned,std::string>& blocks,
        const std::map<unsigned,std::vector<unsigned>>& edges,const std::map<unsigned,std::string>& conditions,unsigned entry){
    emit_flow_regions(o,blocks,edges,conditions,entry,true);
}
Translation rectangle_assembly(const Shader& shader){
    if(shader.stage!=vertex)unsupported("rectangle assembly requires a vertex signature");
    auto obj=shader.machine_object();auto p=interface(shader,obj);
    auto pos=std::ranges::find(p.outputs,1u,&Semantic::system);
    if(pos==p.outputs.end() || pos->type!=3 || pos->mask!=15)unsupported("rectangle requires a clip-space position");
    std::ostringstream s;s<<"struct V {\n";
    constexpr View modes[]{"","nointerpolation ","","centroid ","noperspective ","noperspective centroid ","sample ","noperspective sample "};
    for(auto& a:p.outputs){
        if(a.interpolation>=std::size(modes))unsupported("rectangle interpolation mode");
        s<<modes[a.interpolation]<<type(a)<<" "<<field(a)<<":"<<a.name<<a.index<<";\n";
    }
    s<<"};\n[maxvertexcount(4)] void main(triangle V p[3], inout TriangleStream<V> strip){\n";
    auto r=field(*pos);
    s<<std::format("float4 p0=p[0].{0},p1=p[1].{0},p2=p[2].{0};\n",r)
     <<"float2 n0=p0.xy/p0.w,n1=p1.xy/p1.w,n2=p2.xy/p2.w;\n"
     <<"uint c=all(n1!=n2)?0u:all(n0!=n2)?1u:2u;\n"
     <<"V corner=p[c],a=p[(c+1)%3],b=p[(c+2)%3],d=(V)0;\n"
     <<std::format("float ia=rcp(a.{0}.w),ib=rcp(b.{0}.w),ic=rcp(corner.{0}.w),w=rcp(ia+ib-ic);\n",r);
    for(const auto& o:p.outputs){
        auto f=field(o);
        if(o.type!=3 || o.interpolation==1 || o.system==4)
            s<<std::format("corner.{0}=a.{0}=b.{0}=d.{0}=p[0].{0};\n",f);
        else if(o.system==1)s<<std::format("d.{0}=float4(w*(a.{0}.xyz*ia+b.{0}.xyz*ib-corner.{0}.xyz*ic),w);\n",f);
        else if(o.interpolation==4 || o.interpolation==5 || o.interpolation==7)
            s<<std::format("d.{0}=a.{0}+b.{0}-corner.{0};\n",f);
        else s<<std::format("d.{0}=w*(a.{0}*ia+b.{0}*ib-corner.{0}*ic);\n",f);
    }
    s<<"strip.Append(corner);strip.Append(a);strip.Append(b);strip.Append(d);strip.RestartStrip();\n}\n";
    return {geometry,s.str(),{}};
}

Translation recompile(const Shader& s,unsigned caps) {
    if (!s.xbch.size()) throw Unsupported("no Xbox machine program");
    auto obj=s.machine_object();
    auto p=interface(s,obj);
    p.native_compute_wave=p.stage==compute && (caps&(p.wave==64?2u:1u)) &&
        u64(p.threads[0])*p.threads[1]*p.threads[2]>=p.wave;
    p.native_half=p.stage==compute && (caps&4u)!=0;
    p.native_group_id=p.native_compute_wave && (caps&8u)!=0;
    return recompile(p);
}
Translation recompile(const Program& p) {
    try {return Lower(p).run();}
    catch(const Unsupported& e) {
        View reason(e.what());
        bool trace=std::getenv("FROSTY_TRACE_LOWER")!=nullptr;
        if(trace)std::fprintf(stderr,"structured: %s\n",e.what());
        bool derivative=reason.contains("implicit derivatives without dominating full-quad coordinates");
        bool undefined_vector=reason.contains("unbound numeric register v");
        if(p.stage==pixel && (derivative || undefined_vector)){
            if(auto reduced=fold_quad_mask_edges(p)){
                Program q=p;q.code={*reduced};auto g=decode_graph(q.code,nullptr,q.wave);
                auto& last=g.ins.rbegin()->second;
                // A provably empty entry mask can leave an unreachable use
                // of an uninitialized VGPR in DXC's CFG. Keep the original
                // null-export epilogue instead of initializing that VGPR.
                bool null_only=std::ranges::any_of(g.ins,[](auto& x){return x.second.op=="exp";}) &&
                    std::ranges::none_of(g.ins,[](auto& x){return x.second.op=="exp" && (x.second.args.empty() || x.second.args[0]!="null");});
                if(derivative || null_only){
                    if(trace)std::fprintf(stderr,"quad-mask proof: %zu reachable instructions\n",g.ins.size());
                    return Lower(q).group_graph(last.pc+last.size);
                }
            }
        }
        if(p.stage==compute && (p.wave==32 || p.wave==64) && (u64(p.threads[0])*p.threads[1]*p.threads[2])%p.wave==0 &&
           (reason.contains("unbound numeric register s") || reason.contains("non-predicate used as a lane mask"))){
            auto end=unsigned(p.code.size());
            while(end>=4 && p.code.at<u32>(end-4)==0xbf9f0000)end-=4;
            return Lower(p,true).group_graph(end);
        }
        // A pair of complementary arms can define a VGPR inside an already
        // masked compute region. The structured path only tracks a full-wave
        // split. Use the CFG's lane-preserving joins for that case as well.
        const bool masked_temp=(p.stage==pixel||p.stage==vertex||p.stage==compute) && reason.contains("unbound numeric register v");
        auto early_exit=p.stage==compute && reason.contains("unsupported s_endpgm");
        if((p.stage!=pixel && p.stage!=vertex && p.stage!=compute) || (!masked_temp && !early_exit && !reason.contains("edge") && !reason.contains("jump") &&
           !reason.contains("branch target") && !reason.contains("conditional export") && !reason.contains("alternate pixel exit") &&
           !reason.contains("loop-carried") && !reason.contains("loop latch") && !reason.contains("loop continue block")))throw;
        auto end=unsigned(p.code.size());
        while(end>=4 && p.code.at<u32>(end-4)==0xbf9f0000)end-=4;
        // A compute shader without LDS or a back edge can still branch
        // across a diamond. The full CFG keeps those SGPR/VGPR joins and the
        // wave-wide SCC/EXEC branch votes; a nested if cannot express them.
        if(p.stage==compute)return Lower(p).group_graph(end);
        if(p.stage==pixel && reason.contains("edge"))try{return Lower(p,false,true).run();}
        catch(const Unsupported& ex){if(trace)std::fprintf(stderr,"natural loop: %s\n",ex.what());}
        // Keep a successfully structured prefix (including complementary EXEC
        // definitions) when only a later region needs the general dispatcher.
        // A valid cut is its suffix's sole entry and has no edge back out.
        auto at=reason.find("+0x");unsigned fault=0;
        if(at!=View::npos){
            auto rest=reason.substr(at+3);std::from_chars(rest.data(),rest.data()+rest.size(),fault,16);
        }
        if(fault){
            auto graph=decode_graph(p.code.slice(0,end),nullptr,p.wave);unsigned cut=0;
            for(auto& [h,_]:graph.edges)if(h && h<=fault){
                bool valid=std::ranges::none_of(graph.edges,[&](auto& block){
                    return std::ranges::any_of(block.second,[&](unsigned t){return block.first<h?t>h:t<h;});});
                if(valid)cut=h;
            }
            if(cut)try{return Lower(p).group_graph(end,cut);}
            catch(const Unsupported& ex){if(trace)std::fprintf(stderr,"CFG after +0x%x: %s\n",cut,ex.what());}
            if(p.stage==pixel && graph.order.empty()){
                unsigned after_samples=0;
                for(auto& [pc,i]:graph.ins)if(needs_quad_lod(i.op))after_samples=pc+i.size;
                // Test the branch instruction's PC, not the start of its
                // basic block: the first branch can be *after* the last
                // implicit sample in that same block. No edge may re-enter
                // the prefix or bypass the new entry.
                if(after_samples && after_samples<=fault && graph.ins.contains(after_samples) &&
                   std::ranges::none_of(graph.ins,[&](const auto& pair){
                       const auto& i=pair.second;if(i.op!="s_branch" && !i.op.starts_with("s_cbranch_"))return false;
                       auto off=std::int16_t(p.code.at<u32>(i.pc)&0xffffu);
                       auto to=std::int64_t(i.pc)+i.size+4ll*off;
                       return i.pc<after_samples?to>after_samples:to<after_samples;
                   }))try{return Lower(p).group_graph(end,after_samples);}
                   catch(const Unsupported& ex){if(trace)std::fprintf(stderr,"CFG after quad samples: %s\n",ex.what());}
            }
        }
        return Lower(p).group_graph(end);
    }
}
} // namespace frosty::isa
