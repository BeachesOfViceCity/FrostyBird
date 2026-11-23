#pragma once
#include "opening.hpp"
#include "shaders.hpp"
#include <SDL3/SDL.h>
#include <cairo.h>
#include <functional>
#include <utility>

namespace tb {
struct Frame {
    float time, installMix, progress, fade;
    bool menu;
    OpeningPose opening;
    float completionAge=-1.f; // seconds since Finish, or inactive
};
class Graphics {
public:
    Graphics() = default;
    Graphics(const Graphics &) = delete;
    Graphics &operator=(const Graphics &) = delete;
    ~Graphics();
    void open(const Shaders &shaders);
    // paint() uses a 1280 × 720 coordinate system. Lettering is re-rasterized
    // at drawable resolution on resize; untouched menus cost one GPU triangle.
    // False when the swapchain has no drawable (hidden or minimized).
    bool present(Frame frame, bool dirty, const std::function<void(cairo_t *)> &paint);
    std::pair<double,double> logicalPoint(float x, float y) const;
    SDL_Window *window() const { return window_; }
private:
    void resizeInk(int w, int h);
    SDL_Window *window_{};
    SDL_GPUDevice *gpu_{};
    bool claimed_{};
    SDL_GPUGraphicsPipeline *pipeline_{};
    SDL_GPUTexture *ink_{};
    SDL_GPUTransferBuffer *upload_{};
    SDL_GPUSampler *sampler_{};
    cairo_surface_t *surface_{};
    int inkWidth_{}, inkHeight_{};
};
}
