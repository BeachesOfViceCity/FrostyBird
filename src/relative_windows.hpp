#pragma once
#include "isa.hpp"
#include <algorithm>
#include <charconv>
#include <optional>

namespace frosty::isa {
struct RelativeWindow {unsigned base,width,condition;bool store;};
// Recover the array bound from the actual unsigned check. Keep the original
// M0 election, mask narrowing, waterfall back edge and EXEC restoration.
inline std::map<unsigned,RelativeWindow> pixel_relative_windows(const Graph& g,Bytes code){
    using S=std::string;
    std::map<unsigned,RelativeWindow> out;std::vector<const Instruction*> ins;
    for(auto& [_,i]:g.ins)ins.push_back(&i);
    auto integer=[](std::string_view s)->std::optional<unsigned>{
        unsigned v{};int radix=s.starts_with("0x")?16:10;if(radix==16)s.remove_prefix(2);
        auto [end,ec]=std::from_chars(s.data(),s.data()+s.size(),v,radix);
        if(ec!=std::errc{}||end!=s.data()+s.size())return {};
        return v;
    };
    auto target=[&](const Instruction& i){return unsigned(std::int64_t(i.pc)+i.size+
        4ll*std::bit_cast<std::int16_t>(std::uint16_t(code.at<u32>(i.pc))));};
    auto trivial=[](const Instruction& i){return i.op=="s_nop" || i.op=="v_nop" || i.op=="s_waitcnt";};
    for(size_t k=6;k+3<ins.size();++k){
        auto& mv=*ins[k];bool store=mv.op=="v_movreld_b32";if(!store && mv.op!="v_movrels_b32")continue;
        auto base=integer(std::string_view(mv.args[store?0:1]).substr(1));if(!base || *base>=256)continue;
        size_t bound=0,last=k;unsigned width=0;auto& set=*ins[k-1];auto& cmp=*ins[k-2];
        if(!store && cmp.op=="s_mov_b32" && cmp.args[0]=="m0" && set.op=="v_cmp_lt_u32" &&
           set.args[0]=="vcc" && set.args[1]==cmp.args[1]){
            bound=k-1;width=integer(set.args[2]).value_or(0);
        }else{
            auto& elect=*ins[k-4];auto& eq=*ins[k-3];auto& narrow=*ins[k-2];
            if(elect.op!="v_readfirstlane_b32" || set.op!="s_mov_b32" || set.args!=std::vector<S>{"m0",elect.args[0]} ||
               eq.op!="v_cmp_eq_u32" || eq.args[1]!=elect.args[0] || eq.args[2]!=elect.args[1] ||
               narrow.op!="s_and_saveexec_b64" || narrow.args[1]!=eq.args[0] ||
               ins[k+1]->op!="s_xor_b64" || ins[k+1]->args!=std::vector<S>{"exec","exec",narrow.args[0]} ||
               ins[k+2]->op!="s_cbranch_execnz" || target(*ins[k+2])!=elect.pc)continue;
            last=k+3;while(last<ins.size() && trivial(*ins[last]))++last;
            if(last==ins.size() || ins[last]->op!="s_mov_b64" || ins[last]->args[0]!="exec")continue;
            if(!store){bound=k-5;if(eq.args[0]=="vcc" || narrow.args[0]=="vcc")continue;}
            else{
                size_t j=k-4;
                while(j && (trivial(*ins[j-1]) || (ins[j-1]->op=="s_mov_b64" && ins[j-1]->args[1]=="exec")))--j;
                if(j<2 || ins[j-1]->op!="s_cbranch_execz" || target(*ins[j-1])<=ins[last]->pc ||
                   ins[j-2]->op!="s_and_saveexec_b64" || ins[j-2]->args[1]!="vcc")continue;
                j-=2;
                auto preserves_index=[&](const Instruction& i){
                    if(trivial(i))return true;
                    if(i.args.empty() || i.args[0]==elect.args[1] || i.args[0].size()<2 ||
                       i.args[0][0]!='v' || !integer(std::string_view(i.args[0]).substr(1)))return false;
                    // One VGPR result, no EXEC/VCC side effects, no relative
                    // addressing. A swap or a multi-result op could clobber
                    // the checked index even when its first operand differs.
                    return i.op=="v_mov_b32" || i.op=="v_exp_f32" || i.op=="v_mul_legacy_f32" ||
                           i.op=="v_mul_f32" || i.op=="v_add_f32" || i.op=="v_fma_f32";
                };
                while(j && preserves_index(*ins[j-1]))--j;
                if(!j)continue;
                bound=j-1;
            }
            auto& b=*ins[bound];
            if(b.op!="v_cmp_gt_u32" || b.args[0]!="vcc" || b.args[2]!=elect.args[1])continue;
            width=integer(b.args[1]).value_or(0);
        }
        if(!width || width>256-*base)continue;
        bool entered=false;for(auto* i:ins)if(i->op=="s_branch" || i->op.starts_with("s_cbranch_")){
            auto to=target(*i);
            if((i->pc<ins[bound]->pc || i->pc>ins[last]->pc) && to>ins[bound]->pc && to<=ins[last]->pc){entered=true;break;}
        }
        if(!entered)out[mv.pc]={*base,width,ins[bound]->pc+1,store};
    }
    return out;
}
}
