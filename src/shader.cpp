#include "shader.hpp"
#include <algorithm>
#include <zlib.h>

namespace frosty {
namespace {
Shader container(Bytes input) {
    if (input.at<u32>(20) != 1) throw Error("invalid DXBC version");
    Shader s{.dxbc = input.slice(0, input.at<u32>(24)), .dxil={}, .bitcode={}, .xbch={}};
    const auto count = s.dxbc.at<u32>(28);
    const auto offsets = s.dxbc.slice(32, u64(count) * 4);
    for (u32 i = 0; i < count; ++i) {
        const auto off = offsets.at<u32>(4ull * i);
        if (off < 32 + offsets.size()) throw Error("DXBC chunk overlaps directory");
        auto chunk = s.dxbc.tail(off);
        auto payload = chunk.slice(8, chunk.at<u32>(4));
        if (!chunk.tag(0, "DXIL")) continue;
        if (s.dxil.size() || !payload.tag(8, "DXIL")) throw Error("invalid DXIL header");
        s.dxil = payload;
        s.stage = payload.at<u32>(0) >> 16;
        s.bitcode = payload.slice(8ull + payload.at<u32>(16), payload.at<u32>(20));
        if (payload.tag(24, "XDAT")) {
            const auto n = payload.at<u32>(28);
            const auto table = payload.slice(payload.at<u32>(32), u64(n) * 12);
            for (u32 j = 0; j < n; ++j) {
                auto row = table.slice(12ull * j, 12);
                auto part = payload.slice(row.at<u32>(4), row.at<u32>(8));
                if (row.at<u32>(0) == 13 && part.tag(0, "XBCH")) s.xbch = part;
            }
        }
    }
    return s;
}
} // namespace

Shader inspect_shader(Bytes record) {
    // UE prepends variable-length resource/ray metadata. Prefer a shader
    // program over an embedded root-signature-only DXBC in the same record.
    constexpr std::string_view magic = "DXBC";
    Shader other{};
    while (record.size() >= 32) {
        auto it = std::search(record.data.begin(), record.data.end(), magic.begin(), magic.end());
        if (it == record.data.end()) break;
        record = record.tail(it - record.data.begin());
        try {
            auto s = container(record);
            if (s.dxil.size()) return s;
            if (!other.dxbc.size()) other = s;
            record = record.tail(s.dxbc.size());
        } catch (const Error&) {
            record = record.tail(4); // incidental signature in UE metadata
        }
    }
    if (other.dxbc.size()) return other;
    throw Error("no well-formed DXBC container in shader record");
}

std::string_view Shader::format() const {
    if (bitcode.tag(0, "BC\xc0\xde")) return "DXIL with LLVM bitcode";
    if (bitcode.size() == 4 && bitcode.tag(0, "NOBC") && xbch.size())
        return "XSX machine shader (XDAT/XBCH, NOBC)";
    return dxil.size() ? "unknown DXIL payload" : "non-program DXBC / unsupported record";
}

u64 Shader::key() const {
    // vkd3d_shader_hash() uses FNV-1: multiply *before* xor.
    u64 h = 0xcbf29ce484222325ull;
    for (auto byte : dxbc.data) h = (h * 0x100000001b3ull) ^ byte;
    return h;
}

Buffer Shader::machine_object() const {
    if (!xbch.size() || xbch.at<u32>(4) != 3) throw Error("expected an XBCH v3 machine object");
    auto stored = xbch.slice(24, xbch.at<u64>(8));
    const auto size = xbch.at<u64>(16);
    if (size < 16 || size > 64 * 1024 * 1024 || stored.size() != xbch.size() - 24)
        throw Error("invalid XBCH lengths");
    Buffer out(size);
    uLongf n = size;
    uLong src_len = stored.size();
    if (uncompress2(out.data(), &n, stored.data.data(), &src_len) != Z_OK ||
        n != size || src_len != stored.size())
        throw Error("invalid zlib stream in XBCH");
    Bytes obj{out};
    if (!obj.tag(0, "FSSM") || obj.at<u32>(4) != 0x101 || obj.at<u64>(8) != size - 16)
        throw Error("unsupported FSSM object header");
    return out;
}
} // namespace frosty
