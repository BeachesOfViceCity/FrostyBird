// A barrier loop with a literal scalar trip count need not carry the whole
// VGPR file around an interpreter switch. Prove the next rendezvous for all
// waves, starting each generation with only facts shared by every incoming
// path. Neither a wave-uniform value nor an elected lane is a group constant.
struct BarrierStep {
    unsigned entry;
    std::map<unsigned,std::vector<unsigned>> jumps;
};
std::optional<std::vector<BarrierStep>> literal_barriers(const Graph& graph,const MaskEdges& clones,
        const std::map<unsigned,std::vector<unsigned>>& edges,const std::map<unsigned,unsigned>& stops,unsigned entry){
    using Facts=std::map<unsigned,u32>;
    constexpr unsigned scc=106,done=~0u;
    auto sg=[](View s){return s.size()>1 && s[0]=='s' && (s[1]=='[' || (s[1]>='0' && s[1]<='9'));};
    auto meet=[](Facts& a,const Facts& b){
        return std::erase_if(a,[&](const auto& kv){auto i=b.find(kv.first);return i==b.end() || i->second!=kv.second;})!=0;
    };
    auto word=[&](View s,const Facts& f)->std::optional<u32>{
        if(sg(s)){auto r=registers(s);if(auto i=f.find(r.first);r.count==1 && i!=f.end())return i->second;return {};}
        if(s=="null")return 0;
        bool neg=s.starts_with('-');if(neg)s.remove_prefix(1);
        int radix=s.starts_with("0x")?16:10;if(radix==16)s.remove_prefix(2);
        u32 n{};auto [p,e]=std::from_chars(s.data(),s.data()+s.size(),n,radix);
        if(e!=std::errc{} || p!=s.data()+s.size())return {};
        return neg?0u-n:n;
    };
    // Single-destination integer/float instructions preserve SCC. Only
    // explicit scalar results (e.g. READFIRSTLANE, CMP) lose that word.
    static const std::set<View> valu{"v_add_f32","v_sub_f32","v_mul_f32","v_mul_legacy_f32","v_fma_f32","v_fmac_f32",
        "v_mov_b32","v_cndmask_b32","v_lshlrev_b32","v_lshrrev_b32","v_bfe_u32","v_add_lshl_u32","v_lshl_add_u32",
        "v_cvt_u32_f32","v_cvt_f32_u32","v_add_nc_u32","v_sub_nc_u32","v_subrev_nc_u32","v_mul_lo_u32","v_mul_hi_u32",
        "v_or_b32","v_xor_b32","v_and_b32","v_readfirstlane_b32","v_readlane_b32","v_rcp_f32","v_rcp_iflag_f32",
        "v_sin_f32","v_cos_f32","v_trunc_f32"};
    auto apply=[&](const Instruction& i,Facts& f){
        View op=i.op;const auto& a=i.args;
        if(op=="s_barrier" || op=="s_branch" || op.starts_with("s_cbranch_") || op=="s_endpgm" ||
           op=="s_waitcnt" || op=="s_nop" || op=="v_nop" || op=="s_inst_prefetch")return;
        if(op.starts_with("ds_read") || op.starts_with("ds_write") || op.starts_with("image_load") ||
           op.starts_with("image_store") || op.starts_with("buffer_store") || op.starts_with("buffer_load"))return;
        std::optional<u32> x,y;
        const bool cmp=op.starts_with("s_cmp");
        if(i.modifiers.empty()){
            unsigned arg=cmp?0:1;
            if(a.size()>arg)x=word(a[arg],f);
            if(a.size()>arg+1)y=word(a[arg+1],f);
        }
        if(cmp){
            f.erase(scc);
            if(x && y && (!op.starts_with("s_cmpk_") || *y<=0x7fff)){
                if(op=="s_cmp_eq_u32")f[scc]=*x==*y;
                else if(op=="s_cmp_lg_u32")f[scc]=*x!=*y;
                else if(op=="s_cmp_lt_u32" || op=="s_cmpk_lt_u32")f[scc]=*x<*y;
                else if(op=="s_cmp_ge_u32")f[scc]=*x>=*y;
                else if(op=="s_cmp_gt_u32" || op=="s_cmpk_gt_u32")f[scc]=*x>*y;
            }
            return;
        }
        auto kill=[&]{if(!a.empty() && a[0]=="scc")f.erase(scc);
            else if(!a.empty() && sg(a[0])){auto r=registers(a[0]);for(unsigned j=0;j<r.count;++j)f.erase(r.first+j);}};
        if(valu.contains(op) || op.starts_with("v_cmp_") || op.starts_with("s_load_dword") ||
           op.starts_with("s_buffer_load_dword") || op=="s_mov_b64"){kill();return;}
        if(op=="s_mov_b32" || op=="s_lshl_b32" || op=="s_lshr_b32" || op=="s_add_u32" || op=="s_add_i32" ||
           op=="s_sub_u32" || op=="s_sub_i32" || op=="s_and_b32" || op=="s_mul_i32"){
            kill();if(op!="s_mov_b32")f.erase(scc);
            if(a.empty() || !sg(a[0]) || !x || (op!="s_mov_b32" && !y))return;
            auto r=registers(a[0]);if(r.count!=1)return;
            if(op=="s_mov_b32")f[r.first]=*x;
            else if(op=="s_lshl_b32")f[r.first]=*x<<(*y&31);
            else if(op=="s_lshr_b32")f[r.first]=*x>>(*y&31);
            else if(op=="s_and_b32")f[r.first]=*x & *y;
            else if(op=="s_mul_i32")f[r.first]=*x * *y;
            else f[r.first]=op.starts_with("s_add")?*x+*y:*x-*y;
            return;
        }
        if(op=="s_and_b64" || op=="s_andn2_b64" || op=="s_or_b64" || op=="s_xor_b64" || op=="s_and_saveexec_b64" ||
           op=="s_cselect_b64" || op=="s_cselect_b32" || op=="s_mul_hi_u32"){kill();f.erase(scc);return;}
        f.clear();
    };
    std::vector<BarrierStep> result;Facts incoming;size_t visits=0,copies=0;
    while(entry!=done){
        if(result.size()>=128)return {};
        auto& step=result.emplace_back(BarrierStep{entry,{}});
        std::map<unsigned,Facts> in{{entry,incoming}};std::deque<unsigned> queue{entry};
        std::optional<unsigned> next;Facts shared;
        while(!queue.empty()){
            if(++visits>edges.size()*18+128)return {};
            auto h=queue.front();queue.pop_front();auto f=in.at(h);auto original=h;
            if(auto c=clones.clones.find(h);c!=clones.clones.end())original=c->second.first;
            View op;
            for(auto it=graph.ins.find(original);it!=graph.ins.end();++it){auto& i=it->second;op=i.op;apply(i,f);
                if(op=="s_endpgm" || op=="s_branch" || op.starts_with("s_cbranch_") || graph.edges.contains(i.pc+i.size))break;}
            auto& out=step.jumps[h];out=edges.at(h);
            if(auto b=stops.find(h);b!=stops.end() || out.empty()){
                auto to=b==stops.end()?done:b->second;
                if(next && *next!=to)return {};
                if(!next){shared=f;next=to;}else meet(shared,f);
                out.clear();continue;
            }
            if(out.size()==2 && f.contains(scc) && (op=="s_cbranch_scc0" || op=="s_cbranch_scc1")){
                unsigned to=out[f.at(scc)==unsigned(op=="s_cbranch_scc1")?0:1];out={to};
            }
            for(auto to:out){auto [it,fresh]=in.try_emplace(to,f);
                if(fresh || meet(it->second,f))queue.push_back(to);}
        }
        if(!next || (copies+=step.jumps.size())>4*edges.size()+32)return {};
        entry=*next;incoming=std::move(shared);
    }
    return result;
}
