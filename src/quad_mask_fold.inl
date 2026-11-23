// Fold a scalar branch only when the original mask algebra forces its
// condition. WQM(M) contains M for *any* coverage, including partial quads.
// An SGPR clobber invalidates every overlapping saved pair. No fact crosses
// a basic-block entry or an unrecognized instruction.
std::optional<Buffer> fold_quad_mask_edges(const Program& p){
    if(p.stage!=pixel || p.wave!=64)return {};
    auto g=decode_graph(p.code,nullptr,p.wave);
    if(std::ranges::any_of(g.ins,[](auto& e){return e.second.op.contains("pc_");}))return {};
    struct Fact{unsigned id;std::set<unsigned> contains;std::optional<bool> empty;};
    unsigned serial=0;std::optional<Buffer> copy;
    auto fact=[&](std::optional<bool> empty={}){++serial;return Fact{serial,{serial},empty};};
    for(auto& [h,edges]:g.edges){
        std::map<unsigned,Fact> pairs;std::optional<bool> scc;
        auto base=[](View n)->std::optional<unsigned>{
            if(n=="exec")return 108;
            if(n=="vcc")return 106;
            if(n.starts_with("s[") && n.ends_with(']')){auto r=registers(n);if(r.count==2)return r.first;}
            return {};
        };
        auto get=[&](View n){
            if(n=="0" || n=="null")return fact(true);
            if(n=="-1")return fact(false);
            auto k=base(n);if(!k)return fact();
            auto it=pairs.find(*k);if(it!=pairs.end())return it->second;
            return pairs.emplace(*k,fact()).first->second;
        };
        auto forget=[&](unsigned k,unsigned words){
            std::erase_if(pairs,[&](auto& e){return k<e.first+2 && e.first<k+words;});
        };
        auto put=[&](View name,Fact f){if(auto k=base(name)){forget(*k,2);pairs.emplace(*k,std::move(f));}
            else {pairs.clear();scc.reset();}};
        for(auto it=g.ins.find(h);it!=g.ins.end();++it){
            const auto& i=it->second;auto& a=i.args;View op=i.op;
            if(op=="s_branch" || op=="s_endpgm")break;
            if(op.starts_with("s_cbranch_")){
                std::optional<bool> take;
                if(op=="s_cbranch_scc0" || op=="s_cbranch_scc1"){if(scc)take=(*scc==op.ends_with('1'));}
                else if(op=="s_cbranch_execz" || op=="s_cbranch_execnz"){
                    if(auto e=get("exec").empty)take=(*e==(op=="s_cbranch_execz"));}
                if(take && i.size==4){
                    if(!copy)copy.emplace(p.code.data.begin(),p.code.data.end());
                    u32 word=*take?0xbf820000u|(p.code.at<u32>(i.pc)&65535u):0xbf800000u;
                    std::memcpy(copy->data()+i.pc,&word,4);
                }
                break;
            }
            if(!i.modifiers.empty()){pairs.clear();scc.reset();}
            else if(op=="s_mov_b64" && a.size()==2)put(a[0],get(a[1]));
            else if(op=="s_wqm_b64" && a.size()==2){
                auto src=get(a[1]),v=fact(src.empty);v.contains.insert(src.contains.begin(),src.contains.end());
                scc=v.empty?std::optional<bool>(!*v.empty):std::nullopt;put(a[0],std::move(v));
            }else if(a.size()==3 && (op=="s_andn2_b64" || op=="s_xor_b64" || op=="s_or_b64" || op=="s_and_b64")){
                auto x=get(a[1]),y=get(a[2]),v=fact();
                if(op=="s_andn2_b64" && (y.contains.contains(x.id) || x.empty==true))v.empty=true;
                else if(op=="s_xor_b64" && x.id==y.id)v.empty=true;
                else if(op=="s_and_b64" && (x.empty==true || y.empty==true))v.empty=true;
                else if(op=="s_or_b64"){
                    v.contains.insert(x.contains.begin(),x.contains.end());v.contains.insert(y.contains.begin(),y.contains.end());
                    if(x.empty==false || y.empty==false)v.empty=false;else if(x.empty==true && y.empty==true)v.empty=true;
                }
                scc=v.empty?std::optional<bool>(!*v.empty):std::nullopt;put(a[0],std::move(v));
            }else if(op=="s_mov_b32" && a.size()==2 && a[0].size()>1 && a[0][0]=='s' && std::isdigit(static_cast<unsigned char>(a[0][1])))
                forget(registers(a[0]).first,1);
            else if(op!="s_inst_prefetch" && op!="s_nop" && op!="s_waitcnt"){pairs.clear();scc.reset();}
            if(g.edges.contains(i.pc+i.size))break;
        }
    }
    return copy;
}
