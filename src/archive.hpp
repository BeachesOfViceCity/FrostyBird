#pragma once

#include "bytes.hpp"

namespace frosty {
struct ShaderEntry {
    Bytes stored;
    u32 unpacked_size;
    unsigned frequency;
    Buffer unpack() const;
};

class ShaderArchive {
    Bytes entries_, payload_, map_hashes_, shader_hashes_, maps_, indices_;
public:
    explicit ShaderArchive(Bytes archive);
    size_t size() const { return entries_.size() / 17; }
    ShaderEntry operator[](u32 i) const;
    size_t map_count() const {return map_hashes_.size()/20;}
    Bytes map_hash(u32 i) const {return map_hashes_.slice(u64(i)*20,20);}
    Bytes shader_hash(u32 i) const {return shader_hashes_.slice(u64(i)*20,20);}
    Bytes map_indices(u32 i) const;
};

// View into a memory-mapped Pak; no data copied when locating an archive.
ShaderArchive open_library(Bytes pak, std::string_view library);
Buffer global_shader_cache(Bytes pak);
} // namespace frosty
