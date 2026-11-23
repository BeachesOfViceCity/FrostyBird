#pragma once

#include "bytes.hpp"

namespace frosty {
struct Unsupported : Error { using Error::Error; };
struct Shader {
    Bytes dxbc, dxil, bitcode, xbch;
    u32 stage{};
    u64 key() const;
    std::string_view format() const;
    Buffer machine_object() const;
};
Shader inspect_shader(Bytes ue_record);
} // namespace frosty
