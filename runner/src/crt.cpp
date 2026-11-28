// SPDX-License-Identifier: GPL-2.0-or-later
#include "crt.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "log.h"
#include "png.h"
#include "video.h"

extern "C" {
// crt_shaders.S: SPIR-V compiled from runner/shaders/ at build time.
extern const unsigned char fg_spv_vga_1080p[], fg_spv_vga_1080p_end[];
extern const unsigned char fg_spv_crt_hyllian[], fg_spv_crt_hyllian_end[];
}

namespace fg {

namespace {

// Uniform blocks of the shaders (std140: two vec2, then floats, padded to 16 bytes).
struct Vga1080pParams {
    float input_size[2], output_start[2];
    float spot_width, spot_height, phosphor_layout, scanline_strength_min, scanline_strength_max,
        color_boost_even, color_boost_odd, mask_strength, gamma_input, gamma_output, pad[2];
};
struct HyllianParams {
    float input_size[2], output_start[2];
    float beam_profile, hfilter_profile, beam_min_width, beam_max_width, scanlines_strength, color_boost,
        sharpness_hack, phosphor_layout, mask_intensity, crt_anti_ringing, input_gamma, output_gamma,
        vscanlines, pad[3];
};
static_assert(sizeof(Vga1080pParams) == 64 && sizeof(HyllianParams) == 80, "std140 layout");

// DOSBox Staging's values: vga-1080p.glsl's defaults, and the presets
// resources/shader-presets/crt/crt-hyllian/vga-1440p.preset and vga-4k.preset.
constexpr Vga1080pParams kVga1080p{{}, {}, 0.85f, 0.80f, 2.0f, 0.80f, 0.85f, 4.80f, 1.40f, 0.10f, 2.4f, 2.62f, {}};
constexpr HyllianParams kVga1440p{{}, {}, 0, 0, 0.95f, 1.30f, 0.85f, 2.50f, 1, 2, 0.55f, 1, 2.40f, 2.48f, 0, {}};
constexpr HyllianParams kVga4k{{}, {}, 0, 0, 1.00f, 1.40f, 0.90f, 3.65f, 1, 11, 0.65f, 1, 2.40f, 2.48f, 0, {}};

SDL_GPUShader* make_shader(SDL_GPUDevice* dev, const unsigned char* begin, const unsigned char* end) {
    SDL_GPUShaderCreateInfo si{};
    si.code = begin;
    si.code_size = size_t(end - begin);
    si.entrypoint = "main";
    si.format = SDL_GPU_SHADERFORMAT_SPIRV;
    si.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    si.num_samplers = 1;
    si.num_uniform_buffers = 1;
    SDL_GPUShader* s = SDL_CreateGPUShader(dev, &si);
    if (!s) FG_LOG("crt: cannot create a shader: %s", SDL_GetError());
    return s;
}

}  // namespace

bool CrtFilter::init(SDL_Renderer* ren) {
    shutdown();
    if (std::strcmp(SDL_GetRendererName(ren), SDL_GPU_RENDERER) != 0) {
        FG_LOG("crt: unavailable with the %s renderer (needs SDL's GPU renderer)", SDL_GetRendererName(ren));
        return false;
    }
    device_ = SDL_GetGPURendererDevice(ren);
    if (!device_ || !(SDL_GetGPUShaderFormats(device_) & SDL_GPU_SHADERFORMAT_SPIRV)) {
        FG_LOG("crt: unavailable: the GPU device (%s) takes no SPIR-V shaders",
               device_ ? SDL_GetGPUDeviceDriver(device_) : "none");
        return false;
    }
    shader_1080p_ = make_shader(device_, fg_spv_vga_1080p, fg_spv_vga_1080p_end);
    shader_hyllian_ = make_shader(device_, fg_spv_crt_hyllian, fg_spv_crt_hyllian_end);
    if (shader_1080p_ && shader_hyllian_) {
        SDL_GPURenderStateCreateInfo ci{};
        ci.fragment_shader = shader_1080p_;
        state_1080p_ = SDL_CreateGPURenderState(ren, &ci);
        ci.fragment_shader = shader_hyllian_;
        state_hyllian_ = state_1080p_ ? SDL_CreateGPURenderState(ren, &ci) : nullptr;
        if (!state_hyllian_) FG_LOG("crt: cannot create the render state: %s", SDL_GetError());
    }
    if (!available()) {
        shutdown();
        return false;
    }
    FG_LOG("crt: available (%s)", SDL_GetGPUDeviceDriver(device_));
    return true;
}

void CrtFilter::shutdown() {
    if (state_1080p_) SDL_DestroyGPURenderState(state_1080p_);
    if (state_hyllian_) SDL_DestroyGPURenderState(state_hyllian_);
    if (shader_1080p_) SDL_ReleaseGPUShader(device_, shader_1080p_);
    if (shader_hyllian_) SDL_ReleaseGPUShader(device_, shader_hyllian_);
    state_1080p_ = state_hyllian_ = nullptr;
    shader_1080p_ = shader_hyllian_ = nullptr;
    device_ = nullptr;
}

const char* CrtFilter::preset_for(int src_w, int src_h, int rows) {
    if (src_w != 640 || src_h != 480) return nullptr;  // only the game's VGA screen
    int k = rows / src_h;
    if (k >= 4) return "vga-4k";
    if (k == 3) return "vga-1440p";
    if (k == 2 && rows == 2 * src_h) return "vga-1080p";  // exact 2x only
    return nullptr;
}

bool CrtFilter::begin(SDL_Renderer* ren, int src_w, int src_h, const SDL_FRect& dst) {
    if (!available()) return false;
    const char* preset = preset_for(src_w, src_h, int(std::lround(dst.h)));
    if (!preset) return false;
    float in[2] = {float(src_w), float(src_h)}, at[2] = {std::round(dst.x), std::round(dst.y)};
    if (std::strcmp(preset, "vga-1080p") == 0) {
        Vga1080pParams p = kVga1080p;
        std::memcpy(p.input_size, in, sizeof in);
        std::memcpy(p.output_start, at, sizeof at);
        SDL_SetGPURenderStateFragmentUniforms(state_1080p_, 0, &p, sizeof p);
        return SDL_SetGPURenderState(ren, state_1080p_);
    }
    HyllianParams p = std::strcmp(preset, "vga-4k") == 0 ? kVga4k : kVga1440p;
    std::memcpy(p.input_size, in, sizeof in);
    std::memcpy(p.output_start, at, sizeof at);
    SDL_SetGPURenderStateFragmentUniforms(state_hyllian_, 0, &p, sizeof p);
    return SDL_SetGPURenderState(ren, state_hyllian_);
}

void CrtFilter::end(SDL_Renderer* ren) { SDL_SetGPURenderState(ren, nullptr); }

bool CrtFilter::render_to_png(SDL_Renderer* ren, SDL_Texture* tex, int src_w, int src_h, int out_w, int out_h,
                              const std::string& png) {
    SDL_Texture* target = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, out_w, out_h);
    if (!target) return false;
    int lw = 0, lh = 0;
    SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
    SDL_GetRenderLogicalPresentation(ren, &lw, &lh, &mode);
    SDL_SetRenderTarget(ren, target);
    SDL_SetRenderLogicalPresentation(ren, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    int k = std::max(1, out_h / src_h);
    float h = float(k * src_h), w = h * 4 / 3;
    SDL_FRect dst{std::floor((out_w - w) / 2), std::floor((out_h - h) / 2), w, h};
    bool filtered = begin(ren, src_w, src_h, dst);
    SDL_RenderTexture(ren, tex, nullptr, &dst);
    if (filtered) end(ren);
    bool ok = false;
    if (SDL_Surface* sf = SDL_RenderReadPixels(ren, nullptr)) {
        if (SDL_Surface* c = SDL_ConvertSurface(sf, SDL_PIXELFORMAT_XRGB8888)) {
            Frame f;
            f.width = c->w;
            f.height = c->h;
            f.pixels.resize(size_t(c->w) * c->h);
            for (int y = 0; y < c->h; ++y)
                std::memcpy(&f.pixels[size_t(y) * c->w], static_cast<uint8_t*>(c->pixels) + y * c->pitch,
                            size_t(c->w) * 4);
            ok = write_png(png, f);
            SDL_DestroySurface(c);
        }
        SDL_DestroySurface(sf);
    }
    SDL_SetRenderTarget(ren, nullptr);
    SDL_SetRenderLogicalPresentation(ren, lw, lh, mode);
    SDL_DestroyTexture(target);
    FG_LOG("crt: %s %dx%d (%s) -> %s", ok ? "wrote" : "could not write", out_w, out_h,
           filtered ? preset_for(src_w, src_h, int(h)) : "unfiltered", png.c_str());
    return ok;
}

}  // namespace fg
