// A reducible SCC is a loop if all incoming edges enter its header.
// Replace back edges with continue; route exits through break, then apply
// the same rule to its inner SCCs. The remaining DAG reconverges at actual
// immediate postdominators, so unrelated register values do not all have
// to survive a giant program-counter loop. Multi-entry SCCs and excessive
// code duplication keep the general dispatcher.
template<class Region>
std::optional<Str> structured_flow(std::map<unsigned,Region> nodes,unsigned entry,unsigned& fresh,size_t cap,unsigned depth=0){
    if(depth>12)return {};
    constexpr unsigned end=~0u;
    auto edges=[&]{std::map<unsigned,std::vector<unsigned>> e;for(auto& [h,r]:nodes)e[h]=r.out;return e;};
    for(auto& members:components(edges(),entry)){
        if(members.size()==1 && std::ranges::find(nodes.at(members[0]).out,members[0])==nodes.at(members[0]).out.end())continue;
        std::set<unsigned> loop(members.begin(),members.end()),starts,exits;
        if(loop.contains(entry))starts.insert(entry);
        for(auto& [from,r]:nodes)for(auto to:r.out){
            if(!loop.contains(from) && loop.contains(to))starts.insert(to);
            if(loop.contains(from) && !loop.contains(to))exits.insert(to);
        }
        if(starts.size()!=1 || exits.empty() || exits.size()>12)return {};
        auto h=*starts.begin(),cont=fresh++;auto select=std::format("exit{}_",cont);
        std::map<unsigned,Region> inside;std::map<unsigned,unsigned> escape;
        inside[cont].body="    continue;\n";
        std::vector<unsigned> targets;for(auto out:exits){
            auto edge=fresh++;escape[out]=edge;
            inside[edge].body=(exits.size()==1?Str{}:std::format("    {}={}u;\n",select,targets.size()))+"    break;\n";
            targets.push_back(out);
        }
        for(auto v:members){
            auto r=std::move(nodes.at(v));nodes.erase(v);
            for(auto& to:r.out)if(to==h)to=cont;else if(!loop.contains(to))to=escape.at(to);
            inside[v]=std::move(r);
        }
        auto inner=structured_flow(std::move(inside),h,fresh,cap,depth+1);if(!inner)return {};
        auto& r=nodes[h];r.out=std::move(targets);
        if(r.out.size()>1){r.body="    uint "+select+"=0u;\n";r.branch=r.out.size()==2?select+"==0u":select;}
        r.body+="    [loop] for(;;){\n"+*inner+"    }\n";if(r.body.size()>cap)return {};
    }
    auto sorted=components(edges(),entry);
    std::map<unsigned,unsigned> parent,level;parent[end]=end;level[end]=0;
    auto common=[&](unsigned a,unsigned b){
        while(a!=b){
            if(level.at(a)<level.at(b))b=parent.at(b);
            else if(level.at(b)<level.at(a))a=parent.at(a);
            else{a=parent.at(a);b=parent.at(b);}
        }return a;
    };
    for(auto it=sorted.rbegin();it!=sorted.rend();++it){
        if(it->size()!=1)return {};
        auto h=it->front();auto& dest=nodes.at(h).out;
        auto join=dest.empty()?end:dest[0];
        for(auto v:dest){if(!parent.contains(v))return {};join=common(join,v);}
        parent[h]=join;level[h]=level.at(join)+1;
    }
    Str out;size_t emitted=0;
    auto walk=[&](auto&& self,unsigned h,unsigned stop,unsigned nesting)->bool{
        while(h!=stop && h!=end){
            if(nesting>100 || ++emitted>nodes.size()*4+8 || out.size()>cap)return false;
            auto& n=nodes.at(h);out+=n.body;
            if(n.out.empty())return true;
            if(n.out.size()==1){h=n.out[0];continue;}
            auto merge=parent.at(h);
            if(n.out.size()==2 && (n.out[0]==merge || n.out[1]==merge)){
                auto other=n.out[0]==merge?1u:0u;
                out+="    if("+Str(other?"!(":"(")+n.branch+")){\n";
                if(!self(self,n.out[other],merge,nesting+1))return false;
                out+="    }\n";
            }else{
                for(size_t i=0;i<n.out.size();++i){
                    out+=i?" else":"    ";
                    if(i+1<n.out.size())out+=" if("+n.branch+(n.out.size()==2?"":std::format("=={}u",i))+")";
                    out+="{\n";if(!self(self,n.out[i],merge,nesting+1))return false;out+="    }";
                }
                out+="\n";
            }
            h=merge;
        }
        return out.size()<=cap;
    };
    if(!walk(walk,entry,end,0) || out.size()>cap)return {};
    return out;
}
