// A native halfword LDS array is legal only if every actual halfword access
// is aligned. Track known-zero low bits across the original CFG. Vector
// facts hold on the current EXEC lanes; widening/changing EXEC drops them.
bool even_half_addresses(const Graph& graph,unsigned start){
    using Facts=std::bitset<362>;
    auto range=[](View s)->std::optional<Range>{
        if(s.size()<2 || (s[0]!='s' && s[0]!='v') || (!std::isdigit(static_cast<unsigned char>(s[1])) && s[1]!='['))return {};
        return registers(s);
    };
    auto ix=[](Range r){return r.first+(r.bank=='s'?256u:0u);};
    bool aligned=true;
    auto visit=[&](unsigned h,Facts facts,bool verify){
        for(auto it=graph.ins.find(h);it!=graph.ins.end();++it){
            auto& i=it->second;View op=i.op;auto& a=i.args;
            auto even=[&](View s){
                if(auto r=range(s))return r->count==1 && facts[ix(*r)];
                if(s=="null")return true;
                if(s.starts_with("lit(") && s.ends_with(')'))s=s.substr(4,s.size()-5);
                if(s.starts_with('-'))s.remove_prefix(1);
                if(s.empty() || !std::isdigit(static_cast<unsigned char>(s[0])) || s.contains('.'))return false;
                return !(number(s)&1u);
            };
            bool half=op.starts_with("ds_") && (op.contains("b16") || op.contains("i16") || op.contains("u16"));
            if(verify && half){
                auto mod=View(i.modifiers);
                aligned&=even(a.at(op.starts_with("ds_read")?1:0)) && (mod.empty() || (mod.starts_with("offset:") && !(number(mod.substr(7))&1u)));
            }
            if(op.starts_with("v_cmpx_") || op.contains("saveexec") || op.contains("wrexec") ||
               (!a.empty() && (a[0]=="exec" || a[0]=="exec_lo" || a[0]=="exec_hi")))
                for(unsigned j=0;j<256;++j)facts.reset(j);
            if(op.contains("movrel") || op.contains("swaprel")){facts.reset();if(verify)aligned=false;}
            else if(!a.empty() && !op.starts_with("ds_write") && !op.contains("_store") && !op.starts_with("s_cmp")){
                if(auto r=range(a[0])){
                    bool v=false;
                    if(r->count==1 && i.modifiers.empty()){
                        if((op=="v_mov_b32" || op=="s_mov_b32") && a.size()==2)v=even(a[1]);
                        else if(a.size()==3){
                            if(op=="v_lshlrev_b32" || op=="s_lshl_b32"){
                                auto& s=a[op=="s_lshl_b32"?2:1];v=!s.empty() && std::isdigit(static_cast<unsigned char>(s[0])) &&
                                    !s.contains('.') && (number(s)&31u);
                            }else if(op=="v_and_b32" || op=="s_and_b32" || op=="v_mul_lo_u32" || op=="s_mul_i32")v=even(a[1])||even(a[2]);
                            else if(op=="v_add_nc_u32" || op=="v_sub_nc_u32" || op=="v_subrev_nc_u32" || op=="v_or_b32" || op=="v_xor_b32" ||
                                    op=="s_add_u32" || op=="s_sub_u32" || op=="s_or_b32" || op=="s_xor_b32")v=even(a[1])&&even(a[2]);
                        }else if((op=="v_lshl_add_u32" || op=="v_lshl_or_b32") && a.size()==4){
                            auto& s=a[2];v=even(a[3]) && (even(a[1]) || (!s.empty() && std::isdigit(static_cast<unsigned char>(s[0])) && !s.contains('.') && (number(s)&31u)));
                        }
                    }
                    for(unsigned j=0;j<r->count;++j)facts.set(ix(*r)+j,v);
                }
                if(op=="v_swap_b32")if(auto r=range(a.at(1)))facts.reset(ix(*r));
                // The extra scalar carry-out is not an aligned integer.
                if(op.contains("_co_") && a.size()>1)if(auto r=range(a[1]))for(unsigned j=0;j<r->count;++j)facts.reset(ix(*r)+j);
            }
            if(op=="s_endpgm" || op=="s_branch" || op.starts_with("s_cbranch_") || graph.edges.contains(i.pc+i.size))break;
        }return facts;
    };
    std::map<unsigned,Facts> entry{{start,{}}};std::deque<unsigned> todo{start};
    while(!todo.empty()){
        auto h=todo.front();todo.pop_front();auto out=visit(h,entry.at(h),false);
        for(auto v:graph.edges.at(h)){auto [p,fresh]=entry.try_emplace(v,out);
            if(fresh)todo.push_back(v);
            else if(auto meet=p->second&out;meet!=p->second){p->second=meet;todo.push_back(v);}}
    }
    for(auto& [h,in]:entry)visit(h,in,true);
    return aligned;
}
