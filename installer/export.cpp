// Maintainer tool. Publish record locations, SHA-256s, bindings and API state,
// never the recorded shader bytecode, translated packets or device caches.
#include "core.hpp"
#include "archive.hpp"
#include <iostream>
#include <map>
#include <regex>
#include <set>

int main(int argc, char** argv) try {
    using namespace fb;
    if (argc != 6) {
        std::cerr << "frostybird-profile GAME.pak SPECIALIZATIONS PREPARED_RAYS COLLECTIONS OUTPUT.json.gz\n";
        return 2;
    }
    fs::path cache = argv[2], raydir = argv[3], collections = argv[4];
    json j = {{"version", 1},          {"sources", json::array()},     {"pipelines", json::array()},
              {"rays", json::array()}, {"collections", json::array()}, {"ray_pipelines", json::array()}};
    std::map<std::string, json> products, standalone;
    std::vector<fs::path> ray_requests, ray_plans;
    std::map<std::string, std::set<std::pair<u32, std::string>>> wanted;
    std::set<std::string> found;
    auto workload = cache / "workload-v1";
    for (auto& it : fs::directory_iterator(workload / "recipes")) {
        auto name = it.path().filename().string();
        auto dot = name.find('.'), last = name.rfind('.');
        if (dot != 64 || last == dot)
            continue;
        auto d = name.substr(0, dot);
        auto p = read(it.path(), 8192);
        if (sha(p) != name.substr(last + 1))
            throw Error("Damaged recorded parameters");
        u32 w = std::stoul(name.substr(dot + 1, last - dot - 1));
        wanted[d].emplace(w, hex(p));
        if (w >= 6 && w <= 8)
            ray_requests.push_back(it.path());
    }
    auto dir = cache / "pipeline-recipes-v1";
    std::map<std::string, std::set<std::string>> hints;
    std::map<std::string, json> plans;
    for (auto& it : fs::directory_iterator(dir)) {
        auto name = it.path().filename().string();
        if (name.size() == 134 && name.ends_with(".hint")) {
            auto b = read(it.path(), 512);
            if (sha(b) == name.substr(65, 64))
                hints[name.substr(0, 64)].insert(hex(b));
        }
        if (name.size() != 69 || !name.ends_with(".plan"))
            continue;
        auto b = read(it.path());
        if (sha(b) != it.path().stem().string())
            throw Error("Damaged pipeline recipe");
        if (Bytes{b}.at<u32>(4) == 3) {
            ray_plans.push_back(it.path());
            continue;
        }
        auto p = split_plan(b);
        std::vector<std::string> refs;
        for (auto& s : p.shaders) {
            auto a = s.empty() ? "" : sha(s);
            refs.push_back(a);
            if (!a.empty())
                wanted.try_emplace(a);
        }
        auto k = it.path().stem().string();
        plans[k] = {k, hex(p.state), refs};
    }
    std::map<std::string, json> rayrows;
    for (auto& it : fs::directory_iterator(raydir)) {
        if (it.path().extension() != ".xsrt")
            continue;
        auto base = (raydir / it.path().stem()).string();
        auto d = file_sha(it.path());
        std::string desc;
        int kind;
        if (fs::exists(base + ".checked.plan")) {
            desc = text(base + ".checked.plan");
            kind = 6;
        } else if (fs::exists(base + ".hit.plan")) {
            desc = text(base + ".hit.plan");
            kind = 8;
        } else if (fs::exists(base + ".rt.dxil")) {
            auto abi = read(base + ".rt.abi");
            desc = std::to_string(Bytes{abi}.at<u32>(0) / 4);
            kind = 7;
        } else
            continue;
        wanted.try_emplace(d);
        rayrows[d] = {d, kind, desc};
        auto ext = kind == 6 ? ".checked.dxil" : kind == 8 ? ".hit.dxil" : ".rt.dxil";
        products[file_sha(base + ext)] = {"prepared", d, kind};
    }
    // Only index the few observed RT requests, not the entire packet cache.
    for (auto& dir : fs::directory_iterator(cache / "packets-v1"))
        for (auto& path : ray_requests) {
            auto file = dir.path() / path.filename();
            if (!fs::is_regular_file(file))
                continue;
            auto b = read(file, 17u << 20);
            if (b.size() < 72)
                continue;
            Bytes packet{Data(b).subspan(32)};
            auto n = packet.at<u32>(0), len = packet.at<u32>(4);
            if (n > 256 || packet.size() != 8 + 4ull * n + len)
                continue;
            auto name = path.filename().string();
            auto wire = std::stoul(name.substr(65));
            products.try_emplace(sha(packet.tail(8 + 4 * n).data),
                                 json{"request", name.substr(0, 64), wire, hex(read(path, 8192))});
        }
    const std::regex stride(R"(struct E_([tu][0-9]+) \{uint w\[([0-9]+)\];\};)");
    for (auto& it : fs::directory_iterator(collections)) {
        if (it.path().extension() != ".xsrt")
            continue;
        auto d = file_sha(it.path()), base = (collections / it.path().stem()).string(),
             hlsl = text(base + ".rt.hlsl");
        std::string layout;
        for (std::sregex_iterator i(hlsl.begin(), hlsl.end(), stride), end; i != end; ++i) {
            if (!layout.empty())
                layout += ",";
            layout += (*i)[1].str() + "=" + std::to_string(4 * std::stoul((*i)[2]));
        }
        auto abi = read(base + ".rt.abi");
        standalone[d] = {Bytes{abi}.at<u32>(0) / 4, layout};
        wanted.try_emplace(d);
    }
    Map pak(argv[1]);
    size_t variants = 0;
    for (unsigned id : {0u, 1u}) {
        auto ar = open_library(pak.bytes(), id ? "Frosty" : "Global");
        for (u32 i = 0; i < ar.size(); ++i) {
            auto e = ar[i];
            if (e.frequency > 8)
                continue;
            auto rb = e.unpack();
            Buffer raw;
            try {
                raw = stage_input({rb}, e.frequency);
            } catch (const Error&) {
                continue;
            }
            auto d = sha(raw);
            auto v = wanted.find(d);
            if (v == wanted.end() || !found.insert(d).second)
                continue;
            if (standalone.contains(d))
                j["collections"].push_back({d, e.frequency, standalone.at(d)[0], standalone.at(d)[1]});
            j["sources"].push_back({id, i, d, v->second});
            variants += v->second.size();
        }
    }
    for (auto& [key, v] : plans) {
        bool ok = true;
        for (std::string d : v[2])
            if (!d.empty() && !found.contains(d))
                ok = false;
        if (ok) {
            v.push_back(hints[key]);
            j["pipelines"].push_back(v);
        }
    }
    for (auto& [key, r] : rayrows)
        if (found.contains(key))
            j["rays"].push_back(r);
    std::sort(ray_plans.begin(), ray_plans.end());
    for (auto& path : ray_plans) {
        auto b = read(path);
        Cursor c{{b}};
        c.take(8);
        auto root = c.take(c.word());
        c.take(16);
        auto local = c.take(c.word());
        auto root_only = [](Bytes rs) {
            return rs.tag(0, "DXBC") && rs.at<u32>(24) == rs.size() && rs.at<u32>(28) == 1 &&
                   rs.at<u32>(32) == 36 && rs.tag(36, "RTS0") && rs.at<u32>(40) == rs.size() - 44;
        };
        if (!root_only(root.tail(4)) || !root_only(local))
            throw Error("Unexpected ray root state");
        c.word();
        auto count = c.word();
        if (!count || count > 256)
            throw Error("Bad ray library count");
        json r = {hex(Data(b).first(b.size() - c.rest.size())), json::array()};
        bool complete = true;
        for (unsigned i = 0; i < count; ++i) {
            auto before = c.rest;
            for (int n = 0; n < 3; ++n) {
                auto name = c.take(c.word());
                if (name.size() % 2 || name.size() > 1024)
                    throw Error("Bad ray export");
                for (size_t k = 0; k < name.size(); k += 2)
                    if (name.data[k + 1] || (!isalnum(name.data[k]) && name.data[k] != '_'))
                        throw Error("Unexpected ray symbol");
            }
            auto names = hex(before.data.first(before.size() - c.rest.size()));
            auto digest = sha(c.take(c.word()).data);
            if (!products.contains(digest)) {
                complete = false;
                continue;
            }
            r[1].push_back({names, products.at(digest)});
        }
        if (c.rest.size())
            throw Error("Trailing ray library bytes");
        if (complete)
            j["ray_pipelines"].push_back(r);
        else
            std::cerr << "No exact source recipe for ray pipeline " << path.stem() << '\n';
    }
    if (!variants || j["pipelines"].empty())
        throw Error("Empty workload");
    j["recorded_variants"] = variants;
    save_profile(argv[5], j);
    std::cout << variants << " recorded variants, " << found.size() << " package sources, "
              << j["pipelines"].size() << " pipelines + " << j["ray_pipelines"].size() << " ray pipelines, "
              << j["rays"].size() << " ray bindings, " << j["collections"].size() << " collections. "
              << wanted.size() - found.size() << " non-package sources excluded.\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
