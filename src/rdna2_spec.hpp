#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
namespace frosty::isa {
struct OpcodeSpec {
    std::string_view name,family,group;unsigned encodings,flags,size;
    std::array<std::uint32_t,3> specimen;
};
inline constexpr OpcodeSpec catalog[]{
#include "rdna2_catalog.inc"
};
static_assert(std::ranges::is_sorted(catalog,{},&OpcodeSpec::name));
inline const OpcodeSpec* opcode_spec(std::string_view op){
    auto it=std::ranges::lower_bound(catalog,op,{},&OpcodeSpec::name);
    return it!=std::end(catalog) && it->name==op?&*it:nullptr;
}
}
