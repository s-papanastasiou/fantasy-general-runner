// SPDX-License-Identifier: GPL-2.0-or-later
// Text for the launcher: one TrueType font at one size, rasterised with
// stb_truetype into a glyph atlas (Latin, Greek and Cyrillic, so folder
// names in those scripts show) and drawn with an SDL renderer.
#pragma once
#include <SDL3/SDL.h>

#include <string>
#include <string_view>
#include <vector>

namespace fg {

enum class FontFace { Regular, SemiBold, Display };  // Inter, Inter, Metamorphous

class LauncherFont {
public:
    ~LauncherFont();
    // `px` in layout units; `density` = screen pixels per layout unit (the
    // renderer's scale), so glyphs are rasterised at their real size.
    bool init(SDL_Renderer* ren, FontFace face, float px, float density);
    void release();

    // Width in layout units.
    float width(std::string_view utf8) const;
    // Draws with the top of the line box at y.
    void draw(SDL_Renderer* ren, std::string_view utf8, float x, float y, SDL_Color c) const;
    // `utf8` shortened in the middle with "…" to fit `max_w`.
    std::string elide(std::string_view utf8, float max_w) const;
    // Lines of at most `max_w`, broken at spaces.
    std::vector<std::string> wrap(std::string_view utf8, float max_w) const;

    float line_height() const { return line_h_; }

private:
    struct Range { int first, count, offset; };
    int glyph_index(uint32_t cp) const;

    SDL_Texture* tex_ = nullptr;
    int atlas_w_ = 0, atlas_h_ = 0;
    float density_ = 1, ascent_ = 0, line_h_ = 0;
    std::vector<Range> ranges_;
    std::vector<unsigned char> packed_;  // stbtt_packedchar[], opaque here
};

}  // namespace fg
