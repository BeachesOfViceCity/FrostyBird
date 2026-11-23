#pragma once
#include "rt_collection.hpp"
#include "isa.hpp"
#include <map>
#include <set>
#include <optional>

namespace frosty {
struct RayBind {
    u32 type{},reg{},space{};
    auto operator<=>(const RayBind&)const=default;
    std::string name()const;
};
enum class RayView { cb, tex2, tex3, rwtex2, typed, accel, buffer_words };
struct RayUse { RayView view; u32 words{}; };
struct RayBlock { u32 pc; std::vector<isa::Instruction> ins;std::vector<u32> edges; };
struct RayPart { u32 start;std::map<u32,RayBlock> blocks;std::vector<u32> order; };
struct RayProgram {
    std::vector<RayPart> parts;
    std::map<u32,RayBind> access; // actual instruction PC, not a shader key
    std::map<RayBind,RayUse> used;
    std::map<u32,std::string> special; // recovered launch/SRD input operations
    u32 stack_words{},frame_words{};
};
RayProgram ray_program(const RtModule&,const RtExport&);
struct RayContract{u32 selector,word,value;};
struct RayLiveSource{std::string source;std::vector<u32> bindings;};
RayLiveSource ray_live_source(const RtCollection&,u32,RayContract,std::string_view);
std::string ray_machine_source(const RtModule&,const RtExport&,unsigned,std::string_view,std::optional<RayContract> = {});
std::string ray_hit_specialization(const RtModule&,const RtExport&,u32 words,u32 known_word,u32 value);
}
