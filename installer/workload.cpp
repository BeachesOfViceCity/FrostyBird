#include "core.hpp"
#include "archive.hpp"
#include "rt_machine.hpp"
#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <sstream>
#include <thread>

namespace fb {
Buffer stage_input(Bytes r, unsigned freq) {
    auto b = freq >= 6 && freq <= 8 ? rt_record_blob(r) : inspect_shader(r).dxbc;
    return {b.data.begin(), b.data.end()};
}
Plan split_plan(Data bytes) {
    Cursor c{{bytes}};
    if (c.word() != 0x31505358)
        throw Error("Unknown pipeline format");
    auto kind = c.word();
    if (kind != 1 && kind != 2)
        throw Error("Not a graphics/compute pipeline");
    auto root = c.take(c.word()); // node mask + a root-signature-only DXBC (no executable chunks)
    if (root.size() < 48 || !root.tag(4, "DXBC") || root.at<u32>(28) != root.size() - 4 ||
        root.at<u32>(32) != 1 || root.at<u32>(36) != 36 || !root.tag(40, "RTS0") ||
        root.at<u32>(44) != root.size() - 48)
        throw Error("Unexpected root signature");
    if (kind == 1)
        c.take(8);
    else {
        c.take(32);
        auto n = c.word();
        if (n > 8)
            throw Error("Invalid render targets");
        c.take(4 * n + 4 + 8 + 8 * 37 + 44 + 50);
        auto count = c.word();
        if (count > 32)
            throw Error("Invalid vertex layout");
        while (count--) {
            auto field = c.take(c.word());
            if (field.size() > 256 || field.size() == 0 || field.data.back() != 0)
                throw Error("Invalid vertex semantic");
            c.take(24);
        }
    }
    Plan out;
    auto prefix = bytes.first(bytes.size() - c.rest.size());
    out.state.assign(prefix.begin(), prefix.end());
    for (unsigned i = 0; i < (kind == 1 ? 1u : 3u); ++i) {
        auto b = c.take(c.word());
        out.shaders.emplace_back(b.data.begin(), b.data.end());
    }
    if (c.rest.size())
        throw Error("Trailing pipeline data");
    return out;
}
static void rays(const json& rows, const std::map<std::string, Buffer>& raw, const fs::path& dir) {
    fs::create_directories(dir);
    for (const auto& r : rows) {
        const auto& b = raw.at(r.at(0).get<std::string>());
        auto c = rt_collection({b});
        auto& m = c.modules.back();
        auto base = (dir / std::format("{:016x}", rt_key(c.blob))).string();
        auto kind = r.at(1).get<unsigned>();
        auto desc = r.at(2).get<std::string>();
        std::istringstream in(desc);
        u32 words{}, route{}, field{}, value{}, as{}, count{};
        std::string source;
        if (!(in >> words) || !words || words > 32)
            throw Error("Bad recorded ray payload");
        if (kind == 6) {
            if (!(in >> route >> field >> value >> as >> count) || count > 256 || field >= words)
                throw Error("Bad binding contract");
            std::string layout, ty;
            u32 reg, dim;
            for (u32 i = 0; i < count; ++i) {
                if (!(in >> reg >> dim >> ty))
                    throw Error("Bad ray binding");
                if (i)
                    layout += ",";
                layout += std::format("{}{}={}", reg & 256 ? 'u' : 't', reg & 255, ty);
            }
            auto e = std::ranges::find_if(m.exports, [](auto& v) { return !v.type && !(v.flags & 3); });
            if (e == m.exports.end())
                throw Error("Missing raygen");
            source = ray_machine_source(m, *e, words, layout, RayContract{route, field, value});
            write(base + ".checked.dxil", compile_rt_library(source));
            write(base + ".checked.plan", desc);
        } else if (kind == 8) {
            std::string symbol;
            if (!(in >> field >> value >> symbol) || field >= words)
                throw Error("Bad hit contract");
            auto e = std::ranges::find_if(
                m.exports, [&](auto& v) { return v.type == 2 && !(v.flags & 3) && v.symbols[0] == symbol; });
            if (e == m.exports.end())
                throw Error("Missing closest-hit export");
            write(base + ".hit.dxil", compile_rt_library(ray_hit_specialization(m, *e, words, field, value)));
            write(base + ".hit.plan", desc);
        } else if (kind == 7)
            write(base + ".rt.dxil", compile_rt_library(rt_source(c, 7, words, {})));
        else
            throw Error("Unknown ray profile");
        u32 abi[]{words * 4, 8};
        write(base + ".rt.abi", Data{reinterpret_cast<const u8*>(abi), 8});
        write(base + ".xsrt", b);
    }
}
void warm(const fs::path& pak, const json& profile, const fs::path& plans, Cache& cache, u32 caps,
          const Progress& progress) {
    Map file(pak);
    auto g = open_library(file.bytes(), "Global"), f = open_library(file.bytes(), "Frosty");
    std::map<std::string, Buffer> raw;
    struct Work {
        const Buffer* raw;
        Request req;
    };
    std::vector<Work> work;
    for (const auto& row : profile.at("sources")) {
        int lib = row[0];
        if (lib != 0 && lib != 1)
            throw Error("Invalid workload library");
        auto e = (lib ? f : g)[row[1].get<u32>()];
        auto unpacked = e.unpack();
        auto b = stage_input({unpacked}, e.frequency);
        auto digest = sha(b);
        if (digest != row[2].get<std::string>())
            throw Error(
                "The game shader archive differs from this workload. Select the supported original package.");
        auto& bytes = raw.emplace(digest, std::move(b)).first->second;
        std::set<std::pair<u32, std::string>> seen;
        for (auto& v : row[3]) {
            u32 w = v[0];
            if ((w & 255) == 5)
                w = 5 | (caps << 8);
            std::string p = v[1];
            if (seen.emplace(w, p).second)
                work.push_back({&bytes, {w, unhex(p)}});
        }
    }
    if (work.empty())
        throw Error("Release contains no recorded shaders");
    size_t done = 0;
    std::atomic<size_t> next{};
    std::atomic<bool> failed{};
    std::mutex guard;
    std::exception_ptr error;
    auto worker = [&] {
        try {
            for (;;) {
                auto i = next.fetch_add(1);
                if (i >= work.size() || failed)
                    break;
                cache.resolve(*work[i].raw, work[i].req);
                std::lock_guard lock(guard);
                ++done;
                if (done == 1 || done % 32 == 0 || done == work.size())
                    progress(float(done) / work.size() * .88f,
                             std::format("{} / {} shader variants", done, work.size()));
            }
        } catch (...) {
            failed = true;
            std::lock_guard lock(guard);
            if (!error)
                error = std::current_exception();
        }
    };
    {
        std::vector<std::jthread> workers;
        for (unsigned i = 0; i < std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u); ++i)
            workers.emplace_back(worker);
    }
    if (error)
        std::rethrow_exception(error);
    progress(.89f, "Preparing ray-tracing shaders");
    auto raydir = plans.parent_path() / "rays", libs = plans.parent_path() / "collections";
    fs::create_directories(libs);
    rays(profile.at("rays"), raw, raydir);
    for (auto& r : profile.at("collections")) {
        auto& b = raw.at(r[0].get<std::string>());
        auto c = rt_collection({b});
        u32 words = r[2];
        auto base = (libs / std::format("{:016x}", rt_key(c.blob))).string();
        u32 abi[]{4 * words, 8};
        write(base + ".rt.dxil",
              compile_rt_library(rt_source(c, r[1].get<u32>(), words, r[3].get<std::string>())));
        write(base + ".xsrt", b);
        write(base + ".rt.abi", Data{reinterpret_cast<const u8*>(abi), 8});
    }
    fs::create_directories(plans);
    size_t n = 0;
    for (const auto& r : profile.at("pipelines")) {
        Buffer plan = unhex(r[1].get<std::string>());
        for (const auto& ref : r[2]) {
            auto id = ref.get<std::string>();
            Data b = id.empty() ? Data{} : Data(raw.at(id));
            u32 len = b.size();
            auto p = reinterpret_cast<const u8*>(&len);
            plan.insert(plan.end(), p, p + 4);
            plan.insert(plan.end(), b.begin(), b.end());
        }
        auto key = sha(plan);
        if (key != r[0].get<std::string>())
            throw Error("Reconstructed pipeline does not match recording");
        write(plans / (key + ".plan"), plan);
        for (auto& h : r[3]) {
            auto b = unhex(h.get<std::string>());
            write(plans / (key + "." + sha(b) + ".hint"), b);
        }
        if (++n % 256 == 0)
            progress(.91f + .09f * float(n) / profile.at("pipelines").size(),
                     "Restoring recorded pipeline states");
    }
    for (auto& r : profile.at("ray_pipelines")) {
        auto plan = unhex(r[0].get<std::string>());
        for (auto& entry : r[1]) {
            auto names = unhex(entry[0].get<std::string>());
            auto& ref = entry[1];
            auto& b = raw.at(ref[1].get<std::string>());
            u32 kind = ref[2];
            Buffer code;
            if (ref[0] == "prepared") {
                auto ext = kind == 6 ? ".checked.dxil" : kind == 8 ? ".hit.dxil" : ".rt.dxil";
                code = read(raydir / std::format("{:016x}{}", rt_key({b}), ext));
            } else {
                auto packet = cache.resolve(b, {kind, unhex(ref.at(3).get<std::string>())});
                Bytes p{packet};
                auto bytes = p.tail(8 + 4ull * p.at<u32>(0)).data;
                code.assign(bytes.begin(), bytes.end());
            }
            plan.insert(plan.end(), names.begin(), names.end());
            u32 len = code.size();
            auto p = reinterpret_cast<u8*>(&len);
            plan.insert(plan.end(), p, p + 4);
            plan.insert(plan.end(), code.begin(), code.end());
        }
        write(plans / (sha(plan) + ".plan"), plan);
    }
}
} // namespace fb
