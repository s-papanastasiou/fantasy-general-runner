// SPDX-License-Identifier: GPL-2.0-or-later
#include "launcher_text.h"

#include <cmath>

#include "log.h"

#include "stb_truetype.h"  // implementation: third_party/stb/stb_truetype.c

extern "C" {
// launcher_fonts.S
extern const unsigned char fg_font_inter_regular[], fg_font_inter_semibold[], fg_font_metamorphous[];
}

namespace fg {

namespace {

// Next code point of `s` at `i` (advanced); U+FFFD for bad bytes.
uint32_t next_cp(std::string_view s, size_t& i) {
    unsigned char c = static_cast<unsigned char>(s[i++]);
    if (c < 0x80) return c;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
    if (n < 0) return 0xFFFD;
    uint32_t cp = c & (0x3F >> n);
    for (int k = 0; k < n; ++k) {
        if (i >= s.size() || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3F);
    }
    return cp;
}

const stbtt_packedchar* chars(const std::vector<unsigned char>& v) {
    return reinterpret_cast<const stbtt_packedchar*>(v.data());
}

}  // namespace

LauncherFont::~LauncherFont() { release(); }

void LauncherFont::release() {
    if (tex_) SDL_DestroyTexture(tex_);
    tex_ = nullptr;
}

bool LauncherFont::init(SDL_Renderer* ren, FontFace face, float px, float density) {
    release();
    const unsigned char* data = face == FontFace::Regular    ? fg_font_inter_regular
                                : face == FontFace::SemiBold ? fg_font_inter_semibold
                                                             : fg_font_metamorphous;
    density_ = density;
    float size = px * density;
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, data, stbtt_GetFontOffsetForIndex(data, 0))) return false;
    float scale = stbtt_ScaleForPixelHeight(&info, size);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
    ascent_ = asc * scale;
    line_h_ = (asc - desc + gap) * scale / density;

    // ASCII, Latin-1 and Latin Extended-A, Greek, Cyrillic, general punctuation
    // (dashes, quotes, bullet, ellipsis). The display face (Metamorphous, Latin
    // only) is for the launcher's own words: ASCII and Latin-1.
    ranges_.clear();
    auto add = [&](int first, int count) {
        int offset = ranges_.empty() ? 0 : ranges_.back().offset + ranges_.back().count;
        ranges_.push_back({first, count, offset});
    };
    add(32, 95);
    add(160, 96);
    if (face != FontFace::Display) {
        add(256, 128);
        add(0x370, 144);
        add(0x400, 256);
    }
    add(0x2010, 0x18);
    int total = ranges_.back().offset + ranges_.back().count;
    packed_.assign(sizeof(stbtt_packedchar) * total, 0);
    auto* pc_chars = reinterpret_cast<stbtt_packedchar*>(packed_.data());
    std::vector<stbtt_pack_range> pr;
    for (const Range& r : ranges_) {
        stbtt_pack_range x{};
        x.font_size = size;
        x.first_unicode_codepoint_in_range = r.first;
        x.num_chars = r.count;
        x.chardata_for_range = pc_chars + r.offset;
        pr.push_back(x);
    }
    // Small text is sharper with horizontal oversampling.
    int over = size < 40 ? 2 : 1;
    std::vector<unsigned char> alpha;
    for (atlas_w_ = 512, atlas_h_ = 512;; atlas_h_ < atlas_w_ ? atlas_h_ *= 2 : atlas_w_ *= 2) {
        if (atlas_w_ > 4096) {
            FG_LOG("launcher: font atlas too large (%.0f px)", size);
            return false;
        }
        alpha.assign(size_t(atlas_w_) * atlas_h_, 0);
        stbtt_pack_context pc;
        if (!stbtt_PackBegin(&pc, alpha.data(), atlas_w_, atlas_h_, 0, 1, nullptr)) return false;
        stbtt_PackSetOversampling(&pc, over, 1);
        int ok = stbtt_PackFontRanges(&pc, data, 0, pr.data(), int(pr.size()));
        stbtt_PackEnd(&pc);
        if (ok) break;
    }
    std::vector<uint32_t> rgba(alpha.size());
    for (size_t i = 0; i < alpha.size(); ++i) rgba[i] = 0x00FFFFFFu | (uint32_t(alpha[i]) << 24);
    tex_ = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, atlas_w_, atlas_h_);
    if (!tex_) {
        FG_LOG("launcher: %s", SDL_GetError());
        return false;
    }
    SDL_UpdateTexture(tex_, nullptr, rgba.data(), atlas_w_ * 4);
    SDL_SetTextureBlendMode(tex_, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(tex_, SDL_SCALEMODE_LINEAR);
    return true;
}

int LauncherFont::glyph_index(uint32_t cp) const {
    for (const Range& r : ranges_)
        if (cp >= uint32_t(r.first) && cp < uint32_t(r.first + r.count)) return r.offset + int(cp - r.first);
    return '?' - 32;  // not in the atlas
}

float LauncherFont::width(std::string_view s) const {
    if (packed_.empty()) return 0;
    float x = 0, y = 0;
    stbtt_aligned_quad q;
    for (size_t i = 0; i < s.size();)
        stbtt_GetPackedQuad(chars(packed_), atlas_w_, atlas_h_, glyph_index(next_cp(s, i)), &x, &y, &q, 0);
    return x / density_;
}

void LauncherFont::draw(SDL_Renderer* ren, std::string_view s, float x, float y, SDL_Color c) const {
    if (!tex_) return;
    SDL_SetTextureColorMod(tex_, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(tex_, c.a);
    float px = std::round(x * density_), py = std::round(y * density_ + ascent_);
    stbtt_aligned_quad q;
    for (size_t i = 0; i < s.size();) {
        stbtt_GetPackedQuad(chars(packed_), atlas_w_, atlas_h_, glyph_index(next_cp(s, i)), &px, &py, &q, 1);
        if (q.x1 <= q.x0) continue;  // a space
        SDL_FRect src{q.s0 * atlas_w_, q.t0 * atlas_h_, (q.s1 - q.s0) * atlas_w_, (q.t1 - q.t0) * atlas_h_};
        SDL_FRect dst{q.x0 / density_, q.y0 / density_, (q.x1 - q.x0) / density_, (q.y1 - q.y0) / density_};
        SDL_RenderTexture(ren, tex_, &src, &dst);
    }
}

std::string LauncherFont::elide(std::string_view s, float max_w) const {
    if (width(s) <= max_w) return std::string(s);
    std::vector<std::string> cps;
    for (size_t i = 0; i < s.size();) {
        size_t a = i;
        next_cp(s, i);
        cps.emplace_back(s.substr(a, i - a));
    }
    size_t keep_head = cps.size() / 2, keep_tail = cps.size() - keep_head;
    for (;;) {
        std::string t;
        for (size_t k = 0; k < keep_head; ++k) t += cps[k];
        t += "…";
        for (size_t k = cps.size() - keep_tail; k < cps.size(); ++k) t += cps[k];
        if (width(t) <= max_w || (keep_head == 0 && keep_tail == 0)) return t;
        // Keep more of the end: for paths the last folders matter most.
        if (keep_head > keep_tail / 2 && keep_head) --keep_head;
        else --keep_tail;
    }
}

std::vector<std::string> LauncherFont::wrap(std::string_view s, float max_w) const {
    std::vector<std::string> lines;
    std::string cur;
    size_t i = 0;
    while (i <= s.size()) {
        size_t sp = s.find(' ', i);
        if (sp == std::string_view::npos) sp = s.size();
        std::string word(s.substr(i, sp - i));
        std::string trial = cur.empty() ? word : cur + " " + word;
        if (!cur.empty() && width(trial) > max_w) {
            lines.push_back(cur);
            cur = word;
        } else {
            cur = trial;
        }
        i = sp + 1;
    }
    if (!cur.empty() || lines.empty()) lines.push_back(cur);
    return lines;
}

}  // namespace fg
