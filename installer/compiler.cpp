#include "core.hpp"
#include "rt_machine.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace fb {
void check_request(u32 wire, size_t size, size_t n) {
    auto s = wire & 255, c = wire >> 8;
    if ((s != 0 && s != 1 && s != 2 && s != 5 && s != 6 && s != 7 && s != 8 && s != 129) || c > 15 ||
        (c && s != 5) || size < 32 || size > 16u << 20 || n > (s == 6 ? 8192u : 256u) ||
        ((s == 7 || s == 8) && n < 13) || (s == 6 && n <= 16))
        throw Error("Invalid shader request");
}
static Buffer translate(Data raw, u32 wire, Data params) {
    u32 stage = wire & 255;
    Buffer code;
    std::vector<u32> views;
    if (stage >= 6 && stage <= 8) {
        Bytes p{params};
        auto c = rt_collection({raw});
        u32 words = p.at<u32>(0);
        if (!words || words > 32)
            throw Error("Invalid ray payload width");
        if (stage == 6) {
            RayContract shape{p.at<u32>(4), p.at<u32>(8), p.at<u32>(12)};
            if (shape.selector > 0xffffff || shape.word >= words)
                throw Error("Invalid raygen contract");
            auto l = ray_live_source(c, words, shape,
                                     {reinterpret_cast<const char*>(params.data() + 16), params.size() - 16});
            views = std::move(l.bindings);
            code = compile_rt_library(l.source);
        } else {
            auto field = p.at<u32>(4);
            if (field >= words)
                throw Error("Invalid ray payload index");
            std::string sym(params.begin() + 12, params.end());
            auto& m = c.modules.back();
            auto e = std::ranges::find_if(m.exports, [&](auto& entry) {
                return !(entry.flags & 3) && entry.type == (stage == 8 ? 2u : 4u) && entry.symbols[0] == sym;
            });
            if (e == m.exports.end())
                throw Error("Missing recorded ray export");
            code = compile_rt_library(stage == 7 ? rt_source(c, 7, words, {})
                                                 : ray_hit_specialization(m, *e, words, field, p.at<u32>(8)));
        }
    } else {
        auto s = inspect_shader({raw});
        if (s.dxbc.size() != raw.size() || s.stage != (stage == 129 ? 1 : stage))
            throw Error("Stage does not match source");
        auto t = stage == 129 ? isa::rectangle_assembly(s) : isa::recompile(s, wire >> 8);
        code = isa::compile_dxil(t, params.empty() ? "" : "x" + hex(params));
        views = std::move(t.views);
    }
    u32 h[]{u32(views.size()), u32(code.size())};
    Buffer out(8 + views.size() * 4 + code.size());
    std::memcpy(out.data(), h, 8);
    if (!views.empty())
        std::memcpy(out.data() + 8, views.data(), 4 * views.size());
    std::memcpy(out.data() + 8 + 4 * views.size(), code.data(), code.size());
    return out;
}
Cache::Cache(const fs::path& p) : packets(p / "packets") { fs::create_directories(packets); }
Buffer Cache::resolve(Data raw, const Request& r) {
    check_request(r.wire, raw.size(), r.params.size());
    auto s = r.wire & 255;
    Data params = r.params;
    if ((s < 6 || s > 8) && std::ranges::all_of(params, [](u8 b) { return !b; }))
        params = {};
    auto source = sha(raw);
    auto name = source + "." + std::to_string(r.wire) + "." + sha(params);
    auto path = packets / name;
    std::lock_guard guard(locks[std::stoul(source.substr(0, 2), nullptr, 16) % locks.size()]);
    auto seal = [&](Data packet) {
        Buffer bound(name.begin(), name.end());
        bound.insert(bound.end(), packet.begin(), packet.end());
        return unhex(sha(bound));
    };
    if (fs::is_regular_file(path)) {
        auto b = read(path, 17u << 20);
        if (b.size() >= 72) {
            Bytes p{Data(b).subspan(32)};
            auto n = p.at<u32>(0), len = p.at<u32>(4);
            if (n <= 256 && len >= 32 && p.size() == 8 + 4ull * n + len && p.tag(8 + 4 * n, "DXBC") &&
                std::ranges::equal(seal(p.data), Data(b).first(32)))
                return {p.data.begin(), p.data.end()};
        }
    }
    auto packet = translate(raw, r.wire, params);
    auto disk = seal(packet);
    disk.insert(disk.end(), packet.begin(), packet.end());
    write(path, disk);
    return packet;
}
} // namespace fb
