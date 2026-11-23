#pragma once
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace frosty::isa {
// CMP predicates are bitsets over {less, equal, greater, unordered}. Preserve
// the guest ordering of subnormals even on backends that flush FP32 inputs.
inline std::string fast_fcmp32(std::string a,std::string b,unsigned mask,bool& needs_pair){
    auto literal=[](std::string_view v)->std::optional<unsigned>{
        if(!v.ends_with('u'))return {};
        v.remove_suffix(1);int base=10;if(v.starts_with("0x")){v.remove_prefix(2);base=16;}
        unsigned n{};auto [p,ec]=std::from_chars(v.data(),v.data()+v.size(),n,base);
        if(ec!=std::errc{} || p!=v.end())return {};
        return n;
    };
    auto ca=literal(a),cb=literal(b);
    auto zero=[](auto v){return v && (*v&0x7fffffffu)==0;};
    auto non_tiny=[](auto v){return v && (*v&0x7fffffffu)>=0x00800000u;};
    if(zero(ca)){std::swap(a,b);std::swap(ca,cb);mask=(mask&10u)|((mask&1u)<<2u)|((mask&4u)>>2u);}
    bool invert=mask>7;if(invert)mask^=15;
    auto mag=[](const std::string& x){return "("+x+" & 0x7fffffffu)";};
    std::string e;
    if(mask==0)e="false";
    else if(mask==7)e="(max("+mag(a)+","+mag(b)+") <= 0x7f800000u)";
    else if(zero(cb)){
        // Negative nonzero numbers, including -inf, occupy one contiguous
        // unsigned interval. NaNs stay out, and both signed zeros compare equal.
        switch(mask){
        case 1:e="(("+a+"-0x80000001u) < 0x7f800000u)";break;
        case 2:e="("+mag(a)+" == 0u)";break;
        case 3:e="(("+a+"-0x80000000u) <= 0x7f800000u || "+a+" == 0u)";break;
        case 4:e="(("+a+"-1u) < 0x7f800000u)";break;
        case 5:e="(("+mag(a)+"-1u) < 0x7f800000u)";break;
        case 6:e="("+a+" <= 0x7f800000u || "+a+" == 0x80000000u)";break;
        }
    }else{
        bool pair=!non_tiny(ca)&&!non_tiny(cb);
        if(pair){needs_pair=true;e="xs_fcmp32("+a+","+b+","+std::to_string(mask)+"u)";}
        else{
            auto l="asfloat("+a+")",r="asfloat("+b+")";
            static constexpr const char* op[]{"","<","==","<=",">","",">="};
            e=mask==5?"("+l+" < "+r+" || "+l+" > "+r+")":
                "("+l+" "+op[mask]+" "+r+")";
        }
    }
    return invert?"!("+e+")":e;
}
inline constexpr char fcmp32_helper[]=R"(
bool xs_fcmp32(uint a,uint b,uint test){
    // Only two tiny inputs can have their relative order changed by FTZ.
    // Rebase both magnitudes into the smallest normal bin in that case.
    // Do not rebase a pair of zeros: -0 and +0 must still compare equal.
    uint m=max(a<<1u,b<<1u);
    uint bias=m-1u<0x00ffffffu?0x00800000u:0u;
    float x=asfloat(a|bias),y=asfloat(b|bias);
    if(test==1u)return x<y; if(test==2u)return x==y;
    if(test==3u)return x<=y; if(test==4u)return x>y;
    if(test==5u)return x<y || x>y;
    return x>=y;
}
)";
}
