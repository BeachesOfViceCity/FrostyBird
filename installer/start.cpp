#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

int main(int argc, char** argv) {
    char path[PATH_MAX];
    auto n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0 || size_t(n) == sizeof(path) - 1)
        return 1;
    path[n] = 0;
    std::string entry(path), base = entry.substr(0, entry.rfind('/'));
    std::string loader = base + "/libc/ld-linux-x86-64.so.2", core = base + "/libexec/frostybird-core";
    std::string libs = base + "/libc:" + base + "/lib";
    if (setenv("FROSTYBIRD_ENTRY", path, 1))
        return 1;
    char libarg[] = "--library-path", namearg[] = "--argv0";
    std::vector<char*> args{loader.data(), libarg, libs.data(), namearg, path, core.data()};
    for (int i = 1; i < argc; ++i)
        args.push_back(argv[i]);
    args.push_back(nullptr);
    execv(loader.c_str(), args.data());
    std::fprintf(stderr, "Frosty Bird: cannot open %s: %s. Extract the whole ZIP first.\n", loader.c_str(),
                 strerror(errno));
    return 1;
}
