#pragma once
#include "isa.hpp"
#include <charconv>
#include <optional>

namespace frosty::isa {
// A scalar word can still contain an SRD along one incoming edge and be an
// ordinary index on another. Do not merge those paths through a branch that
// provably never takes the SRD to the integer consumer. Facts apply only to
// active lanes and are discarded on unknown instructions, including EXEC
// changes. In particular, EXEC itself is *not* assumed to be nonempty.
struct MaskEdges {
    std::map<std::pair<unsigned,unsigned>,unsigned> redirects;
    std::map<unsigned,std::pair<unsigned,unsigned>> clones; // id -> (original block, known successor)

    MaskEdges(const Graph& g,unsigned wave,unsigned first_id){
        using Word=std::optional<u32>;
        struct Facts {std::map<std::string,u32> lanes;Word mask;};
        auto gpr=[](std::string_view s,char bank){
            return s.size()>1 && s.front()==bank && s.find_first_not_of("0123456789",1)==s.npos;
        };
        auto vcc=[&](std::string_view s){return s=="vcc" || (wave==32 && s=="vcc_lo");};
        auto exec=[&](std::string_view s){return s=="exec" || (wave==32 && s=="exec_lo");};
        auto transfer=[&](Facts& f,const Instruction& i){
            const auto& a=i.args;const auto& op=i.op;
            auto word=[&](const std::string& s)->Word{
                if(auto it=f.lanes.find(s);it!=f.lanes.end())return it->second;
                std::string_view v=s;bool neg=v.starts_with('-');if(neg)v.remove_prefix(1);
                int base=v.starts_with("0x")?16:10;if(base==16)v.remove_prefix(2);
                if(v.empty())return {};
                u32 n{};auto [p,ec]=std::from_chars(v.data(),v.data()+v.size(),n,base);
                if(ec!=std::errc{} || p!=v.data()+v.size())return {};
                return neg?0u-n:n;
            };
            // A two-bit truth table for a VCC bit when EXEC is 0 or 1.
            auto mask=[&](const std::string& s)->Word{
                if(exec(s))return 2;
                if(vcc(s))return f.mask;
                if(s=="0")return 0;
                if(s=="-1")return 3;
                return {};
            };
            if(!i.modifiers.empty()){f={};return;}
            if(op=="v_mov_b32" && a.size()==2 && gpr(a[0],'v')){
                if(auto n=word(a[1]))f.lanes[a[0]]=*n;else f.lanes.erase(a[0]);return;
            }
            if((op=="v_cmp_eq_u32" || op=="v_cmp_ne_u32") && a.size()==3 && vcc(a[0])){
                auto x=word(a[1]),y=word(a[2]);f.mask.reset();
                if(x && y)f.mask=((*x==*y)==(op=="v_cmp_eq_u32"))?2:0;
                return;
            }
            if(a.size()==3 && vcc(a[0]) && (op.ends_with("_b64") || (wave==32 && op.ends_with("_b32")))){
                auto x=mask(a[1]),y=mask(a[2]);
                if(op.starts_with("s_andn2_") || op.starts_with("s_and_") || op.starts_with("s_or_") || op.starts_with("s_xor_")){
                    f.mask.reset();
                    if(x && y)f.mask=op.starts_with("s_andn2_")?(*x & ~*y):op.starts_with("s_and_")?(*x & *y):
                        op.starts_with("s_or_")?(*x | *y):(*x ^ *y);
                    return;
                }
            }
            if(op=="s_branch" || op.starts_with("s_cbranch_") || op=="s_cmp_eq_u32" || op=="s_cmp_lg_u32" ||
               op=="s_waitcnt" || op=="s_nop" || (op=="s_mov_b32" && a.size()==2 && gpr(a[0],'s')))return;
            f={};
        };
        std::map<unsigned,std::vector<unsigned>> incoming;
        for(const auto& [h,edges]:g.edges)for(auto to:edges)incoming[to].push_back(h);
        auto visit=[&](unsigned h,auto&& fn){
            for(auto it=g.ins.find(h);it!=g.ins.end();++it){
                const auto& i=it->second;fn(i);
                if(i.op=="s_endpgm" || i.op=="s_branch" || i.op.starts_with("s_cbranch_") || g.edges.contains(i.pc+i.size))break;
            }
        };
        for(const auto& [h,preds]:incoming){
            const auto& out=g.edges.at(h);if(preds.size()<2)continue;
            if(out.empty()){
                // Compilers share the final store/sample epilogue between
                // arms which load different complete SRDs into the same
                // SGPR group. Specialize the terminal block per incoming
                // edge; no dynamic resource index or interchangeable UAV
                // assumption is required. A terminal has no branch to fold.
                bool memory=false;visit(h,[&](const Instruction& i){memory|=i.op.starts_with("buffer_") ||
                    i.op.starts_with("image_") || i.op.starts_with("s_buffer_load");});
                if(memory)for(size_t k=1;k<preds.size();++k){
                    redirects.emplace(std::pair{preds[k],h},first_id);clones.emplace(first_id++,std::pair{h,h});
                }
                continue;
            }
            if(out.size()!=2)continue;
            const Instruction* br{};visit(h,[&](const Instruction& i){br=&i;});
            if(!br || (br->op!="s_cbranch_vccnz" && br->op!="s_cbranch_vccz"))continue;
            std::map<unsigned,unsigned> ids;
            for(auto pred:preds){
                Facts f;visit(pred,[&](const Instruction& i){transfer(f,i);});
                visit(h,[&](const Instruction& i){transfer(f,i);});
                if(!f.mask || (*f.mask!=0 && *f.mask!=3))continue;
                bool taken=(*f.mask==0)==(br->op=="s_cbranch_vccz");
                unsigned to=out[taken?0:1];
                if(!ids.contains(to)){ids[to]=first_id;clones.emplace(first_id++,std::pair{h,to});}
                redirects.emplace(std::pair{pred,h},ids[to]);
            }
        }
    }
    unsigned dest(unsigned from,unsigned to) const{
        auto it=redirects.find({from,to});return it==redirects.end()?to:it->second;
    }
};
}
