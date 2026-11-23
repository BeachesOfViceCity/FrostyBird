#pragma once
#include <string_view>

namespace frosty::isa {
// IEEE binary64, round-to-nearest/even, using SM6 integer operations. The
// 128-bit accumulator keeps the product intact through a fused multiply/add.
// Used only in shaders with wide floating-point ALU, including on devices
// whose Vulkan driver cannot execute native double arithmetic.
inline constexpr std::string_view soft64=R"(
#define SF_ONE uint64_t(1)
#define SF_HIDDEN (SF_ONE<<52u)
#define SF_SIGN (SF_ONE<<63u)
#define SF_INF (uint64_t(2047)<<52u)
#define SF_NAN (SF_INF|(SF_HIDDEN>>1u))
struct SfParts {uint64_t m;int e;bool neg;};
struct SfLong {uint64_t l,h;};
int sf_top(uint64_t x){uint h=uint(x>>32u);return h!=0u?32+int(firstbithigh(h)):int(firstbithigh(uint(x)));}
uint64_t sf_mag(uint64_t a){return a&~SF_SIGN;}
uint64_t sf_sign(bool n){return n?SF_SIGN:uint64_t(0);}
SfParts sf_parts(uint64_t a){
    SfParts p;p.neg=(a&SF_SIGN)!=0;p.m=a&(SF_HIDDEN-1);uint expv=uint((a>>52u)&2047u);
    p.e=int(expv)-1023;
    if(expv!=0)p.m|=SF_HIDDEN;
    else if(p.m!=0){int k=52-sf_top(p.m);p.m<<=k;p.e=-1022-k;}
    return p;
}
uint64_t sf_jam(uint64_t a,uint n){
    if(n==0)return a;if(n>=64)return uint64_t(a!=0);
    return (a>>n)|uint64_t((a<<(64u-n))!=0);
}
// z has three extra rounding bits; normalize after addition or cancellation.
uint64_t sf_pack(bool neg,int e,uint64_t z){
    uint64_t s=sf_sign(neg);if(z==0)return s;
    int k=sf_top(z)-55;e+=k;
    if(k>0)z=sf_jam(z,uint(k));else if(k<0)z<<=-k;
    if(e < -1022){z=sf_jam(z,uint(-1022-e));e=-1022;}
    uint tail=uint(z)&7u;z>>=3;
    z+=uint64_t(tail>4u || (tail==4u && (z&1)!=0));
    if(z>=(SF_HIDDEN<<1u)){z>>=1;++e;}
    if(e>1023)return s|SF_INF;
    return s|(z<SF_HIDDEN?z:(uint64_t(e+1023)<<52u)|(z&(SF_HIDDEN-1)));
}
SfLong sf_long(uint64_t l,uint64_t h){SfLong v;v.l=l;v.h=h;return v;}
SfLong sf_left(SfLong v,uint n){
    if(!n)return v;if(n>=64)return sf_long(0,v.l<<(n-64));
    return sf_long(v.l<<n,(v.h<<n)|(v.l>>(64-n)));
}
SfLong sf_right(SfLong v,uint n){
    if(!n)return v;
    if(n>=128)return sf_long(uint64_t((v.l|v.h)!=0),0);
    if(n>=64)return sf_long(sf_jam(v.h,n-64)|uint64_t(v.l!=0),0);
    return sf_long((v.l>>n)|(v.h<<(64-n))|uint64_t((v.l<<(64-n))!=0),v.h>>n);
}
SfLong sf_long_add(SfLong a,SfLong b){uint64_t lo=a.l+b.l;return sf_long(lo,a.h+b.h+uint64_t(lo<a.l));}
SfLong sf_long_sub(SfLong a,SfLong b){return sf_long(a.l-b.l,a.h-b.h-uint64_t(a.l<b.l));}
bool sf_smaller(SfLong a,SfLong b){return a.h<b.h || (a.h==b.h && a.l<b.l);}
SfLong sf_product(uint64_t a,uint64_t b){
    uint64_t al=uint(a),ah=a>>32u,bl=uint(b),bh=b>>32u;
    uint64_t p=al*bl,q=ah*bl+(p>>32u),r=al*bh+uint(q);
    return sf_long((r<<32u)|uint(p),ah*bh+(q>>32u)+(r>>32u));
}
uint64_t sf_finish_long(bool neg,int e,SfLong z){
    if((z.h|z.l)==0)return sf_sign(neg);
    int top=z.h!=0?64+sf_top(z.h):sf_top(z.l);
    uint64_t sig=top>=55?sf_right(z,uint(top-55)).l:z.l<<(55-top);
    return sf_pack(neg,e+(top-122),sig);
}
// Operands represented at bit 122, leaving headroom for addition.
uint64_t sf_sum(bool na,int ea,SfLong a,bool nb,int eb,SfLong b){
    int e=max(ea,eb);a=sf_right(a,uint(e-ea));b=sf_right(b,uint(e-eb));
    if(na==nb)return sf_finish_long(na,e,sf_long_add(a,b));
    if(a.h==b.h && a.l==b.l)return 0;
    if(sf_smaller(a,b))return sf_finish_long(nb,e,sf_long_sub(b,a));
    return sf_finish_long(na,e,sf_long_sub(a,b));
}
uint64_t sf_add(uint64_t a,uint64_t b){
    uint64_t am=sf_mag(a),bm=sf_mag(b);
    if(am>SF_INF || bm>SF_INF)return SF_NAN;
    if(am==SF_INF || bm==SF_INF){if(am==bm && a!=b)return SF_NAN;return am==SF_INF?a:b;}
    if(am==0 || bm==0)return (am|bm)==0?(a&b):(am!=0?a:b);
    SfParts x=sf_parts(a),y=sf_parts(b);
    return sf_sum(x.neg,x.e,sf_left(sf_long(x.m,0),70),y.neg,y.e,sf_left(sf_long(y.m,0),70));
}
uint64_t sf_muladd(uint64_t a,uint64_t b,uint64_t c,bool fused){
    uint64_t am=sf_mag(a),bm=sf_mag(b),cm=sf_mag(c);bool neg=((a^b)&SF_SIGN)!=0;
    if(am>SF_INF || bm>SF_INF || (fused && cm>SF_INF))return SF_NAN;
    if(am==SF_INF || bm==SF_INF){
        if(am==0 || bm==0 || (fused && cm==SF_INF && neg!=((c&SF_SIGN)!=0)))return SF_NAN;
        return sf_sign(neg)|SF_INF;
    }
    if(fused && cm==SF_INF)return c;
    if(am==0 || bm==0)return fused?sf_add(sf_sign(neg),c):sf_sign(neg);
    SfParts x=sf_parts(a),y=sf_parts(b);SfLong prod=sf_product(x.m,y.m);
    int top=64+sf_top(prod.h),e=x.e+y.e+top-104;
    prod=sf_left(prod,uint(122-top));
    if(!fused || cm==0)return sf_finish_long(neg,e,prod);
    SfParts z=sf_parts(c);return sf_sum(neg,e,prod,z.neg,z.e,sf_left(sf_long(z.m,0),70));
}
uint64_t sf_rcp(uint64_t a){
    uint64_t mag=sf_mag(a),sign=a&SF_SIGN;if(mag>SF_INF)return SF_NAN;
    if(mag==0)return sign|SF_INF;if(mag==SF_INF)return sign;
    SfParts p=sf_parts(a);int e=-p.e;uint64_t n=SF_HIDDEN;
    if(n<p.m){n<<=1;--e;}n-=p.m;uint64_t q=1;
    [loop] for(uint i=0;i<55u;++i){n<<=1;q<<=1;if(n>=p.m){n-=p.m;q|=1;}}
    return sf_pack(p.neg,e,q|uint64_t(n!=0));
}
uint64_t sf_sqrt(uint64_t a){
    uint64_t mag=sf_mag(a);if(!mag || a==SF_INF)return a;if(a&SF_SIGN || mag>SF_INF)return SF_NAN;
    SfParts p=sf_parts(a);if(p.e&1)p.m<<=1;
    SfLong rad=sf_left(sf_long(p.m,0),58);uint64_t root=0,rem=0;
    [loop] for(int i=55;i>=0;--i){uint k=2*i;rem=(rem<<2u)|(((k>=64?rad.h:rad.l)>>(k&63u))&3u);
        uint64_t trial=(root<<2u)|1u;root<<=1;if(rem>=trial){rem-=trial;root|=1;}}
    return sf_pack(false,p.e>>1,root|uint64_t(rem!=0));
}
uint64_t sf_round(uint64_t a,uint mode){
    uint64_t mag=sf_mag(a);if(!mag||mag>=SF_INF)return a;
    SfParts p=sf_parts(a);if(p.e>=52)return a;uint64_t s=a&SF_SIGN;
    if(p.e<0){bool up=mode==1?p.neg:mode==2?!p.neg:mode==3?(p.e==-1 && p.m>SF_HIDDEN):false;
        return s|(up?(uint64_t(1023)<<52u):0);}
    uint64_t step=SF_ONE<<(52-p.e),frac=mag&(step-1),base=mag-frac;
    bool inc=mode==1?p.neg:mode==2?!p.neg:mode==3?(frac>step/2 || (frac==step/2 && (base&step)!=0)):false;
    return s|(base+((inc && frac!=0)?step:0));
}
uint64_t sf_from_int(uint v,bool sg){
    bool neg=sg&&asint(v)<0;uint m=neg?0u-v:v;if(!m)return 0;
    int e=int(firstbithigh(m));return sf_pack(neg,e,uint64_t(m)<<(55-e));
}
uint sf_to_int(uint64_t a,bool sg){
    uint64_t mag=sf_mag(a);if(!mag || mag>SF_INF)return 0;
    SfParts p=sf_parts(a);if(!sg && p.neg || p.e<0)return 0;
    if(p.e>=(sg?31:32))return sg?(p.neg?0x80000000u:0x7fffffffu):~0u;
    uint n=uint(p.m>>(52-p.e));return p.neg?0u-n:n;
}
uint64_t sf_from_f32(uint a){
    uint expv=(a>>23u)&255u,frac=a&0x7fffffu;bool neg=(a>>31u)!=0u;uint64_t s=sf_sign(neg);
    if(expv==255u)return s|SF_INF|(uint64_t(frac)<<29u);
    if(!expv&&!frac)return s;
    uint m=expv?frac|0x800000u:frac;int e=expv?int(expv)-127:-126;
    int k=23-int(firstbithigh(m));return sf_pack(neg,e-k,uint64_t(m)<<(32+k));
}
uint sf_to_f32(uint64_t a){
    uint64_t mag=sf_mag(a);uint s=uint(a>>32u)&0x80000000u;
    if(mag>=SF_INF)return s|(mag==SF_INF?0x7f800000u:0x7fc00000u);
    if(!mag)return s;
    SfParts p=sf_parts(a);uint64_t z=sf_jam(p.m,26);
    if(p.e< -126){z=sf_jam(z,uint(-126-p.e));p.e=-126;}
    uint tail=uint(z)&7u,m=uint(z>>3);m+=uint(tail>4u || (tail==4u && (m&1u)!=0));
    if(m>=0x1000000u){m>>=1;++p.e;}
    return s|(p.e>127?0x7f800000u:m<0x800000u?m:(uint(p.e+127)<<23u)|(m&0x7fffffu));
}
uint64_t sf_ldexp(uint64_t a,int delta){
    if(!sf_mag(a)||sf_mag(a)>=SF_INF)return a;
    SfParts p=sf_parts(a);return sf_pack(p.neg,p.e+clamp(delta,-4096,4096),p.m<<3u);
}
uint64_t sf_mant(uint64_t a){
    if(!sf_mag(a)||sf_mag(a)>=SF_INF)return a;
    SfParts p=sf_parts(a);return sf_pack(p.neg,-1,p.m<<3u);
}
int sf_exp(uint64_t a){if(!sf_mag(a)||sf_mag(a)>=SF_INF)return 0;SfParts p=sf_parts(a);return p.e+1;}
)";
}
