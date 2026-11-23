#pragma once
#include "bytes.hpp"
#include <array>

namespace frosty {
struct RtEntry {std::string name; u32 offset;};
struct RtExport {
    std::string name;
    std::array<std::string,4> symbols; // public function/group, closest hit, any hit, intersection
    u32 frame,stage_frame,type,flags;
};
struct RtModule {
    Bytes code,metadata,root;
    std::vector<RtEntry> entries;
    std::vector<RtExport> exports;
    u32 stack{};
};
struct RtCollection {Bytes blob;std::vector<RtModule> modules;};
Bytes rt_record_blob(Bytes record);
RtCollection rt_collection(Bytes blob);
std::string rt_listing(const RtModule&,const RtEntry&);
u64 rt_key(Bytes);
// `layout` supplies D3D structured-view strides, e.g. "t1=32,u0=4".
std::string rt_source(const RtCollection&,unsigned frequency,unsigned payload_words,std::string_view layout);
std::string rt_guard_source(const RtCollection&); // diagnostic, no title export
Buffer compile_rt_library(std::string_view);
}
