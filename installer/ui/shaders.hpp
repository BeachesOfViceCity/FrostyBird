#pragma once
#include <cstdint>
#include <vector>

namespace tb {
struct Shaders {
    std::vector<std::uint32_t> vertex, fragment;
};
}
