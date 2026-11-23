#include "runtime.hpp"
#include <SDL3/SDL.h>
#include <iostream>
#include <unistd.h>

int main(int argc, char** argv) try {
    using namespace fb;
    auto root = current_binary().parent_path();
    auto home = Home::get();
    fs::path source;
    bool setup = false, preview = false, mute = false, batch = false, worker = false, noLaunch = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view s = argv[i];
        if (s == "--help" || s == "-h") {
            std::cout << "Frosty Bird · The Matrix Awakens\n  frostybird                   First-run setup, "
                         "then play\n  frostybird --setup [--source PACKAGE] [--mute]\n  frostybird "
                         "--preview         Read-only installer tour\n  frostybird --check PACKAGE   Check "
                         "original game files\n  frostybird --install PACKAGE [--no-launch]\n";
            return 0;
        }
        if (s == "--check" && i + 1 < argc) {
            auto g = verify(argv[++i]);
            std::cout << "Game package verified: " << g.mount << '\n';
            return 0;
        }
        if (s == "--source" || s == "--install" || s == "--worker") {
            if (++i >= argc)
                throw Error("Missing game folder");
            source = argv[i];
            setup = true;
            batch = s != "--source";
            worker = s == "--worker";
        } else if (s == "--setup")
            setup = true;
        else if (s == "--preview")
            preview = true;
        else if (s == "--mute")
            mute = true;
        else if (s == "--no-launch")
            noLaunch = true;
        else
            throw Error("Unknown option: " + std::string(s));
    }
    if (batch && preview)
        throw Error("Preview and installation are separate modes");
    if (setup && !batch && !preview && source.empty()) {
        auto previous = installed(home);
        if (!previous.is_null())
            source = previous.at("mount").get<std::string>();
    }
    auto state = (preview || batch || setup) ? json(nullptr) : installed(home);
    if (!state.is_null()) {
        source = state.at("mount").get<std::string>();
        try {
            verify(source);
        } catch (const std::exception&) {
            setup = true;
        }
    }
    if (batch) {
        int percent = -1;
        std::string previous;
        install(root, home, verify(source), [&](float v, std::string s) {
            int n = int(v * 1000);
            if (n != percent || (s != previous && v >= .25f)) {
                std::cout << n << '\t' << s << std::endl;
                percent = n;
                previous = std::move(s);
            }
        });
        if (worker || noLaunch)
            return 0;
    } else if (preview || setup || state.is_null()) {
        if (source.empty() && std::getenv("FROSTY_GAME_ROOT"))
            source = std::getenv("FROSTY_GAME_ROOT");
        if (!wizard(root, home, source, mute, preview, noLaunch))
            return 0;
    }
    auto ready = installed(home);
    if (ready.is_null())
        throw Error("Installation was not completed");
    auto exe = home.data / "releases" / ready.at("release").get<std::string>() / "frostybird";
    if (!fs::equivalent(exe, current_binary())) {
        execl(exe.c_str(), exe.c_str(), mute ? "--mute" : nullptr, nullptr);
        throw Error("Cannot start installed Frosty Bird");
    }
    return play(home, mute);
} catch (const std::exception& e) {
    std::cerr << "Frosty Bird: " << e.what() << std::endl;
    return 1;
}
