#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#if defined(__x86_64__) && defined(__GNUC__)
#include <smmintrin.h>
#endif

namespace sbt_prefix {
inline constexpr std::uint64_t marker=0x3154525358595453ull;
inline bool ordinary(const unsigned char* p,unsigned records){
    for(unsigned i=0;i<records;++i){std::uint64_t v;std::memcpy(&v,p+32*i,8);if(v==marker)return true;}
    return false;
}
#if defined(__x86_64__) && defined(__GNUC__)
// Upload mappings may be write-combined BAR memory. MOVNTDQA can read a
// WC line into a streaming load buffer; compare independent record headers
// in batches rather than serializing 128 scalar PCIe reads and branches.
__attribute__((target("sse4.1"))) inline bool streaming(const unsigned char* p,unsigned records){
    const auto want=_mm_set1_epi64x(marker);
    auto match=[&](unsigned i) __attribute__((target("sse4.1"))) {
        return _mm_cmpeq_epi64(_mm_stream_load_si128(reinterpret_cast<__m128i*>(const_cast<unsigned char*>(p+32*i))),want);};
    unsigned i=0;for(;records-i>=8;i+=8){
        auto a=_mm_or_si128(match(i),match(i+1)),b=_mm_or_si128(match(i+2),match(i+3));
        auto c=_mm_or_si128(match(i+4),match(i+5)),d=_mm_or_si128(match(i+6),match(i+7));
        auto any=_mm_or_si128(_mm_or_si128(a,b),_mm_or_si128(c,d));
        if(_mm_movemask_pd(_mm_castsi128_pd(any))&1)return true;
    }
    for(;i<records;++i)if(_mm_movemask_pd(_mm_castsi128_pd(match(i)))&1)return true;
    return false;
}
#endif
inline bool contains(const unsigned char* p,std::uint64_t bytes){
    unsigned records=unsigned(std::min<std::uint64_t>(bytes,4096)/32);
#if defined(__x86_64__) && defined(__GNUC__)
    static const bool supported=__builtin_cpu_supports("sse4.1");
    if(supported && !(reinterpret_cast<std::uintptr_t>(p)&15) && records>=8)return streaming(p,records);
#endif
    return ordinary(p,records);
}
}
