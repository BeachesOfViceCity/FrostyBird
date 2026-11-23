#pragma once
#include "isa.hpp"
#include <set>

namespace frosty::isa {
// A word in a single uncompressed representation of the committed hit.
// The address calculation is interpreted at lift time; none of these are
// Vulkan device addresses and no host BVH layout enters the shader.
struct HitWord {
    enum Field { literal, transform, vertex, instance_id, instance_index, sbt_offset, geometry_word } field;
    unsigned n{};
};
// Contract of the compiler's inlined opaque triangle query. Register names
// are recovered from data flow; there is no shader/archive key in this pass.
struct InlineTrace {
    unsigned begin{},entry{},end{},mask{};
    int facing{}; // positive determinant = front by default on BVH64
    std::array<std::string,6> world;
    std::string as_page,tmin,tmax,committed,status,limit;
    std::string leaf,hit_t,base_lo,base_hi;
    unsigned post_begin{},post_end{};
    std::map<unsigned,std::vector<HitWord>> reads;
    std::map<unsigned,unsigned> post_edges;
    std::set<std::string> clobbers;
};
std::map<unsigned,InlineTrace> inline_visibility(const Program&,const Graph&);
}
