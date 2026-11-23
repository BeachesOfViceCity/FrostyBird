// Distinguish bounded-stride output/update loops from a device work queue.
// Remove only backedges controlled by a local induction register and an
// invariant limit. If any remaining cycle can still poll a mutable buffer,
// keep the reference scheduler. No shader key or entry name enters the proof.
bool independent_output_loops(const Graph& g,unsigned start,const std::set<unsigned>& reads,
                              const std::set<unsigned>& mutable_inputs){
    using It=decltype(g.ins)::const_iterator;
    auto regid=[](View s)->std::optional<Range>{
        if(s=="exec")return Range{'s',108,2};
        if(s=="vcc")return Range{'s',106,2};
        if(s=="exec_lo" || s=="exec_hi")return Range{'s',108u+unsigned(s.ends_with("hi")),1};
        if(s=="vcc_lo" || s=="vcc_hi")return Range{'s',106u+unsigned(s.ends_with("hi")),1};
        if(s.size()>1 && (s[0]=='s'||s[0]=='v') && (std::isdigit(static_cast<unsigned char>(s[1]))||s[1]=='['))return registers(s);
        return {};
    };
    auto overlap=[](Range a,Range b){return a.bank==b.bank && a.first<b.first+b.count && b.first<a.first+a.count;};
    auto writes=[&](const Instruction& i,Range r){
        if(i.op.starts_with("s_cbranch") || i.op.starts_with("s_cmp") || i.op.contains("_store") || i.op.starts_with("ds_write"))return false;
        if(i.args.empty())return false;
        if(auto q=regid(i.args[0]);q && overlap(*q,r))return true;
        if(i.op.contains("_co_") && i.args.size()>1)if(auto q=regid(i.args[1]);q && overlap(*q,r))return true;
        return false;
    };
    for(auto& [pc,i]:g.ins)if(i.op.contains("movrel") || i.op.contains("swap") || i.op.contains("pc_"))return false;
    auto trimmed=g.edges;std::set<unsigned> dirty_heads;
    for(auto pc:mutable_inputs)dirty_heads.insert(std::prev(g.edges.upper_bound(pc))->first);
    bool certified=false;
    for(auto& [last,edges]:g.edges){
        if(edges.size()!=2 || edges[0]>last)continue;
        auto head=edges[0];auto first=g.ins.find(head),at=g.ins.find(last);std::vector<It> block;
        for(;at!=g.ins.end();++at){const auto& i=at->second;
            if(i.op!="s_nop" && i.op!="v_nop" && i.op!="s_waitcnt")block.push_back(at);
            if(i.op.starts_with("s_cbranch_") || g.edges.contains(i.pc+i.size))break;}
        if(block.size()<3 || first==g.ins.end())continue;
        auto tail=block.back();auto end=std::next(tail);
        if(std::ranges::any_of(g.edges,[&](auto& e){return e.first>tail->first && std::ranges::any_of(e.second,[&](auto v){return v>head && v<=tail->first;});}))continue;
        auto invariant=[&](auto&& self,const Str& name,It before,unsigned depth)->bool{
            if(depth>6)return false;
            auto reg=regid(name);
            if(!reg)return name=="null" || (!name.empty() && std::isdigit(static_cast<unsigned char>(name[0])));
            if(reg->bank!='s' || reg->first>=106)return false;
            if(std::none_of(first,end,[&](auto& e){return writes(e.second,*reg);}))return true;
            while(before!=g.ins.find(last)){--before;auto& i=before->second;
                if(!writes(i,*reg))continue;
                if(i.args[0]!=name || reg->count!=1)return false;
                if((i.op=="s_mov_b32" || i.op=="s_movk_i32") && i.args.size()==2)return self(self,i.args[1],before,depth+1);
                bool load=i.op=="s_buffer_load_dword" && reads.contains(i.pc) && !mutable_inputs.contains(i.pc);
                return (load || i.op=="s_add_u32" || i.op=="s_add_i32") && i.args.size()==3 &&
                    self(self,i.args[1],before,depth+1) && self(self,i.args[2],before,depth+1);
            }
            return false;
        };
        auto update=[&](const Str& counter,It cmp,bool vec)->It{
            auto r=regid(counter);if(!r || r->count!=1 || r->bank!=(vec?'v':'s'))return end;
            It inc=end;
            for(auto it=first;it!=end;++it)if(writes(it->second,*r)){
                const auto& i=it->second;auto& a=i.args;
                if(inc!=end || i.pc<last || i.pc>=cmp->first || a.size()!=3 || a[0]!=counter ||
                   !(vec?i.op=="v_add_nc_u32":(i.op=="s_add_i32"||i.op=="s_add_u32")))return end;
                auto stride=a[1]==counter?a[2]:a[2]==counter?a[1]:Str{};
                if(auto s=regid(stride)){
                    if(s->bank!='s' || s->count!=1)return end;
                    auto q=it;bool found=false;
                    while(q!=g.ins.find(last)){--q;auto& move=q->second;if(!writes(move,*s))continue;
                        if((move.op=="s_mov_b32" || move.op=="s_movk_i32") && move.args.size()==2 &&
                           move.args[0]==stride){stride=move.args[1];found=true;}break;}
                    if(!found)return end;
                }
                if(stride.empty() || !std::isdigit(static_cast<unsigned char>(stride[0])) || stride.contains('.') ||
                   !number(stride) || number(stride)>65535)return end;
                inc=it;
            }
            return inc;
        };
        auto br=tail->second.op;auto cmp=block[block.size()-2];bool ok=false;
        if(((br=="s_cbranch_scc0" && cmp->second.op=="s_cmp_ge_u32") ||
                (br=="s_cbranch_scc1" && cmp->second.op=="s_cmp_lt_u32")) && cmp->second.args.size()==2)
            ok=update(cmp->second.args[0],cmp,false)!=end && invariant(invariant,cmp->second.args[1],cmp,0);
        else if(br=="s_cbranch_scc1" && block.size()>=5){
            const auto& remove=cmp->second;auto& save=block[block.size()-3]->second;cmp=block[block.size()-4];
            auto& ca=cmp->second.args;
            if(remove.op!="s_andn2_b64" || remove.args.size()!=3 || remove.args[0]!=remove.args[1] || remove.args[2]!="exec" ||
               save.op!="s_and_saveexec_b64" || save.args.size()!=2 || save.args[1]!="vcc" ||
               cmp->second.op!="v_cmp_le_u32" || ca.size()!=3 || ca[0]!="vcc" ||
               first->second.op!="s_and_b64" || first->second.args!=std::vector<Str>{"exec",save.args[0],remove.args[0]})continue;
            auto pending=regid(remove.args[0]);auto inc=update(ca[2],cmp,true);
            if(!pending || pending->count!=2 || inc==end || !invariant(invariant,ca[1],cmp,0) ||
               std::any_of(first,block[block.size()-2],[&](auto& e){return writes(e.second,*pending);}))continue;
            // On a taken trip every remaining lane must perform the original
            // increment and compare. Follow SAVEEXEC/restore through both
            // arms: 2=entry mask, 1=subset, 0=unknown. A conditional restore
            // or a clobbered SGPR pair cannot certify a loop.
            using Masks=std::array<unsigned char,110>;std::map<unsigned,Masks> in{{head,{}}};std::deque<unsigned> todo{head};
            bool same=true;
            while(!todo.empty()){
                auto h=todo.front();todo.pop_front();auto m=in.at(h);
                for(auto it=g.ins.find(h);it!=end;++it){
                    auto& i=it->second;auto& a=i.args;
                    if(it==inc || it==cmp)if(m[108]!=2)same=false;
                    if(h==head && it==first)m[108]=2;
                    else if(!a.empty() && !i.op.starts_with("s_cmp") && !i.op.starts_with("s_cbranch") &&
                            !i.op.starts_with("ds_write") && !i.op.contains("_store")){
                        auto old=m;auto val=[&](const Str& x)->unsigned char{
                            auto r=regid(x);return r && r->bank=='s' && r->count==2?old[r->first]:0;};
                        auto put=[&](const Str& name,unsigned char v){
                            auto r=regid(name);if(!r || r->bank!='s')return;
                            for(unsigned j=r->first?r->first-1:0;j<std::min<unsigned>(110,r->first+r->count);++j)m[j]=0;
                            if(r->count==2)m[r->first]=v;
                        };
                        auto subset=[](unsigned char x)->unsigned char{return x?1:0;};
                        if(i.op=="s_mov_b64" && a.size()==2)put(a[0],val(a[1]));
                        else if(i.op=="s_and_saveexec_b64" && a.size()==2){put(a[0],old[108]);m[108]=subset(old[108]);}
                        else if(i.op=="s_and_b64" && a.size()==3)put(a[0],val(a[1])==2 && val(a[2])==2?2:subset(val(a[1])|val(a[2])));
                        else if(i.op=="s_or_b64" && a.size()==3)put(a[0],val(a[1]) && val(a[2])?std::max(val(a[1]),val(a[2])):0);
                        else if(i.op=="s_andn2_b64" && a.size()==3)put(a[0],subset(val(a[1])));
                        else if(i.op.starts_with("v_cmpx"))m[108]=subset(old[108]);
                        else {
                            put(a[0],i.op.starts_with("v_cmp_")?subset(old[108]):0);
                            if(i.op.contains("_co_") && a.size()>1)put(a[1],0);
                            if(i.op.contains("saveexec")||i.op.contains("wrexec"))m[108]=0;
                        }
                    }
                    if(it==tail || i.op=="s_endpgm" || i.op=="s_branch" || i.op.starts_with("s_cbranch") || g.edges.contains(i.pc+i.size))break;
                }
                if(h==last)continue;
                for(auto v:g.edges.at(h))if(v>=head && v<=last && v!=head){
                    auto [q,fresh]=in.try_emplace(v,m);if(!fresh){auto meet=q->second;
                        for(unsigned k=0;k<meet.size();++k)meet[k]=std::min(meet[k],m[k]);
                        if(meet==q->second)continue;
                        q->second=meet;}
                    todo.push_back(v);
                }
            }
            ok=same && in.contains(last);
        }
        if(ok){std::erase(trimmed.at(last),head);certified=true;}
    }
    if(!certified)return false;
    for(auto& c:components(trimmed,start)){
        if(c.size()==1 && std::ranges::find(trimmed.at(c[0]),c[0])==trimmed.at(c[0]).end())continue;
        if(std::ranges::any_of(c,[&](auto h){return dirty_heads.contains(h);}))return false;
    }
    return true;
}
