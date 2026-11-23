#include "runtime.hpp"
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/random.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace fb {
namespace {
bool io(int fd, void* ptr, size_t n, bool sendit) {
    auto p = static_cast<char*>(ptr);
    while (n) {
        ssize_t s = sendit ? send(fd, p, n, MSG_NOSIGNAL) : recv(fd, p, n, 0);
        if (s < 0 && errno == EINTR)
            continue;
        if (s <= 0)
            return false;
        p += s;
        n -= s;
    }
    return true;
}
struct Close {
    int fd;
    ~Close() {
        if (fd >= 0)
            close(fd);
    }
};
} // namespace
Service::Service(Cache& c, Env& env) : cache(c) {
    u8 bytes[16];
    if (getrandom(bytes, sizeof bytes, 0) != sizeof bytes)
        throw Error("Cannot create shader service token");
    token = hex(bytes);
    Close s{socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof addr;
    if (s.fd < 0 || bind(s.fd, reinterpret_cast<sockaddr*>(&addr), len) || listen(s.fd, 64) ||
        getsockname(s.fd, reinterpret_cast<sockaddr*>(&addr), &len))
        throw Error("Cannot start local shader service");
    env["FROSTY_COMPILER_PORT"] = std::to_string(ntohs(addr.sin_port));
    env["FROSTY_COMPILER_TOKEN"] = token;
    fd = s.fd;
    try {
        for (int i = 0; i < 6; ++i)
            pool.emplace_back([this] { serve(); });
    } catch (...) {
        stopping = true;
        pool.clear();
        throw;
    }
    s.fd = -1;
}
Service::~Service() {
    stopping = true;
    shutdown(fd, SHUT_RDWR);
    pool.clear();
    close(fd);
}
void Service::serve() {
    while (!stopping) {
        pollfd ready{fd, POLLIN, 0};
        if (poll(&ready, 1, 250) <= 0)
            continue;
        Close s{accept4(fd, nullptr, nullptr, SOCK_CLOEXEC)};
        if (s.fd < 0)
            continue;
        timeval tv{10, 0};
        setsockopt(s.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(s.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        char key[32];
        u32 h[3];
        if (!io(s.fd, key, 32, false) || std::string_view(key, 32) != token || !io(s.fd, h, 12, false))
            continue;
        try {
            check_request(h[0], h[1], h[2]);
            Request req{h[0], Buffer(h[2])};
            Buffer raw(h[1]);
            if (!io(s.fd, req.params.data(), h[2], false) || !io(s.fd, raw.data(), h[1], false))
                continue;
            auto packet = cache.resolve(raw, req);
            u32 ok = 0;
            if (io(s.fd, &ok, 4, true))
                io(s.fd, packet.data(), packet.size(), true);
        } catch (const std::exception& e) {
            std::string why = e.what();
            why.resize(std::min<size_t>(why.size(), 2048));
            u32 err[]{1, u32(why.size())};
            if (io(s.fd, err, 8, true))
                io(s.fd, why.data(), why.size(), true);
        }
    }
}
} // namespace fb
