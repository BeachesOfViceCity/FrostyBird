#pragma once
#include "isa.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <optional>

namespace frosty::isa {
// Prove a 32-bit XOR-linear transform from the guest instructions in one
// straight-line EXEC region. Column i is the output from setting only bit i.
// All 32 columns must match; similar-looking direction tables do not qualify.
struct BitTransforms {
    struct Input{unsigned vg;std::string name;};
    std::map<u32,std::vector<Input>> saved;
    std::map<u32,std::string> result;
    static constexpr std::string_view helper=R"(
uint xs_subset_bits(uint x){
    x^=(x&0xaaaaaaaau)>>1u; x^=(x&0xccccccccu)>>2u;
    x^=(x&0xf0f0f0f0u)>>4u; x^=(x&0xff00ff00u)>>8u;
    return x^(x>>16u);
}
)";
    void analyze(std::span<const Instruction* const> block){
        using V=std::string_view;using Cols=std::array<u32,32>;
        if(std::ranges::count_if(block,[](auto i){return i->op=="v_bfe_u32" && i->args.size()==4 && i->args[3]=="1";})<12)return;
        struct Value{u64 seed=~u64(0);u32 bias{};Cols col{};};
        using Maybe=std::optional<Value>;
        static constexpr Cols subset=[](){Cols c{};for(u32 i=0;i<32;++i)for(u32 j=0;j<32;++j)if(!(j&~i))c[i]|=1u<<j;return c;}();
        auto num=[](V s)->std::optional<u32>{
            bool neg=s.starts_with('-');if(neg)s.remove_prefix(1);
            int radix=s.starts_with("0x")?16:10;if(radix==16)s.remove_prefix(2);
            u32 x{};auto [p,e]=std::from_chars(s.data(),s.data()+s.size(),x,radix);
            if(e!=std::errc{} || p!=s.data()+s.size())return {};
            return neg?0u-x:x;
        };
        auto vg=[&](V s)->std::optional<u32>{
            if(s.size()<2 || s[0]!='v' || s.find_first_not_of("0123456789",1)!=s.npos)return {};
            auto n=num(s.substr(1));return n && *n<256?n:std::nullopt;};
        std::map<u32,Value> regs;
        for(auto ip:block){
            auto& i=*ip;V op=i.op;auto& a=i.args;
            if(op.contains("_co_") || (!a.empty() && (a[0]=="exec" || a[0]=="exec_lo" || a[0]=="exec_hi"))){
                regs.clear();continue;
            }
            auto get=[&](const std::string& s)->Maybe{
                if(auto v=vg(s)){
                    auto [it,fresh]=regs.try_emplace(*v);
                    if(fresh){it->second.seed=(u64(i.pc)<<8)|*v;for(unsigned j=0;j<32;++j)it->second.col[j]=1u<<j;}
                    return it->second;
                }
                if(auto x=num(s))return Value{~u64(0),*x,{}};
                return {};
            };
            auto unary=[](Maybe v,auto fn)->Maybe{if(v){v->bias=fn(v->bias);for(auto& c:v->col)c=fn(c);}return v;};
            auto xorm=[](Maybe x,Maybe y)->Maybe{
                if(!x || !y || (x->seed!=~u64(0) && y->seed!=~u64(0) && x->seed!=y->seed))return {};
                if(x->seed==~u64(0))x->seed=y->seed;
                x->bias^=y->bias;for(unsigned j=0;j<32;++j)x->col[j]^=y->col[j];return x;
            };
            auto mask=[&](Maybe v,u32 m){return unary(v,[&](u32 x){return x&m;});};
            auto dst=a.empty()?std::nullopt:vg(a[0]);
            if(!dst){
                if(op=="s_nop" || op=="s_waitcnt" || op=="v_nop" || op.starts_with("v_cmp_"))continue;
                if(!a.empty() && (op=="s_mov_b32" || op=="s_movk_i32" || op.starts_with("s_buffer_load_dword") || op.starts_with("s_load_dword")) &&
                   a[0].size()>1 && a[0][0]=='s' && (a[0][1]=='[' || (a[0][1]>='0' && a[0][1]<='9')))continue;
                regs.clear();continue;
            }
            Maybe v;std::optional<u32> add;
            if(a.size()==4 && op=="v_xad_u32" && i.modifiers.empty()){
                add=num(a[3]);if(add)v=xorm(get(a[1]),get(a[2]));
            }else if(a.size()==3 && (op=="v_xor_b32" || op=="v_xor_b32_sdwa")){
                auto r=get(a[2]);
                if(i.modifiers=="dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:DWORD src1_sel:WORD_0")r=mask(r,65535);
                else if(i.modifiers=="dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:DWORD src1_sel:BYTE_0")r=mask(r,255);
                else if(!i.modifiers.empty() || op!="v_xor_b32")r.reset();
                v=xorm(get(a[1]),r);
            }else if(i.modifiers.empty() && op=="v_and_b32" && a.size()==3){
                if(auto x=num(a[1]))v=mask(get(a[2]),*x);else if(auto y=num(a[2]))v=mask(get(a[1]),*y);
            }else if(i.modifiers.empty() && op=="v_bfe_u32" && a.size()==4){
                auto off=num(a[2]),w=num(a[3]);if(off && w)v=unary(get(a[1]),[&](u32 x){return (x>>(*off&31))&((1u<<(*w&31))-1);});
            }else if(i.modifiers.empty() && (op=="v_ashrrev_i32" || op=="v_lshrrev_b32" || op=="v_lshlrev_b32") && a.size()==3){
                if(auto shift=num(a[1]))v=unary(get(a[2]),[&](u32 x){auto n=*shift&31;return op=="v_lshlrev_b32"?x<<n:
                    op=="v_lshrrev_b32"?x>>n:u32(std::bit_cast<std::int32_t>(x)>>n);});
            }else if(i.modifiers.empty() && op=="v_mov_b32" && a.size()==2)v=get(a[1]);
            else if(i.modifiers.empty() && op=="v_sub_nc_u32" && a.size()==3 && a[1]=="0"){
                auto x=get(a[2]);if(x && x->bias<=1 && std::ranges::all_of(x->col,[](u32 c){return c<=1;}))v=unary(x,[](u32 b){return 0u-b;});
            }else if(!(op.starts_with("v_add_") || op.starts_with("v_sub") || op.starts_with("v_mul_") ||
                        op.starts_with("v_fma") || op.starts_with("v_cvt_") || op=="v_lshl_or_b32" || op=="v_lshl_add_u32" ||
                        op=="v_bfrev_b32" || op=="v_or_b32" || op=="v_and_or_b32" || op=="v_cndmask_b32" ||
                        op=="v_min_f32" || op=="v_max_f32" || op=="v_rcp_f32" || op=="v_rsq_f32")){
                regs.clear();continue;
            }
            if(v && v->seed!=~u64(0) && v->col==subset){
                auto pc=u32(v->seed>>8),reg=u32(v->seed&255);auto name="xs_bit_input_"+std::to_string(pc)+"_"+std::to_string(reg);
                auto& inputs=saved[pc];if(std::ranges::none_of(inputs,[&](auto& in){return in.vg==reg;}))inputs.push_back({reg,name});
                auto expr="xs_subset_bits("+name+")";
                if(v->bias)expr="("+expr+" ^ "+std::to_string(v->bias)+"u)";
                if(add && *add)expr+=" + "+std::to_string(*add)+"u";
                result[i.pc]=std::move(expr);
            }
            if(v && !add)regs[*dst]=*v;else regs.erase(*dst);
        }
    }
};
}
