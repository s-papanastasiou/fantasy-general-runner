// SPDX-License-Identifier: GPL-2.0-or-later
// The launcher window (launcher.h). Drawn with SDL's renderer in layout
// units (760 x 660, scaled to the display), as an immediate-mode UI: every
// frame draws all widgets and handles the mouse in the same pass. Colours
// and fonts follow the game's menus: parchment, gold frames, maroon
// buttons with gold lettering, a teal title plaque (Inter and Metamorphous).
#include "launcher.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "game_locate.h"
#include "launcher_text.h"
#include "log.h"
#include "png.h"
#include "video.h"

#ifndef FG_VERSION
#define FG_VERSION "dev"
#endif

namespace fg {

namespace {

constexpr float kW = 760, kH = 660, kPad = 28;

constexpr SDL_Color rgb(uint32_t v) { return {uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v), 255}; }

// Gilded parchment.
constexpr SDL_Color kFrameDark = rgb(0x4a2c10), kFrameGold = rgb(0xd9a93a), kGround = rgb(0xead08a),
                    kText = rgb(0x2b1a0c), kMuted = rgb(0x5a4022), kDivider = rgb(0xb38f4c),
                    kField = rgb(0xf5e4ad), kFieldHover = rgb(0xfbeec4), kFieldBorder = rgb(0x8a6528),
                    kMaroon = rgb(0x7a1f12), kMaroonHover = rgb(0x922817), kGoldText = rgb(0xf6d55c),
                    kPlaque = rgb(0x123a48), kPlaqueTitle = rgb(0xf3e6c0), kPlaqueText = rgb(0xcfd8d6),
                    kOk = rgb(0x9fe0a8), kBadOnPlaque = rgb(0xffb4a0), kBad = rgb(0x9a2a18),
                    kCodeBg = rgb(0x3b2614), kCodeText = rgb(0xf3dca8), kCodeCaption = rgb(0xd9c08a),
                    kSegOffText = rgb(0x3b2614), kDisabledBg = rgb(0xcdb67e), kDisabledText = rgb(0x6b5530);

SDL_FColor fcol(SDL_Color c) { return {c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f}; }

bool inside(const SDL_FRect& r, float x, float y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

// A folder dialog's answer; SDL may call back on another thread. Owned by
// the launcher, or leaked when the window closes with a dialog still open.
struct DialogState {
    std::mutex m;
    bool done = false;
    bool cancelled = false;
    std::string path;
};

enum class Pick { None, Game, Saves };

class Launcher {
public:
    Launcher(Settings& s, std::string& game_dir) : s_(s), game_dir_(game_dir) {}
    LauncherResult run();

private:
    // Fonts.
    struct Fonts {
        LauncherFont title, tab, label, launch, caption;     // Metamorphous
        LauncherFont r12, r13, r14, sb13, sb14;              // Inter
    } f_;
    bool open_window();
    void fit_to_display();
    bool load_fonts();
    void frame();

    // Drawing.
    void fill(const SDL_FRect& r, SDL_Color c);
    void round_rect(const SDL_FRect& r, float radius, SDL_Color c);
    void framed(const SDL_FRect& r, float radius, float border, SDL_Color edge, SDL_Color inner);
    void circle(float cx, float cy, float radius, SDL_Color c);
    void stroke(float x0, float y0, float x1, float y1, float w, SDL_Color c);
    void text(const LauncherFont& f, std::string_view s, float x, float y, SDL_Color c) { f.draw(ren_, s, x, y, c); }

    // Widgets: each returns true when clicked (pressed and released on it).
    bool hit(int id, const SDL_FRect& r, bool enabled = true);
    float segmented(float x, float y, const std::vector<std::string>& labels, int selected, int id, int& clicked);
    void slider(int id, float x, float y, float w, int& v, int lo, int hi, int step);
    float label_row(float y, std::string_view label, std::string_view value = {});
    float hint(float y, std::string_view s, SDL_Color c = kMuted);

    void tab_game(float y);
    void tab_display(float y);
    void tab_sound(float y);
    void tab_play(float y);
    void open_dialog(Pick what);
    void poll_dialog();

    Settings& s_;
    std::string& game_dir_;
    bool game_picked_ = false;      // chosen in this launcher (remembered on Launch)
    std::string game_error_;
    SDL_Window* win_ = nullptr;
    SDL_Renderer* ren_ = nullptr;
    float scale_ = 1;               // screen pixels per layout unit
    int tab_ = 0;
    LauncherResult result_ = LauncherResult::Quit;
    bool done_ = false;

    // Mouse state for this frame.
    float mx_ = -1, my_ = -1;
    bool pressed_ = false, released_ = false, down_ = false;
    int active_ = 0;                // widget the button went down on
    bool pointer_ = false;          // something clickable is under the mouse
    SDL_Cursor* arrow_ = nullptr;
    SDL_Cursor* hand_ = nullptr;
    bool hand_shown_ = false;

    std::unique_ptr<DialogState> dialog_;
    Pick dialog_for_ = Pick::None;
};

bool Launcher::open_window() {
    auto attempt = [&](const char* driver) {
        if (driver) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, driver);
        SDL_SetHint(SDL_HINT_APP_ID, "fantasy-general-runner");
        SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            FG_LOG("launcher: SDL video: %s", SDL_GetError());
            return false;
        }
        if (SDL_CreateWindowAndRenderer("Fantasy General", int(kW), int(kH),
                                        SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY, &win_, &ren_))
            return true;
        FG_LOG("launcher: no renderer with %s video (%s)", SDL_GetCurrentVideoDriver(), SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    };
    // As for the game window (ui.cpp): a 32-bit runner may have no renderer
    // under Wayland but X11 software rendering still works.
    return attempt(nullptr) || attempt("x11");
}

// Window size for the display's scale, kept within the usable area.
void Launcher::fit_to_display() {
    SDL_DisplayID d = SDL_GetDisplayForWindow(win_);
    float want = SDL_GetWindowDisplayScale(win_);
    if (want <= 0) want = 1;
    float density = SDL_GetWindowPixelDensity(win_);
    if (density <= 0) density = 1;
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(d, &usable)) {
        // Usable bounds are in window units; leave room for the title bar.
        float fit = std::min(usable.w * density / kW, (usable.h - 48) * density / kH);
        want = std::min(want, std::max(0.5f, fit));
    }
    SDL_SetWindowSize(win_, int(std::lround(kW * want / density)), int(std::lround(kH * want / density)));
    SDL_SyncWindow(win_);
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(win_, &pw, &ph);
    scale_ = std::max(0.25f, std::min(pw / kW, ph / kH));
    SDL_SetRenderScale(ren_, scale_, scale_);
}

bool Launcher::load_fonts() {
    float d = scale_;
    return f_.title.init(ren_, FontFace::Display, 32, d) && f_.tab.init(ren_, FontFace::Display, 16, d) &&
           f_.label.init(ren_, FontFace::Display, 15, d) && f_.launch.init(ren_, FontFace::Display, 17, d) &&
           f_.caption.init(ren_, FontFace::Display, 12, d) && f_.r12.init(ren_, FontFace::Regular, 12, d) &&
           f_.r13.init(ren_, FontFace::Regular, 13, d) && f_.r14.init(ren_, FontFace::Regular, 14, d) &&
           f_.sb13.init(ren_, FontFace::SemiBold, 13, d) && f_.sb14.init(ren_, FontFace::SemiBold, 14, d);
}

void Launcher::fill(const SDL_FRect& r, SDL_Color c) {
    SDL_SetRenderDrawColor(ren_, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(ren_, &r);
}

void Launcher::round_rect(const SDL_FRect& r, float radius, SDL_Color c) {
    radius = std::min({radius, r.w / 2, r.h / 2});
    std::vector<SDL_Vertex> v;
    SDL_FColor fc = fcol(c);
    v.push_back({{r.x + r.w / 2, r.y + r.h / 2}, fc, {0, 0}});
    const float cx[4] = {r.x + r.w - radius, r.x + radius, r.x + radius, r.x + r.w - radius};
    const float cy[4] = {r.y + radius, r.y + radius, r.y + r.h - radius, r.y + r.h - radius};
    constexpr int kSteps = 6;
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i <= kSteps; ++i) {
            float a = (k * 90 + i * 90.f / kSteps) * 3.14159265f / 180;
            v.push_back({{cx[k] + radius * std::cos(a), cy[k] - radius * std::sin(a)}, fc, {0, 0}});
        }
    std::vector<int> idx;
    int n = int(v.size()) - 1;
    for (int i = 1; i <= n; ++i) {
        idx.push_back(0);
        idx.push_back(i);
        idx.push_back(i % n + 1);
    }
    SDL_RenderGeometry(ren_, nullptr, v.data(), int(v.size()), idx.data(), int(idx.size()));
}

void Launcher::framed(const SDL_FRect& r, float radius, float border, SDL_Color edge, SDL_Color inner) {
    round_rect(r, radius, edge);
    round_rect({r.x + border, r.y + border, r.w - 2 * border, r.h - 2 * border}, std::max(0.f, radius - border),
               inner);
}

void Launcher::circle(float cx, float cy, float radius, SDL_Color c) {
    round_rect({cx - radius, cy - radius, 2 * radius, 2 * radius}, radius, c);
}

void Launcher::stroke(float x0, float y0, float x1, float y1, float w, SDL_Color c) {
    float dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0) return;
    float nx = -dy / len * w / 2, ny = dx / len * w / 2;
    SDL_FColor fc = fcol(c);
    SDL_Vertex v[4] = {{{x0 + nx, y0 + ny}, fc, {0, 0}}, {{x1 + nx, y1 + ny}, fc, {0, 0}},
                       {{x1 - nx, y1 - ny}, fc, {0, 0}}, {{x0 - nx, y0 - ny}, fc, {0, 0}}};
    const int idx[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(ren_, nullptr, v, 4, idx, 6);
    circle(x0, y0, w / 2, c);  // round joints and ends
    circle(x1, y1, w / 2, c);
}

bool Launcher::hit(int id, const SDL_FRect& r, bool enabled) {
    if (dialog_ || !enabled) return false;
    bool over = inside(r, mx_, my_);
    if (over) pointer_ = true;
    if (over && pressed_) active_ = id;
    return over && released_ && active_ == id;
}

// A row of option buttons; returns its width. `clicked` gets the index of
// the one clicked, if any.
float Launcher::segmented(float x, float y, const std::vector<std::string>& labels, int selected, int id,
                          int& clicked) {
    float x0 = x;
    for (size_t i = 0; i < labels.size(); ++i) {
        bool on = int(i) == selected;
        const LauncherFont& font = on ? f_.sb14 : f_.r14;
        SDL_FRect r{x, y, font.width(labels[i]) + 32, 40};
        bool over = !dialog_ && inside(r, mx_, my_);
        if (hit(id + int(i), r)) clicked = int(i);
        if (on) framed(r, 6, 2, kFrameGold, over ? kMaroonHover : kMaroon);
        else framed(r, 6, 1, kFieldBorder, over ? kFieldHover : kField);
        text(font, labels[i], r.x + 16, r.y + (r.h - font.line_height()) / 2, on ? kGoldText : kSegOffText);
        x += r.w + 4;
    }
    return x - x0;
}

void Launcher::slider(int id, float x, float y, float w, int& v, int lo, int hi, int step) {
    SDL_FRect r{x - 10, y, w + 20, 28};
    float cy = y + 14;
    hit(id, r);
    if (active_ == id && down_ && !dialog_) {
        float t = std::clamp((mx_ - x) / w, 0.f, 1.f);
        int nv = lo + int(std::lround(t * (hi - lo) / step)) * step;
        v = std::clamp(nv, lo, hi);
    }
    float t = float(std::clamp(v, lo, hi) - lo) / float(hi - lo);
    round_rect({x, cy - 2.5f, w, 5}, 2.5f, kDivider);
    round_rect({x, cy - 2.5f, std::max(5.f, w * t), 5}, 2.5f, kMaroon);
    bool big = active_ == id || inside(r, mx_, my_);
    circle(x + w * t, cy, big ? 10.5f : 9.5f, kFrameGold);
    circle(x + w * t, cy, big ? 8 : 7, kMaroon);
}

// A setting's name, and its value on the right. Returns the y below it.
float Launcher::label_row(float y, std::string_view label, std::string_view value) {
    text(f_.label, label, kPad, y, kText);
    if (!value.empty()) text(f_.sb13, value, kW - kPad - f_.sb13.width(value), y + 2, kMaroon);
    return y + f_.label.line_height() + 4;
}

float Launcher::hint(float y, std::string_view s, SDL_Color c) {
    text(f_.r12, f_.r12.elide(s, kW - 2 * kPad), kPad, y, c);
    return y + f_.r12.line_height();
}

void Launcher::open_dialog(Pick what) {
    dialog_ = std::make_unique<DialogState>();
    dialog_for_ = what;
    std::string start = what == Pick::Game ? game_dir_ : s_.save_dir;
    SDL_ShowOpenFolderDialog(
        [](void* ud, const char* const* files, int) {
            auto* d = static_cast<DialogState*>(ud);
            std::lock_guard<std::mutex> lock(d->m);
            if (!files) FG_LOG("folder dialog: %s", SDL_GetError());
            if (!files || !files[0]) d->cancelled = true;
            else d->path = files[0];
            d->done = true;
        },
        dialog_.get(), win_, start.empty() ? nullptr : start.c_str(), false);
}

void Launcher::poll_dialog() {
    if (!dialog_) return;
    std::string path;
    {
        std::lock_guard<std::mutex> lock(dialog_->m);
        if (!dialog_->done) return;
        if (!dialog_->cancelled) path = dialog_->path;
    }
    dialog_.reset();
    if (path.empty()) return;
    if (dialog_for_ == Pick::Game) {
        std::string d = game_dir_in(path);
        if (d.empty()) {
            game_error_ = "No Fantasy General files (EXE\\BARENA.EXE) in " + path;
            FG_LOG("launcher: no EXE\\BARENA.EXE in %s", path.c_str());
        } else {
            game_dir_ = d;
            game_picked_ = true;
            game_error_.clear();
        }
    } else {
        s_.save_dir = path;
    }
}

void Launcher::tab_game(float y) {
    y = label_row(y, "Game folder");
    const LauncherFont& b = f_.r14;
    float bw = b.width("Browse…") + 32;
    SDL_FRect field{kPad, y, kW - 2 * kPad - bw - 8, 40};
    framed(field, 6, 1, kFieldBorder, kField);
    if (game_dir_.empty())
        text(f_.r13, "Not found", field.x + 12, y + (40 - f_.r13.line_height()) / 2, kBad);
    else
        text(f_.r13, f_.r13.elide(game_dir_, field.w - 24), field.x + 12, y + (40 - f_.r13.line_height()) / 2,
             kText);
    SDL_FRect browse{field.x + field.w + 8, y, bw, 40};
    bool over = !dialog_ && inside(browse, mx_, my_);
    if (hit(10, browse)) open_dialog(Pick::Game);
    framed(browse, 6, 1, kFieldBorder, over ? kFieldHover : kField);
    text(b, "Browse…", browse.x + 16, y + (40 - b.line_height()) / 2, kText);
    y += 48;
    if (game_dir_.empty())
        y = hint(y, "Choose the folder the GOG installer installed Fantasy General to.", kBad);
    else if (game_picked_)
        y = hint(y, "Chosen by you; remembered for next time. --game-dir");
    else
        y = hint(y, "Found automatically (GOG, Galaxy, Heroic, Lutris, Wine). --game-dir");
    if (!game_error_.empty()) y = hint(y + 2, game_error_, kBad);

    y = label_row(y + 20, "Saved games");
    int c = -1;
    segmented(kPad, y, {"Automatic", "Choose folder…"}, s_.save_dir.empty() ? 0 : 1, 20, c);
    if (c == 0) s_.save_dir.clear();
    if (c == 1) open_dialog(Pick::Saves);
    y += 48;
    if (!s_.save_dir.empty()) {
        hint(y, s_.save_dir + "  --save-dir");
    } else if (game_dir_.empty()) {
        hint(y, "Next to the game when it can be written to, else in your user folder.");
    } else {
        std::string w = choose_write_overlay(game_dir_);
        hint(y, w.empty() ? "In the game's own SAVES folder." : "In " + w);
    }
}

void Launcher::tab_display(float y) {
    int c = -1;
    y = label_row(y, "Start in");
    segmented(kPad, y, {"Fullscreen", "Window"}, s_.fullscreen ? 0 : 1, 30, c);
    if (c >= 0) s_.fullscreen = c == 0;
    y = hint(y + 48, "Alt+Enter switches while playing. --window");

    y = label_row(y + 20, "Window size");
    std::vector<std::string> sizes{"1×", "2×", "3×", "4×"};
    if (s_.scale > 4) sizes.push_back(std::to_string(s_.scale) + "×");
    c = -1;
    segmented(kPad, y, sizes, s_.scale > 4 ? 4 : s_.scale - 1, 40, c);
    if (c >= 0 && c < 4) s_.scale = c + 1;
    y = hint(y + 48, "640×480 times this, limited to what fits the screen. --scale");

    y = label_row(y + 20, "Scaling");
    c = -1;
    segmented(kPad, y, {"Integer (sharp)", "Fit (fills screen)"}, s_.integer_scale ? 0 : 1, 50, c);
    if (c >= 0) s_.integer_scale = c == 0;
    hint(y + 48, std::string(s_.integer_scale ? "Exact pixels, black borders."
                                              : "Fills the screen at 4:3, slightly uneven pixels.") +
                     " Ctrl+F11 switches while playing. --scaling");
}

void Launcher::tab_sound(float y) {
    y = label_row(y, "Music volume", std::to_string(s_.music_volume) + "%");
    slider(60, kPad, y, kW - 2 * kPad, s_.music_volume, 10, 100, 5);
    y = hint(y + 34, "10–100%. To turn music off, use the game's own music switch. --music-volume");

    y = label_row(y + 20, "Sound effects");
    int c = -1;
    segmented(kPad, y, {"On", "Off"}, s_.sound ? 0 : 1, 70, c);
    if (c >= 0) s_.sound = c == 0;
    hint(y + 48, "Off starts the game without its sound driver. --no-sound");
}

void Launcher::tab_play(float y) {
    auto ms = [](int v, const char* unit) { return v == 0 ? std::string("as fast as possible") : std::to_string(v) + unit; };
    y = label_row(y, "Edge scrolling");
    int c = -1;
    segmented(kPad, y, {"Eight-way", "Original"}, s_.original_scrolling ? 1 : 0, 80, c);
    if (c >= 0) s_.original_scrolling = c == 1;
    y = hint(y + 48, "Original: the screen sides scroll diagonally. --original-scrolling");

    y = label_row(y + 16, "Map scroll speed", std::to_string(s_.scroll_ms) + " ms per step");
    slider(90, kPad, y, kW - 2 * kPad, s_.scroll_ms, 40, 300, 10);
    y = hint(y + 30, "Higher is slower. Default 120. --scroll-ms");

    y = label_row(y + 14, "Computer's unit moves", ms(s_.ai_move_ms, " ms per pixel"));
    slider(91, kPad, y, kW - 2 * kPad, s_.ai_move_ms, 0, 30, 1);
    y = hint(y + 30, "Default 10 (about 0.4 s per hex); 0 is as fast as possible. --ai-move-ms");

    y = label_row(y + 14, "Computer's turn: time per map view", ms(s_.ai_view_ms, " ms"));
    slider(92, kPad, y, kW - 2 * kPad, s_.ai_view_ms, 0, 1000, 50);
    hint(y + 30, "Default 200 ms. --ai-view-ms");
}

void Launcher::frame() {
    pointer_ = false;
    poll_dialog();

    // Frame: dark, gold, dark (like the game's bevelled borders), parchment.
    fill({0, 0, kW, kH}, kFrameDark);
    fill({2, 2, kW - 4, kH - 4}, kFrameGold);
    fill({7, 7, kW - 14, kH - 14}, kFrameDark);
    fill({9, 9, kW - 18, kH - 18}, kGround);

    // Title plaque.
    float th = f_.title.line_height(), sh = f_.r13.line_height();
    SDL_FRect plaque{22, 20, kW - 44, 12 + th + 2 + sh + 12};
    framed(plaque, 4, 2, kFrameGold, kPlaque);
    text(f_.title, "Fantasy General", plaque.x + 16, plaque.y + 12, kPlaqueTitle);
    std::string ver = std::string("runner ") + FG_VERSION;
    text(f_.r12, ver, plaque.x + plaque.w - 16 - f_.r12.width(ver), plaque.y + 12 + th - f_.r12.line_height() - 4,
         kPlaqueText);
    float sy = plaque.y + 12 + th + 2;
    float ix = plaque.x + 16, iy = sy + sh / 2;
    if (!game_dir_.empty()) {
        stroke(ix + 1, iy, ix + 5, iy + 4, 2.2f, kOk);
        stroke(ix + 5, iy + 4, ix + 12, iy - 4, 2.2f, kOk);
        text(f_.r13, "Game found", ix + 20, sy, kPlaqueText);
    } else {
        stroke(ix + 2, iy - 4, ix + 10, iy + 4, 2.2f, kBadOnPlaque);
        stroke(ix + 10, iy - 4, ix + 2, iy + 4, 2.2f, kBadOnPlaque);
        text(f_.r13, "Game not found: choose its folder on the Game tab", ix + 20, sy, kBadOnPlaque);
    }

    // Tabs.
    float ty = plaque.y + plaque.h + 12;
    const char* tabs[4] = {"Game", "Display", "Sound", "Gameplay"};
    float tx = kPad;
    for (int i = 0; i < 4; ++i) {
        SDL_FRect r{tx, ty, f_.tab.width(tabs[i]) + 32, 40};
        if (hit(100 + i, r)) tab_ = i;
        bool over = !dialog_ && inside(r, mx_, my_);
        text(f_.tab, tabs[i], r.x + 16, r.y + (r.h - f_.tab.line_height()) / 2,
             tab_ == i || over ? kText : kMuted);
        if (tab_ == i) fill({r.x, r.y + r.h - 3, r.w, 3}, kMaroon);
        tx += r.w + 4;
    }
    fill({9, ty + 40, kW - 18, 1}, kDivider);

    float cy = ty + 40 + 20;
    switch (tab_) {
    case 0: tab_game(cy); break;
    case 1: tab_display(cy); break;
    case 2: tab_sound(cy); break;
    default: tab_play(cy); break;
    }

    // Footer: Restore defaults, Quit, Launch.
    float by = kH - 22 - 44;
    std::string rd = "Restore defaults";
    SDL_FRect rr{kPad - 4, by, f_.r14.width(rd) + 8, 44};
    if (hit(200, rr)) s_ = Settings{};
    bool over_rd = !dialog_ && inside(rr, mx_, my_);
    float rdy = by + (44 - f_.r14.line_height()) / 2;
    text(f_.r14, rd, kPad, rdy, over_rd ? kText : kMuted);
    fill({kPad, rdy + f_.r14.line_height() - 2, f_.r14.width(rd), 1}, over_rd ? kText : kMuted);

    bool can_launch = !game_dir_.empty();
    SDL_FRect launch{kW - kPad - std::max(140.f, f_.launch.width("Launch") + 48), by, 0, 44};
    launch.w = kW - kPad - launch.x;
    SDL_FRect quit{0, by, std::max(104.f, f_.tab.width("Quit") + 40), 44};
    quit.x = launch.x - 10 - quit.w;
    bool over_q = !dialog_ && inside(quit, mx_, my_);
    if (hit(201, quit)) { result_ = LauncherResult::Quit; done_ = true; }
    framed(quit, 6, 1, kFieldBorder, over_q ? kFieldHover : kField);
    text(f_.tab, "Quit", quit.x + (quit.w - f_.tab.width("Quit")) / 2, by + (44 - f_.tab.line_height()) / 2, kText);
    bool over_l = !dialog_ && inside(launch, mx_, my_);
    if (hit(202, launch)) {
        if (can_launch) { result_ = LauncherResult::Launch; done_ = true; }
    }
    if (!can_launch && over_l && released_) tab_ = 0;
    if (can_launch) framed(launch, 6, 2, kFrameGold, over_l ? kMaroonHover : kMaroon);
    else framed(launch, 6, 1, kFieldBorder, kDisabledBg);
    text(f_.launch, "Launch", launch.x + (launch.w - f_.launch.width("Launch")) / 2,
         by + (44 - f_.launch.line_height()) / 2, can_launch ? kGoldText : kDisabledText);
    if (over_l) pointer_ = true;

    // The equivalent command line.
    float ch = f_.caption.line_height(), lh = f_.r13.line_height();
    SDL_FRect box{kPad, 0, kW - 2 * kPad, 10 + ch + 4 + 2 * lh + 10};
    box.y = by - 16 - box.h;
    framed(box, 6, 1, kDivider, kCodeBg);
    text(f_.caption, "Same as running", box.x + 12, box.y + 10, kCodeCaption);
    std::string cmd = "fg-runner";
    for (const std::string& a : settings_args(s_)) cmd += " " + a;
    std::vector<std::string> lines = f_.r13.wrap(cmd, box.w - 24);
    if (lines.size() > 2) {
        std::string rest = lines[1];
        for (size_t i = 2; i < lines.size(); ++i) rest += " " + lines[i];
        lines.resize(2);
        lines[1] = f_.r13.elide(rest, box.w - 24);
    }
    for (size_t i = 0; i < lines.size(); ++i)
        text(f_.r13, lines[i], box.x + 12, box.y + 10 + ch + 4 + i * lh, kCodeText);

    if (pointer_ != hand_shown_) {
        hand_shown_ = pointer_;
        SDL_SetCursor(pointer_ ? hand_ : arrow_);
    }
}

LauncherResult Launcher::run() {
    if (!open_window()) return LauncherResult::NoWindow;
    FG_LOG("launcher: %s video, %s renderer", SDL_GetCurrentVideoDriver(), SDL_GetRendererName(ren_));
    fit_to_display();
    if (!load_fonts()) {
        FG_LOG("launcher: cannot prepare its fonts");
        SDL_DestroyRenderer(ren_);
        SDL_DestroyWindow(win_);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return LauncherResult::NoWindow;
    }
    SDL_SetRenderVSync(ren_, 1);
    arrow_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    hand_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
    SDL_ShowWindow(win_);
    float shown_scale = scale_;

    // For checking the look without a person: FG_LAUNCHER_SHOTS=PREFIX draws
    // each tab once into PREFIX-N.png and quits (e.g. with
    // SDL_VIDEO_DRIVER=offscreen).
    if (const char* shots = std::getenv("FG_LAUNCHER_SHOTS")) {
        for (tab_ = 0; tab_ < 4; ++tab_) {
            SDL_RenderClear(ren_);
            frame();
            if (SDL_Surface* sf = SDL_RenderReadPixels(ren_, nullptr)) {
                if (SDL_Surface* c = SDL_ConvertSurface(sf, SDL_PIXELFORMAT_XRGB8888)) {
                    Frame fr;
                    fr.width = c->w;
                    fr.height = c->h;
                    fr.pixels.resize(size_t(c->w) * c->h);
                    for (int y = 0; y < c->h; ++y)
                        std::memcpy(&fr.pixels[size_t(y) * c->w], static_cast<uint8_t*>(c->pixels) + y * c->pitch,
                                    size_t(c->w) * 4);
                    write_png(std::string(shots) + "-" + std::to_string(tab_) + ".png", fr);
                    SDL_DestroySurface(c);
                }
                SDL_DestroySurface(sf);
            }
            SDL_RenderPresent(ren_);
        }
        done_ = true;
    }

    while (!done_) {
        pressed_ = released_ = false;
        SDL_Event e;
        bool any = SDL_WaitEventTimeout(&e, 100);
        while (any) {
            SDL_ConvertEventToRenderCoordinates(ren_, &e);
            switch (e.type) {
            case SDL_EVENT_QUIT: case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                result_ = LauncherResult::Quit;
                done_ = true;
                break;
            case SDL_EVENT_MOUSE_MOTION: mx_ = e.motion.x; my_ = e.motion.y; break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (e.button.button == SDL_BUTTON_LEFT) { pressed_ = down_ = true; mx_ = e.button.x; my_ = e.button.y; }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (e.button.button == SDL_BUTTON_LEFT) { released_ = true; down_ = false; mx_ = e.button.x; my_ = e.button.y; }
                break;
            case SDL_EVENT_WINDOW_MOUSE_LEAVE: mx_ = my_ = -1; break;
            case SDL_EVENT_KEY_DOWN:
                if (dialog_ || e.key.repeat) break;
                if (e.key.key == SDLK_ESCAPE) { result_ = LauncherResult::Quit; done_ = true; }
                else if ((e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) && !game_dir_.empty()) {
                    result_ = LauncherResult::Launch;
                    done_ = true;
                } else if (e.key.key == SDLK_TAB && (e.key.mod & SDL_KMOD_CTRL)) {
                    tab_ = (tab_ + ((e.key.mod & SDL_KMOD_SHIFT) ? 3 : 1)) % 4;
                }
                break;
            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: fit_to_display(); break;
            default: break;
            }
            // A press and its release in one batch: handle the press first.
            if (pressed_ && released_) break;
            any = SDL_PollEvent(&e);
        }
        if (done_) break;
        if (scale_ != shown_scale) {
            shown_scale = scale_;
            load_fonts();
        }
        SDL_SetRenderDrawColor(ren_, kFrameDark.r, kFrameDark.g, kFrameDark.b, 255);
        SDL_RenderClear(ren_);
        frame();
        if (released_) active_ = 0;
        SDL_RenderPresent(ren_);
    }

    if (dialog_) (void)dialog_.release();  // SDL may still call back into it: leak it
    if (result_ == LauncherResult::Launch && game_picked_) remember_game_dir(game_dir_);
    for (LauncherFont* f : {&f_.title, &f_.tab, &f_.label, &f_.launch, &f_.caption, &f_.r12, &f_.r13, &f_.r14,
                            &f_.sb13, &f_.sb14})
        f->release();
    SDL_DestroyCursor(arrow_);
    SDL_DestroyCursor(hand_);
    SDL_DestroyRenderer(ren_);
    SDL_DestroyWindow(win_);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return result_;
}

}  // namespace

LauncherResult run_launcher(Settings& s, std::string& game_dir) {
    Launcher l(s, game_dir);
    return l.run();
}

}  // namespace fg
