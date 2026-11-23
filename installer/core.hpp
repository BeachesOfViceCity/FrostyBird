#pragma once
#include "bytes.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <array>

namespace fb {
namespace fs = std::filesystem;
using namespace frosty;
using json = nlohmann::json;
using Data = std::span<const u8>;
using Progress = std::function<void(float, std::string)>;
inline constexpr auto title = "Frosty-XSX-Shipping.exe";
inline constexpr auto bindir = "Frosty/Binaries/XSX";
inline constexpr auto pakfile = "Frosty/Content/Paks/Frosty-XSX.pak";
std::string hex(Data);
Buffer unhex(std::string_view);
std::string sha(Data);
std::string file_sha(const fs::path&);
Buffer read(const fs::path&, size_t limit = 64u << 20);
std::string text(const fs::path&, size_t limit = 1u << 20);
void write(const fs::path&, Data);
void write(const fs::path&, std::string_view);
json load_profile(const fs::path&);
void save_profile(const fs::path&, const json&);
struct Map {
    const u8* data{};
    size_t size{};
    explicit Map(const fs::path&);
    ~Map();
    Map(const Map&) = delete;
    Bytes bytes() const { return {{data, size}}; }
};
struct Request {
    u32 wire;
    Buffer params;
};
void check_request(u32 wire, size_t size, size_t params);
class Cache {
    fs::path packets;
    std::array<std::mutex, 128> locks;

  public:
    explicit Cache(const fs::path&);
    Buffer resolve(Data raw, const Request&);
};
// Split XSP1 into binding/render state and original stage references. Ray
// pipeline libraries contain translated code and are not exported as blobs.
struct Plan {
    Buffer state;
    std::vector<Buffer> shaders;
};
Plan split_plan(Data);
Buffer stage_input(Bytes record, unsigned frequency);
void warm(const fs::path& pak, const json& profile, const fs::path& plans, Cache&, u32 caps, const Progress&);
} // namespace fb
