#pragma once
#include "core.hpp"
#include <thread>
#include <map>

namespace fb {
using Env = std::map<std::string, std::string>;
class Service {
    Cache& cache;
    std::string token;
    int fd = -1;
    std::atomic<bool> stopping{};
    std::vector<std::jthread> pool;
    void serve();

  public:
    Service(Cache&, Env&);
    ~Service();
};
struct Home {
    fs::path data, cache, bin, apps;
    static Home get();
};
struct Game {
    fs::path mount, system, pak;
};
Game verify(const fs::path&);
std::string storage_summary(const fs::path& bundle, const Home&);
json installed(const Home&);
fs::path current_binary();
void install(const fs::path& bundle, const Home&, const Game&, const Progress&);
int play(const Home&, bool mute);
bool wizard(const fs::path& bundle, const Home&, fs::path source, bool mute, bool preview, bool noLaunch);
} // namespace fb
