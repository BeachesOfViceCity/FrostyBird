#include "graphics.hpp"
#include "menu_layout.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace tb {
namespace {
void require(bool ok, const char *what) {
    if (!ok) throw std::runtime_error(std::string(what) + ": " + SDL_GetError());
}
SDL_GPUShader *shader(SDL_GPUDevice *gpu, const std::vector<std::uint32_t> &code, bool frag) {
    SDL_GPUShaderCreateInfo info{};
    info.code = reinterpret_cast<const Uint8 *>(code.data());
    info.code_size = code.size()*sizeof(code[0]);
    info.entrypoint = "main";
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    info.stage = frag ? SDL_GPU_SHADERSTAGE_FRAGMENT : SDL_GPU_SHADERSTAGE_VERTEX;
    info.num_samplers = frag;
    info.num_uniform_buffers = frag;
    return SDL_CreateGPUShader(gpu,&info);
}
}

void Graphics::open(const Shaders &shaders) {
    SDL_Rect usable{0,0,1280,720};
    SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable);
    float s = std::clamp(std::min(usable.w/1400.f, usable.h/830.f), .50f, 1.25f);
    window_ = SDL_CreateWindow("Frosty Bird - The Matrix Awakens", int(1280*s), int(720*s),
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    require(window_, "Create window");
    SDL_SetWindowMinimumSize(window_, 640, 360);
    gpu_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV,
#ifdef NDEBUG
        false,
#else
        true,
#endif
        "vulkan");
    require(gpu_, "Create Vulkan device");
    claimed_ = SDL_ClaimWindowForGPUDevice(gpu_,window_);
    require(claimed_, "Create Vulkan swapchain");
    SDL_Log("Frosty Bird setup: SDL_GPU / %s", SDL_GetGPUDeviceDriver(gpu_));
    // The shader composites display-referred Cairo colors, so request SDR UNORM.
    require(SDL_SetGPUSwapchainParameters(gpu_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
            SDL_GPU_PRESENTMODE_VSYNC), "Configure Vulkan swapchain");
    auto *v = shader(gpu_, shaders.vertex, false);
    require(v, "Create vertex shader");
    auto *f = shader(gpu_, shaders.fragment, true);
    if (!f) { SDL_ReleaseGPUShader(gpu_,v); require(false, "Create fragment shader"); }
    SDL_GPUColorTargetDescription target{};
    target.format = SDL_GetGPUSwapchainTextureFormat(gpu_,window_);
    SDL_GPUGraphicsPipelineCreateInfo desc{};
    desc.vertex_shader = v;
    desc.fragment_shader = f;
    desc.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    desc.target_info.color_target_descriptions = &target;
    desc.target_info.num_color_targets = 1;
    pipeline_ = SDL_CreateGPUGraphicsPipeline(gpu_,&desc);
    SDL_ReleaseGPUShader(gpu_,v);
    SDL_ReleaseGPUShader(gpu_,f);
    require(pipeline_, "Create menu pipeline");
    SDL_GPUSamplerCreateInfo si{};
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = SDL_CreateGPUSampler(gpu_,&si);
    require(sampler_, "Create menu sampler");
}

void Graphics::resizeInk(int w, int h) {
    if (surface_) cairo_surface_destroy(surface_);
    if (ink_) SDL_ReleaseGPUTexture(gpu_,ink_);
    if (upload_) SDL_ReleaseGPUTransferBuffer(gpu_,upload_);
    ink_ = nullptr; upload_ = nullptr;
    surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,w,h);
    if (cairo_surface_status(surface_) != CAIRO_STATUS_SUCCESS)
        throw std::runtime_error(cairo_status_to_string(cairo_surface_status(surface_)));
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM; // native Cairo ARGB32 on x86-64
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = w; ti.height = h; ti.layer_count_or_depth = 1; ti.num_levels = 1;
    ink_ = SDL_CreateGPUTexture(gpu_,&ti);
    require(ink_, "Create menu texture");
    SDL_GPUTransferBufferCreateInfo bi{};
    bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    bi.size = h*cairo_image_surface_get_stride(surface_);
    upload_ = SDL_CreateGPUTransferBuffer(gpu_,&bi);
    require(upload_, "Create menu upload buffer");
    inkWidth_=w; inkHeight_=h;
}

std::pair<double,double> Graphics::logicalPoint(float x, float y) const {
    int w=0,h=0;
    SDL_GetWindowSize(window_, &w,&h);
    double scale=std::min(w/1280.,h/720.);
    if (scale<=0) return {-1.,-1.};
    return {(x-(w-1280*scale)/2)/scale, (y-(h-720*scale)/2)/scale};
}

bool Graphics::present(Frame frame, bool dirty, const std::function<void(cairo_t *)> &paint) {
    auto *cmd=SDL_AcquireGPUCommandBuffer(gpu_);
    require(cmd, "Acquire command buffer");
    SDL_GPUTexture *swap{};
    Uint32 w=0,h=0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd,window_,&swap,&w,&h)) {
        SDL_CancelGPUCommandBuffer(cmd);
        require(false, "Acquire Vulkan image");
    }
    if (!swap || !w || !h) {
        require(SDL_SubmitGPUCommandBuffer(cmd), "Submit empty frame");
        return false;
    }
    float scale=std::min(w/1280.f,h/720.f);
    int units=std::clamp(int(std::ceil(scale*80)), 1, 160);
    if (inkWidth_!=16*units || inkHeight_!=9*units) {
        resizeInk(16*units,9*units);
        dirty=true;
    }
    if (dirty) {
        auto *cr=cairo_create(surface_);
        cairo_set_operator(cr,CAIRO_OPERATOR_CLEAR);
        cairo_paint(cr);
        cairo_set_operator(cr,CAIRO_OPERATOR_OVER);
        cairo_scale(cr,inkWidth_/1280.,inkHeight_/720.);
        paint(cr);
        auto status=cairo_status(cr);
        cairo_destroy(cr);
        if (status!=CAIRO_STATUS_SUCCESS) throw std::runtime_error(cairo_status_to_string(status));
        cairo_surface_flush(surface_);
        int pitch=cairo_image_surface_get_stride(surface_);
        auto *dst=SDL_MapGPUTransferBuffer(gpu_,upload_,true);
        require(dst,"Map menu upload buffer");
        std::memcpy(dst,cairo_image_surface_get_data(surface_),pitch*inkHeight_);
        SDL_UnmapGPUTransferBuffer(gpu_,upload_);
        SDL_GPUTextureTransferInfo src{upload_,0,static_cast<Uint32>(pitch/4),static_cast<Uint32>(inkHeight_)};
        SDL_GPUTextureRegion region{};
        region.texture=ink_; region.w=inkWidth_; region.h=inkHeight_; region.d=1;
        auto *copy=SDL_BeginGPUCopyPass(cmd);
        SDL_UploadToGPUTexture(copy,&src,&region,true);
        SDL_EndGPUCopyPass(copy);
    }
    SDL_GPUColorTargetInfo target{};
    target.texture=swap;
    target.load_op=SDL_GPU_LOADOP_DONT_CARE;
    target.store_op=SDL_GPU_STOREOP_STORE;
    const float uniforms[]={(w-scale*1280)/2,(h-scale*720)/2,scale*1280,scale*720,
                            frame.time,frame.installMix,frame.progress,frame.fade,
                            layout::barLeft,layout::barTop,layout::barWidth,layout::barHeight,
                            layout::rail,layout::footerTop,frame.menu?1.f:0.f,0,
                            frame.opening.light,frame.opening.railX,frame.opening.choicesX,frame.opening.footerY,
                            frame.completionAge,0,0,0};
    SDL_PushGPUFragmentUniformData(cmd,0,uniforms,sizeof uniforms);
    auto *pass=SDL_BeginGPURenderPass(cmd,&target,1,nullptr);
    SDL_BindGPUGraphicsPipeline(pass,pipeline_);
    SDL_GPUTextureSamplerBinding binding{ink_,sampler_};
    SDL_BindGPUFragmentSamplers(pass,0,&binding,1);
    SDL_DrawGPUPrimitives(pass,3,1,0,0);
    SDL_EndGPURenderPass(pass);
    require(SDL_SubmitGPUCommandBuffer(cmd), "Present menu");
    return true;
}

Graphics::~Graphics() {
    if (surface_) cairo_surface_destroy(surface_);
    if (gpu_) {
        SDL_WaitForGPUIdle(gpu_);
        if (ink_) SDL_ReleaseGPUTexture(gpu_,ink_);
        if (upload_) SDL_ReleaseGPUTransferBuffer(gpu_,upload_);
        if (sampler_) SDL_ReleaseGPUSampler(gpu_,sampler_);
        if (pipeline_) SDL_ReleaseGPUGraphicsPipeline(gpu_,pipeline_);
        if (claimed_) SDL_ReleaseWindowFromGPUDevice(gpu_,window_);
        SDL_DestroyGPUDevice(gpu_);
    }
    if (window_) SDL_DestroyWindow(window_);
}
}
