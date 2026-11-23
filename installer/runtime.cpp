#include "runtime.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <elf.h>
#include <fstream>
#include <format>
#include <iostream>
#include <set>
#include <spawn.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
namespace fb {
namespace {
fs::path setting(const char* key, const fs::path& fallback) {
    auto p = std::getenv(key);
    return p && fs::path(p).is_absolute() ? fs::path(p) : fallback;
}
struct Lock {
    int fd;
    explicit Lock(const fs::path& p) : fd(open(p.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600)) {
        if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB)) {
            if (fd >= 0)
                close(fd);
            throw Error("Another Frosty Bird session is active, or its directory is not writable");
        }
    }
    ~Lock() { close(fd); }
};
std::string windows(fs::path p) {
    auto s = fs::absolute(p).string();
    std::ranges::replace(s, '/', '\\');
    return "Z:" + s;
}
bool safeid(std::string_view s) {
    return s.size() == 64 && s.find_first_not_of("0123456789abcdef") == s.npos;
}
fs::path checked(const fs::path& p) {
    if (!fs::is_regular_file(p) || !fs::file_size(p))
        throw Error("Missing file: " + p.string());
    return p;
}
void link_to(const fs::path& target, const fs::path& at) {
    if (fs::is_symlink(at) && fs::read_symlink(at) == target)
        return;
    if (fs::symlink_status(at).type() != fs::file_type::not_found)
        throw Error("Existing path points elsewhere: " + at.string());
    fs::create_symlink(target, at);
}
// ELF interpreters cannot contain $ORIGIN. The release builder reserves this
// one field, so we can place Wine anywhere (including a path with spaces).
// Its preloader and every child then use the same libc as the first process.
void relocate_wine(const fs::path& root) {
    auto loader = checked(root / "libc/ld-linux-x86-64.so.2").string();
    for (auto rel : {"wine/bin/wine", "wine/bin/wineserver", "wine/lib/wine/x86_64-unix/wine"}) {
        auto dest = root / rel;
        auto b = read(dest.string() + ".template");
        Bytes image{b};
        if (!image.tag(0, std::string_view(ELFMAG, SELFMAG)) || image.at<u8>(EI_CLASS) != ELFCLASS64 ||
            image.at<u8>(EI_DATA) != ELFDATA2LSB || image.at<uint16_t>(18) != EM_X86_64 ||
            image.at<uint16_t>(54) != sizeof(Elf64_Phdr))
            throw Error("Invalid Wine ELF template");
        auto phoff = image.at<u64>(32);
        bool relocated = false;
        for (unsigned i = 0; i < image.at<uint16_t>(56); ++i) {
            auto ph = image.slice(phoff + i * sizeof(Elf64_Phdr), sizeof(Elf64_Phdr));
            if (ph.at<u32>(0) != PT_INTERP)
                continue;
            auto at = ph.at<u64>(8), size = ph.at<u64>(32);
            auto slot = image.slice(at, size);
            if (relocated || !slot.tag(0, "/FROSTYBIRD/") || loader.size() + 1 > size)
                throw Error("Invalid loader slot or install path too long");
            std::fill_n(b.data() + at, size, u8{});
            std::copy(loader.begin(), loader.end(), b.data() + at);
            relocated = true;
        }
        if (!relocated)
            throw Error("Missing loader slot in " + dest.string());
        auto temp = dest;
        temp += ".relocated";
        write(temp, b);
        fs::permissions(temp, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                                  fs::perms::others_read | fs::perms::others_exec);
        fs::rename(temp, dest);
    }
}
void siblings(const fs::path& src, const fs::path& dst, std::set<std::string> skip) {
    for (auto& e : fs::directory_iterator(src)) {
        auto n = e.path().filename().string(), k = n;
        std::ranges::transform(k, k.begin(), [](unsigned char c) { return std::tolower(c); });
        if (!skip.contains(k))
            link_to(e.path(), dst / n);
    }
}
struct BakeAttempt {
    unsigned next{}, total{}, built{}, variants{}, rays{}, skipped{};
    std::string active;
    bool timed_out{}, committed{};

    bool consume(const std::string& line) {
        if (total && next == total &&
            (line == "frostybird/pso-cache-committed" || line == "frostybird/pso-cache-committed\r")) {
            committed = true;
            return true;
        }
        unsigned at, count, n[4]{};
        if (std::sscanf(line.c_str(), "frostybird/pso-progress: %u %u %u %u %u %u", &at, &count, &n[0], &n[1],
                        &n[2], &n[3]) == 6 &&
            count && (!total || count == total) && at >= next && at <= count) {
            bool advanced = at > next || !total;
            next = at;
            total = count;
            active.clear();
            built = n[0];
            variants = n[1];
            rays = n[2];
            skipped = n[3];
            return advanced;
        }
        char key[65]{};
        if (std::sscanf(line.c_str(), "frostybird/pso-begin: %u %64[0-9a-f]", &at, key) == 2 && at == next &&
            at < total && safeid(key)) {
            active = key;
            return true;
        }
        return false;
    }
};

int run(const fs::path& exe, std::vector<std::string> args, const Env& env, const fs::path& cwd,
        const fs::path& log, const Progress& progress = {}, BakeAttempt* bake = nullptr) {
    args.insert(args.begin(), exe.string());
    std::vector<char*> av, ep;
    std::vector<std::string> vars;
    for (auto& a : args)
        av.push_back(a.data());
    av.push_back(nullptr);
    for (auto& [k, v] : env)
        vars.push_back(k + "=" + v);
    for (auto& s : vars)
        ep.push_back(s.data());
    ep.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions))
        throw Error("Cannot initialize process");
    struct Cleanup {
        posix_spawn_file_actions_t& a;
        ~Cleanup() { posix_spawn_file_actions_destroy(&a); }
    } cleanup{actions};
    if (posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str()) ||
        posix_spawn_file_actions_addopen(&actions, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600) ||
        posix_spawn_file_actions_adddup2(&actions, 1, 2))
        throw Error("Cannot prepare child process");
    pid_t pid{};
    auto err = posix_spawn(&pid, exe.c_str(), &actions, nullptr, av.data(), ep.data());
    if (err)
        throw Error("Cannot start " + exe.string() + ": " + strerror(err));
    int status;
    std::ifstream stream;
    using Clock = std::chrono::steady_clock;
    auto last_activity = Clock::now();
    constexpr auto bake_idle_limit = std::chrono::seconds(90);
    constexpr auto bake_exit_grace = std::chrono::seconds(15);
    for (;;) {
        int rc = waitpid(pid, &status, bake ? WNOHANG : 0);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            throw Error("Cannot wait for Wine");
        }
        if (bake) {
            if (!stream.is_open())
                stream.open(log);
            stream.clear();
            auto previous = bake->next;
            const bool was_committed = bake->committed;
            std::string line;
            for (;;) {
                auto offset = stream.tellg();
                if (!std::getline(stream, line))
                    break;
                if (stream.eof() && rc != pid) {
                    stream.clear();
                    stream.seekg(offset);
                    break;
                }
                if (bake->consume(line))
                    last_activity = Clock::now();
            }
            if (!was_committed && bake->committed)
                progress(.985f, "Vulkan pipeline cache saved");
            else if (bake->total && !bake->committed && (bake->next != previous || rc == pid))
                progress(.84f + .14f * float(bake->next) / bake->total,
                         std::format("{} / {} pipeline recipes", bake->next, bake->total));
            if (rc != pid &&
                Clock::now() - last_activity >= (bake->committed ? bake_exit_grace : bake_idle_limit)) {
                // The child is only the standalone compiler. Do not kill Wine's
                // server, the installer, or a user's running title to abort it.
                kill(pid, SIGKILL);
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
                }
                if (bake->committed) {
                    std::ofstream notice(log, std::ios::app);
                    notice << "Frosty Bird: cache was committed; ended the unresponsive compiler teardown.\n";
                    return 0;
                }
                bake->timed_out = true;
                return 124;
            }
        }
        if (rc == pid)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
std::string last_line(const fs::path& log, std::string_view prefix, std::string_view contains = {}) {
    std::ifstream f(log);
    std::string s, result;
    while (std::getline(f, s)) {
        if (s.ends_with('\r'))
            s.pop_back();
        if (s.starts_with(prefix) && s.contains(contains))
            result = s;
    }
    return result;
}
struct Layout {
    fs::path release, base, prefix, view, bin, cache, logs;
    Home home;
    Game game;
    Layout(const Home& h, const Game& g, std::string id)
        : release(h.data / "releases" / id), home(h), game(g) {
        std::string path = g.mount.string();
        auto key = sha({reinterpret_cast<const u8*>(path.data()), path.size()});
        base = h.data / "instances" / id / key;
        prefix = base / "prefix";
        view = base / "Mount";
        bin = view / bindir;
        cache = h.cache / id;
        logs = base / "logs";
    }
    Env environment() {
        Env e;
        for (char** s = environ; *s; ++s) {
            std::string v = *s;
            auto n = v.find('=');
            auto k = v.substr(0, n);
            if (n != v.npos && !k.starts_with("FROSTY_") && !k.starts_with("XS_DEBUG_") &&
                k != "VKD3D_SHADER_DUMP_PATH")
                e[k] = v.substr(n + 1);
        }
        auto lib = (release / "wine-libs").string();
        if (e.contains("LD_LIBRARY_PATH") && !e["LD_LIBRARY_PATH"].empty())
            lib += ":" + e["LD_LIBRARY_PATH"];
        e["LD_LIBRARY_PATH"] = lib;
        e["WINEPREFIX"] = prefix.string();
        e["WINESERVER"] = (release / "wine/bin/wineserver").string();
        e.erase("WINEDLLPATH");
        e.erase("WINELOADER");
        e.erase("WINEARCH");
        e["WINEDLLOVERRIDES"] =
            "d3d12,d3d12core,d3d12_xs,dxgi,gameinput=n;winemenubuilder.exe,mscoree,mshtml=";
        e["WINEDEBUG"] = "-all";
        e["FROSTY_PRESENT"] = "dxgi";
        e["FROSTY_ASYNC_HTTP_HEADERS"] = "1";
        e["FROSTY_SPIRV_STAGES"] = "1";
        std::vector<std::string> amd;
        std::error_code ec;
        for (fs::directory_iterator it("/sys/class/drm", ec), end; !ec && it != end; it.increment(ec)) {
            auto n = it->path().filename().string();
            if (!n.starts_with("card") || n.size() < 5 || n.find_first_not_of("0123456789", 4) != n.npos)
                continue;
            std::ifstream vendor(it->path() / "device/vendor"), dev(it->path() / "device/device");
            std::string a, b;
            if (vendor >> a && dev >> b && a == "0x1002")
                amd.push_back(b);
        }
        if (amd == std::vector<std::string>{"0x73a5"}) {
            e.try_emplace("RADV_PERFTEST", "pswave32,gewave32");
            e["FROSTY_NATIVE_GROUP_ORDINAL"] = "1";
            e.try_emplace("VKD3D_CONFIG", "retain_psos,no_upload_hvv");
        }
        e.try_emplace("VKD3D_CONFIG", "retain_psos");
        auto paths = std::map<std::string, fs::path>{
            {"FROSTY_SPIRV_DIR", cache / "spirv"},
            {"VKD3D_SHADER_CACHE_PATH", cache / "vkd3d"},
            {"FROSTY_TILE_PLANS", cache / ("tiles-" + file_sha(game.system / "xg_xs.dll"))},
            {"FROSTY_PIPELINE_RECIPES", cache / "plans"},
            {"FROSTY_RT_DEMAND_PATH", cache / "rays"},
            {"FROSTY_RT_LIBRARY_PATH", cache / "collections"}};
        for (auto& [k, p] : paths) {
            fs::create_directories(p);
            e[k] = windows(p);
        }
        fs::create_directories(logs);
        e["DXVK_LOG_PATH"] = logs.string();
        e["GST_PLUGIN_SYSTEM_PATH_1_0"] = (release / "gst").string();
        e["GST_REGISTRY_1_0"] = (cache / "gst-registry.bin").string();
        e["GST_REGISTRY_FORK"] = "no";
        auto id = home.data / "player/local-player.id";
        if (fs::exists(id))
            e["FROSTY_LOCAL_ID"] = text(id, 32);
        return e;
    }
    int wine(std::vector<std::string> args, const Env& env, const fs::path& cwd, const char* which) {
        return run(release / "wine/bin/wine", std::move(args), env, cwd, logs / which);
    }
    void require_wine(std::vector<std::string> args, const Env& env, const fs::path& cwd, const char* which) {
        if (wine(std::move(args), env, cwd, which))
            throw Error("Wine setup failed. See " + (logs / which).string());
    }
    void view_setup() {
        auto player = home.data / "player";
        fs::create_directories(player / "Temp");
        fs::create_directories(player / "Saved");
        if (!fs::exists(player / "local-player.id")) {
            u64 id;
            if (getrandom(&id, 8, 0) != 8)
                throw Error("Cannot allocate player ID");
            write(player / "local-player.id",
                  std::format("{:016x}", 0xe000000000000000ULL | (id & 0xffffffffffffULL)));
        }
        fs::create_directories(bin);
        siblings(game.mount, view, {"frosty"});
        auto src = game.mount / "Frosty", dst = view / "Frosty";
        siblings(src, dst, {"binaries", "saved", "config"});
        link_to(player / "Saved", dst / "Saved");
        fs::create_directories(dst / "Config");
        if (fs::is_directory(src / "Config"))
            siblings(src / "Config", dst / "Config", {"userengine.ini"});
        auto ini = src / "Config/UserEngine.ini";
        write(dst / "Config/UserEngine.ini",
              (fs::is_regular_file(ini) ? text(ini) : "") + "\n" + text(release / "profile/UserEngine.ini"));
        siblings(src / "Binaries", dst / "Binaries", {"xsx"});
        std::map<std::string, fs::path> libs;
        for (auto n : {"d3d12.dll", "d3d12core.dll", "dxgi.dll", "d3d12_xs.dll", "AcpHal.dll",
                       "avcodec-62.dll", "avutil-60.dll", "xcrdapi.dll", "xmem.dll"})
            libs[n] = release / "guest" / n;
        libs["GameInput.dll"] = release / "guest/frosty-gameinput.dll";
        for (auto n : {"dxcompiler_xs.dll", "xg_xs.dll"})
            libs[n] = game.system / n;
        std::set<std::string> skip;
        for (auto& [n, p] : libs) {
            auto lower = n;
            std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return std::tolower(c); });
            skip.insert(lower);
        }
        siblings(game.mount / bindir, bin, skip);
        for (auto& [n, p] : libs)
            link_to(checked(p), bin / n);
    }
};
struct ShaderPaths {
    fs::path packets, hlsl;
};
ShaderPaths shader_paths(const Layout& l, bool import_legacy = false) {
    auto compiler = text(l.release / "profile/translator.sha256", 128);
    if (compiler.ends_with('\n'))
        compiler.pop_back();
    if (!safeid(compiler))
        throw Error("Invalid shader compiler fingerprint");
    auto library_id = [](const fs::path& release) {
        return file_sha(release / "lib/libdxcompiler.so") + file_sha(release / "lib/libdxil.so");
    };
    auto dxc = library_id(l.release);
    auto hlsl_id = sha({reinterpret_cast<const u8*>(dxc.data()), dxc.size()});
    auto hlsl = l.home.cache / "hlsl-v1" / hlsl_id;
    fs::create_directories(hlsl);
    if (import_legacy && fs::is_empty(hlsl)) {
        for (const auto& old : fs::directory_iterator(l.home.data / "releases")) {
            auto id = old.path().filename().string();
            if (!safeid(id) || old.path() == l.release)
                continue;
            auto path = l.home.cache / id / "hlsl";
            if (!fs::is_directory(path) || !fs::is_regular_file(old.path() / "lib/libdxcompiler.so") ||
                !fs::is_regular_file(old.path() / "lib/libdxil.so") || library_id(old.path()) != dxc)
                continue;
            // DXC-cache hits still verify the complete generated HLSL and
            // argument hash, plus the cached DXIL's own digest. No packet from
            // an older/unidentified translator is ever trusted directly.
            for (const auto& entry : fs::directory_iterator(path)) {
                auto name = entry.path().filename().string();
                if (!safeid(name) || !fs::is_regular_file(entry.symlink_status()))
                    continue;
                auto dest = hlsl / name;
                std::error_code ec;
                fs::create_hard_link(entry.path(), dest, ec);
                if (ec && ec != std::errc::file_exists)
                    fs::copy_file(entry.path(), dest, fs::copy_options::skip_existing);
            }
        }
    }
    return {l.home.cache / "compiler-v1" / compiler, hlsl};
}

std::string bake_pipelines(Layout& l, const fs::path& bake, Env& env, const Progress& progress) {
    unsigned next = 0, retry_at = ~0u, built = 0, variants = 0, rays = 0, unavailable = 0;
    std::vector<std::string> deferred;
    env["FROSTY_INSTALL_PROGRESS"] = "1";
    env["VKD3D_SHADER_CACHE_WRITE_SYNC"] = "1";
    for (unsigned attempt = 0;; ++attempt) {
        auto log = l.logs / (attempt ? std::format("pipelines-retry-{}.log", attempt) : "pipelines.log");
        BakeAttempt jobs;
        jobs.next = next;
        int status =
            run(l.release / "wine/bin/wine", {(bake / title).string(), "--prepare", std::to_string(next)},
                env, bake, log, progress, &jobs);
        built += jobs.built;
        variants += jobs.variants;
        rays += jobs.rays;
        unavailable += jobs.skipped;
        if (status == 0 && jobs.committed)
            break;
        if (!jobs.timed_out || jobs.active.empty() || deferred.size() == 4) {
            auto reason = jobs.timed_out ? "Vulkan pipeline preparation stopped responding. See "
                                         : "Vulkan pipeline preparation failed. See ";
            throw Error(reason + log.string());
        }
        next = jobs.next;
        if (retry_at == next) {
            // Unexpected driver hangs are isolated to the exact portable PSO.
            // Keep its recipe for the normal demand path; never report it as baked.
            deferred.push_back(jobs.active);
            ++next;
            progress(.84f + .14f * float(next) / jobs.total, "Continuing with remaining pipelines");
        } else {
            retry_at = next;
            progress(.84f + .14f * float(next) / jobs.total, "Retrying an unresponsive Vulkan pipeline");
        }
    }
    if (!built && !rays)
        throw Error("No native pipelines were prepared. See " + (l.logs / "pipelines.log").string());
    auto summary =
        std::format("{} native pipelines, {} bound variants, {} ray pipelines ({} unavailable, {} deferred)",
                    built, variants, rays, unavailable, deferred.size());
    std::string report = summary + "\n", demand_only;
    for (auto& key : deferred) {
        report += "Deferred " + key + "\n";
        demand_only += key + "\n";
    }
    write(l.cache / "plans/deferred-v1.txt", demand_only);
    write(l.logs / "pipeline-result.txt", report);
    return summary;
}
void shortcuts(const Home& h, const fs::path& release) {
    fs::create_directories(h.bin);
    auto link = h.bin / "frostybird";
    auto dest = release / "frostybird";
    if (fs::symlink_status(link).type() != fs::file_type::not_found &&
        (!fs::is_symlink(link) || fs::read_symlink(link).parent_path().parent_path() != h.data / "releases"))
        throw Error("Cannot replace an unrelated executable: " + link.string());
    auto temp = h.bin / (".frostybird-" + std::to_string(getpid()));
    fs::create_symlink(dest, temp);
    fs::rename(temp, link);
    fs::create_directories(h.apps);
    std::string quoted;
    for (char c : link.string()) {
        if (c == '%')
            quoted += "%%";
        else {
            if (c == '"' || c == '\\' || c == '`' || c == '$')
                quoted += '\\';
            quoted += c;
        }
    }
    write(h.apps / "frostybird.desktop",
          "[Desktop Entry]\nType=Application\nName=Frosty Bird — The Matrix Awakens\nExec=\"" + quoted +
              "\"\nIcon=" + (release / "assets/frostybird.png").string() +
              "\nTerminal=false\nCategories=Game;\n");
}
} // namespace
Home Home::get() {
    auto h = std::getenv("HOME");
    if (!h || !*h)
        throw Error("HOME is not set");
    fs::path home = h;
    auto data = setting("XDG_DATA_HOME", home / ".local/share");
    return {data / "frostybird", setting("XDG_CACHE_HOME", home / ".cache") / "frostybird",
            setting("XDG_BIN_HOME", home / ".local/bin"), data / "applications"};
}
fs::path current_binary() {
    if (auto p = std::getenv("FROSTYBIRD_ENTRY"); p && *p)
        return fs::canonical(p);
    return fs::canonical("/proc/self/exe");
}
Game verify(const fs::path& selected) {
    if (selected.empty())
        throw Error("Choose the folder containing Mount and EmbeddedXvd, or Mount itself.");
    auto m = fs::canonical(selected);
    if (fs::is_directory(m / "Mount"))
        m /= "Mount";
    auto sys = m.parent_path() / "EmbeddedXvd/Windows/System32";
    auto exe = checked(m / bindir / title);
    Map image(exe);
    auto pe = image.bytes().at<u32>(60);
    if (!image.bytes().tag(0, "MZ") || !image.bytes().tag(pe, std::string_view("PE\0\0", 4)) ||
        image.bytes().at<uint16_t>(pe + 4) != 0x8664)
        throw Error("The shipping executable is not an x64 Xbox PE.");
    for (auto n : {"dxcompiler_xs.dll", "xg_xs.dll", "mfhevcdec.dll"})
        checked(sys / n);
    for (auto n : {"Frosty-XSX.ucas", "Frosty-XSX.utoc", "global.ucas", "global.utoc"})
        checked(m / "Frosty/Content/Paks" / n);
    return {m, sys, checked(m / pakfile)};
}
std::string storage_summary(const fs::path& bundle, const Home& h) {
    auto near = [](fs::path p) {
        while (!fs::exists(p) && p.has_relative_path())
            p = p.parent_path();
        return p;
    };
    auto data = near(h.data), cache = near(h.cache);
    struct stat a{}, b{};
    if (stat(data.c_str(), &a) || stat(cache.c_str(), &b))
        throw Error("Cannot inspect install destination");
    auto manifest = bundle / "manifest.sha256";
    std::ifstream in(checked(manifest));
    std::string row;
    uintmax_t bytes = 0;
    while (std::getline(in, row)) {
        if (row.size() < 67)
            throw Error("Damaged release manifest");
        bytes += fs::file_size(bundle / row.substr(66));
    }
    bool present = fs::is_regular_file(h.data / "releases" / file_sha(manifest) / "manifest.sha256");
    uintmax_t need = (present ? 0 : bytes) + (2ull << 30), free = fs::space(data).available;
    if (a.st_dev == b.st_dev)
        need += 2ull << 30;
    if (free < need || fs::space(cache).available < (2ull << 30))
        throw Error("Insufficient disk space for the runtime, prefix and shader cache.");
    return std::format("{:.1f} GiB runtime   ·   {:.1f} GiB available", bytes / 1073741824.,
                       free / 1073741824.);
}
json installed(const Home& h) {
    auto p = h.data / "installed.json";
    if (!fs::is_regular_file(p))
        return nullptr;
    auto j = json::parse(text(p));
    if (j.at("version") != 1 || !safeid(j.at("release").get<std::string>()))
        throw Error("Invalid installation record");
    return j;
}
void install(const fs::path& bundle, const Home& home, const Game& g, const Progress& progress) {
    fs::create_directories(home.data);
    Lock lock(home.data / "session.lock");
    auto manifest = bundle / "manifest.sha256";
    auto id = file_sha(manifest);
    Layout l(home, g, id);
    fs::create_directories(l.release.parent_path());
    auto stage = l.release;
    stage += ".partial";
    bool existing = fs::is_regular_file(l.release / "manifest.sha256");
    if (!existing)
        fs::create_directories(stage);
    std::ifstream list(manifest);
    std::string line;
    std::vector<std::pair<std::string, fs::path>> files;
    uintmax_t total = 0, done = 0;
    while (std::getline(list, line)) {
        if (line.size() < 67 || line.substr(64, 2) != "  " || !safeid(line.substr(0, 64)))
            throw Error("Damaged release manifest");
        fs::path p = line.substr(66);
        if (p.empty() || p.is_absolute() || p.lexically_normal() != p || *p.begin() == ".." ||
            !fs::is_regular_file(fs::symlink_status(bundle / p)))
            throw Error("Invalid release path");
        total += fs::file_size(bundle / p);
        files.emplace_back(line.substr(0, 64), p);
    }
    if (files.empty())
        throw Error("Empty release");
    fs::create_directories(home.cache);
    struct stat data_fs{}, cache_fs{};
    if (stat(home.data.c_str(), &data_fs) || stat(home.cache.c_str(), &cache_fs))
        throw Error("Cannot inspect destination filesystems");
    constexpr uintmax_t reserve = 2ull << 30;
    if (fs::space(home.data).available <
        (existing ? 0 : total) + reserve + (data_fs.st_dev == cache_fs.st_dev ? reserve : 0))
        throw Error("Not enough room for the runtime and Wine prefix.");
    if (fs::space(home.cache).available < reserve)
        throw Error("At least 2 GiB is needed for shader caches.");
    for (auto& [hash, p] : files) {
        auto dst = (existing ? l.release : stage) / p;
        if (!existing) {
            fs::create_directories(dst.parent_path());
            if (fs::weakly_canonical(dst) != fs::weakly_canonical(stage) / p)
                throw Error("Unexpected link in partial installation: " + p.string());
            fs::copy_file(bundle / p, dst, fs::copy_options::overwrite_existing);
        }
        if (file_sha(dst) != hash)
            throw Error("Release checksum mismatch: " + p.string());
        done += fs::file_size(dst);
        progress(.04f + .14f * float(done) / total, "Verifying and installing runtime files");
    }
    if (!existing) {
        fs::copy_file(manifest, stage / "manifest.sha256", fs::copy_options::overwrite_existing);
        fs::rename(stage, l.release);
    }
    relocate_wine(l.release);
    progress(.19f, "Creating the Wine prefix");
    l.view_setup();
    auto env = l.environment();
    fs::create_directories(l.prefix);
    l.require_wine({"wineboot", "-i"}, env, l.base, "wineboot.log");
    link_to(l.view, l.prefix / "dosdevices/g:");
    link_to(home.data / "player/Temp", l.prefix / "dosdevices/t:");
    for (auto [n, src] : {std::pair{"xgameruntime.dll", "frosty-local-grt.dll"},
                          {"frosty_grt_core.dll", "frosty_grt_core.dll"}})
        link_to(l.release / "guest" / src, l.prefix / "drive_c/windows/system32" / n);
    progress(.22f, "Registering the movie decoder");
    auto guest = l.release / "guest";
    l.require_wine({(guest / "frosty-media.exe").string(), windows(g.system / "mfhevcdec.dll"), "--register",
                    windows(guest / "frosty-video.dll")},
                   env, guest, "media.log");
    auto bake = l.base / "pipeline-baker";
    fs::create_directories(bake);
    link_to(guest / "frosty-pipeline-check.exe", bake / title);
    for (auto n : {"d3d12.dll", "d3d12core.dll", "dxgi.dll"})
        link_to(guest / n, bake / n);
    progress(.25f, "Checking your graphics adapter");
    l.require_wine({(bake / title).string(), "--caps"}, env, bake, "adapter.log");
    auto caps = last_line(l.logs / "adapter.log", "frostybird-compute-caps=");
    if (caps.empty())
        throw Error("Adapter did not return shader capabilities");
    auto mask = std::stoul(caps.substr(caps.find('=') + 1));
    if (mask > 15)
        throw Error("Invalid GPU capabilities");
    auto shaders = shader_paths(l, true);
    if (setenv("FROSTY_DXIL_SOURCE_CACHE", shaders.hlsl.c_str(), 1))
        throw Error("Cannot configure the DXC cache");
    Cache cache(shaders.packets);
    auto profile = load_profile(l.release / "assets/workload.json.gz");
    warm(g.pak, profile, l.cache / "plans", cache, u32(mask),
         [&](float p, std::string s) { progress(.26f + .57f * p, std::move(s)); });
    fs::copy_file(l.release / "profile/dispatch.contract", l.cache / "rays/dispatch.contract",
                  fs::copy_options::overwrite_existing);
    progress(.84f, "Building Vulkan pipelines for your driver");
    std::string report;
    {
        Service service(cache, env);
        report = bake_pipelines(l, bake, env, progress);
    }
    progress(.99f, "Creating the application shortcut");
    shortcuts(home, l.release);
    write(home.data / "installed.json", json{{"version", 1},
                                             {"release", id},
                                             {"mount", g.mount.string()},
                                             {"pipeline_result", report},
                                             {"compute_caps", mask}}
                                            .dump(2));
    progress(1.f, "Ready to play");
}
int play(const Home& home, bool mute) {
    Lock lock(home.data / "session.lock");
    auto j = installed(home);
    Layout l(home, verify(j.at("mount").get<std::string>()), j.at("release").get<std::string>());
    auto env = l.environment();
    auto shaders = shader_paths(l);
    if (setenv("FROSTY_DXIL_SOURCE_CACHE", shaders.hlsl.c_str(), 1))
        throw Error("Cannot configure the DXC cache");
    Cache cache(shaders.packets);
    Service service(cache, env);
    std::vector<std::string> args{(l.bin / title).string(), "-NoLoadingScreen"};
    if (mute)
        args.push_back("-nosound");
    return l.wine(std::move(args), env, l.bin, "play.log");
}
} // namespace fb
