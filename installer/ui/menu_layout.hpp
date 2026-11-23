#pragma once

// Logical pixels on the 1280 × 720 canvas. Shared with the Vulkan composite
// so the bar, its label and the edge of the menu transition stay in register.
namespace tb::layout {
inline constexpr float rail = 280;
inline constexpr float left = 352, right = 1208, width = right - left;
inline constexpr float footerTop = 600;
inline constexpr float barWidth = 512, barLeft = (1280 - barWidth) / 2;
inline constexpr float barTop = 614, barHeight = 3;
}
