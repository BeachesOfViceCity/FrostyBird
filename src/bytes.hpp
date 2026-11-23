#pragma once

#include <bit>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace frosty {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using Buffer = std::vector<u8>;
using Error = std::runtime_error;

// PE, Pak, DXBC and the shader machine words are all little-endian. Many of
// the Xbox DXBC chunks are *not* word-aligned.
struct Bytes {
    std::span<const u8> data;

    size_t size() const { return data.size(); }
    Bytes slice(u64 pos, u64 count) const {
        if (pos > size() || count > size() - pos)
            throw Error("offset or length outside binary record");
        return {data.subspan(pos, count)};
    }
    Bytes tail(u64 pos) const {
        if (pos > size()) throw Error("offset outside binary record");
        return {data.subspan(pos)};
    }
    template<std::unsigned_integral T> T at(u64 pos) const {
        T value;
        std::memcpy(&value, slice(pos, sizeof(T)).data.data(), sizeof(T));
        if constexpr (std::endian::native == std::endian::big)
            value = std::byteswap(value);
        return value;
    }
    bool tag(u64 pos, std::string_view s) const {
        return pos <= size() && s.size() <= size() - pos &&
               !std::memcmp(data.data() + pos, s.data(), s.size());
    }
};

struct Cursor {
    Bytes rest;
    Bytes take(u64 n) {
        auto ret = rest.slice(0, n);
        rest = rest.tail(n);
        return ret;
    }
    u32 word() { return take(4).at<u32>(0); }
    Bytes array(u32 stride) { return take(u64(word()) * stride); }
};
} // namespace frosty
