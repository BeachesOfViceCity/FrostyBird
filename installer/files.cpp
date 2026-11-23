#include "core.hpp"
#include <llvm/Support/SHA256.h>
#include <llvm/ADT/ArrayRef.h>
#include <zlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <memory>

namespace fb {
std::string hex(Data b) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (u8 v : b) {
        s += digits[v >> 4];
        s += digits[v & 15];
    }
    return s;
}
Buffer unhex(std::string_view s) {
    auto digit = [](char c) -> unsigned {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return 10 + c - 'a';
        throw Error("invalid hex digit");
    };
    if (s.size() % 2)
        throw Error("odd hexadecimal length");
    Buffer b;
    for (size_t i = 0; i < s.size(); i += 2)
        b.push_back(digit(s[i]) * 16 + digit(s[i + 1]));
    return b;
}
std::string sha(Data d) { return hex(llvm::SHA256::hash({d.data(), d.size()})); }
Map::Map(const fs::path& p) {
    int fd = open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        throw Error("Cannot open " + p.string());
    struct stat st{};
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        close(fd);
        throw Error("Empty or invalid file: " + p.string());
    }
    size = st.st_size;
    auto m = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED)
        throw Error("Cannot map " + p.string());
    data = static_cast<const u8*>(m);
}
Map::~Map() { munmap(const_cast<u8*>(data), size); }
std::string file_sha(const fs::path& p) {
    if (!fs::file_size(p))
        return sha({});
    Map m(p);
    return sha(m.bytes().data);
}
Buffer read(const fs::path& p, size_t limit) {
    auto n = fs::file_size(p);
    if (n > limit)
        throw Error("File too large: " + p.string());
    Buffer b(n);
    std::ifstream f(p, std::ios::binary);
    if (!f || (n && !f.read(reinterpret_cast<char*>(b.data()), n)))
        throw Error("Cannot read " + p.string());
    return b;
}
std::string text(const fs::path& p, size_t limit) {
    auto b = read(p, limit);
    return {b.begin(), b.end()};
}
void write(const fs::path& p, Data b) {
    static std::atomic<unsigned> seq{};
    auto tmp = p;
    tmp += "." + std::to_string(getpid()) + "." + std::to_string(++seq) + ".tmp";
    try {
        std::ofstream f;
        f.exceptions(std::ios::failbit | std::ios::badbit);
        f.open(tmp, std::ios::binary);
        if (!b.empty())
            f.write(reinterpret_cast<const char*>(b.data()), b.size());
        f.close();
        fs::rename(tmp, p);
    } catch (...) {
        std::error_code ec;
        fs::remove(tmp, ec);
        throw;
    }
}
void write(const fs::path& p, std::string_view s) {
    write(p, Data{reinterpret_cast<const u8*>(s.data()), s.size()});
}
json load_profile(const fs::path& p) {
    std::unique_ptr<gzFile_s, decltype(&gzclose)> f(gzopen(p.c_str(), "rb"), gzclose);
    if (!f)
        throw Error("Missing shader workload: " + p.string());
    std::string s;
    std::array<char, 65536> b;
    int n;
    while ((n = gzread(f.get(), b.data(), b.size())) > 0) {
        s.append(b.data(), n);
        if (s.size() > 64u << 20)
            throw Error("Oversized shader workload");
    }
    if (n < 0)
        throw Error("Damaged shader workload");
    auto j = json::parse(s);
    if (j.at("version") != 1)
        throw Error("Unsupported shader workload");
    return j;
}
void save_profile(const fs::path& p, const json& j) {
    auto s = j.dump();
    auto tmp = p;
    tmp += ".tmp";
    auto f = gzopen(tmp.c_str(), "wb9");
    if (!f)
        throw Error("Cannot create shader workload");
    auto n = gzwrite(f, s.data(), s.size());
    auto rc = gzclose(f);
    if (n != int(s.size()) || rc != Z_OK)
        throw Error("Cannot save shader workload");
    fs::rename(tmp, p);
}
} // namespace fb
