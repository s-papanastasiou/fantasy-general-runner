// SPDX-License-Identifier: GPL-2.0-or-later
// The CRT filter: DOSBox Staging's VGA monitor shaders (MIT, runner/shaders/)
// applied to the game image as it is drawn. Like DOSBox Staging's crt-auto,
// the look follows how many screen rows each of the game's 480 lines gets:
//   4 or more  crt-hyllian, preset vga-4k
//   3          crt-hyllian, preset vga-1440p
//   exactly 2  vga-1080p
//   fewer      no filter (sharp pixels)
// The caller scales the picture by whole numbers vertically while the filter
// is on, as the shaders expect. Needs SDL's GPU renderer with SPIR-V shaders
// (Vulkan); otherwise the filter is unavailable.
#pragma once
#include <SDL3/SDL.h>

#include <string>

namespace fg {

class CrtFilter {
public:
    ~CrtFilter() { shutdown(); }
    // False (and logged) if the renderer cannot run the shaders.
    bool init(SDL_Renderer* ren);
    void shutdown();
    bool available() const { return state_hyllian_ != nullptr; }

    // The look for a src_w x src_h image drawn `rows` screen rows high
    // ("vga-1440p", ...), or nullptr if the filter does not apply.
    static const char* preset_for(int src_w, int src_h, int rows);

    // Around drawing the game image (src_w x src_h) into `dst` (target
    // pixels): begin() returns false if the image is to be drawn plainly.
    bool begin(SDL_Renderer* ren, int src_w, int src_h, const SDL_FRect& dst);
    void end(SDL_Renderer* ren);

    // For checks without a person: draws `tex` filtered as on an out_w x
    // out_h screen (integer vertical scale, centred) into an offscreen image
    // and writes it to `png`.
    bool render_to_png(SDL_Renderer* ren, SDL_Texture* tex, int src_w, int src_h, int out_w, int out_h,
                       const std::string& png);

private:
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUShader* shader_1080p_ = nullptr;
    SDL_GPUShader* shader_hyllian_ = nullptr;
    SDL_GPURenderState* state_1080p_ = nullptr;
    SDL_GPURenderState* state_hyllian_ = nullptr;
};

}  // namespace fg
