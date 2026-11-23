#pragma once

// A shader request may depend on the bound SRV's original image dimensions
// as well as its numeric type. Each selector transports one byte so existing
// variant caches can key on the exact data used at the draw.
namespace frosty::view {
constexpr unsigned image=0x400, field_shift=11, atomic64=0x80;
constexpr unsigned table_word=0x10000, table_space=0x7a31, table_count=16;
constexpr unsigned buffer_word=0x40000000u;
// Raygen replies list only the global bindings actually consumed by the ISA.
constexpr unsigned ray_as=0x20000u;
constexpr unsigned buffer_key(unsigned slot,unsigned part,unsigned binding){return buffer_word|(part<<17)|(slot<<field_shift)|binding;}
constexpr unsigned sampler=0x80000000u;
// One packed pair of address modes, then four bytes per floating-point field.
constexpr unsigned sampler_addr=0,sampler_min=1,sampler_max=5,sampler_bias=9,sampler_border=13,sampler_bytes=29;
constexpr unsigned sampler_key(unsigned reg,unsigned byte){return sampler|(byte<<field_shift)|reg;}
// For a compute shader which actually needs raw user-SGPR bits at a CFG
// join: map an original table to a private root constant using one of its
// real register bindings. The low DWORD comes from the title's GPU handle.
constexpr unsigned table_key(unsigned slot,unsigned binding){return table_word | (slot<<field_shift) | binding;}
enum Field : unsigned { width_lo, width_hi, height_lo, height_hi, base_mip, min_lod0,min_lod1,min_lod2,min_lod3,fields };
constexpr unsigned key(unsigned reg,unsigned field){return image | (field<<field_shift) | reg;}
constexpr bool valid(unsigned k){
    if(k&sampler)return !(k&0x780u) && ((k&~sampler)>>field_shift)<sampler_bytes;
    if(k&buffer_word)return !(k&~(buffer_word|(3u<<17)|((table_count-1)<<field_shift)|0x17fu)) && ((k>>17)&3u)<3u;
    if(k&table_word)return !(k&~(table_word|((table_count-1)<<field_shift)|0x37fu));
    return k&image ? (k>>field_shift)<fields && !(k&0x380) : !(k&~0x3ffu) && (!(k&atomic64) || (k&0x100));
}
}
