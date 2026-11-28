// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <thread>
#include <vector>

#include "input.h"
#include "log.h"
#include "png.h"
#include "video.h"

namespace fg {

namespace {

bool take_screenshot(const std::string& path) {
    Frame f;
    g_video.snapshot(f);
    if (!write_png(path, f)) { FG_LOG("cannot write %s", path.c_str()); return false; }
    FG_LOG("screenshot %dx%d -> %s", f.width, f.height, path.c_str());
    return true;
}

// SDL key -> BIOS keyboard code ((PC scan code << 8) | ASCII), 0 if unmapped.
uint16_t bios_key(const SDL_KeyboardEvent& k) {
    bool shift = k.mod & SDL_KMOD_SHIFT;
    SDL_Keycode c = k.key;
    struct Special { SDL_Keycode key; uint16_t code; };
    static const Special specials[] = {
        {SDLK_ESCAPE, 0x011B}, {SDLK_RETURN, 0x1C0D}, {SDLK_KP_ENTER, 0x1C0D}, {SDLK_BACKSPACE, 0x0E08},
        {SDLK_TAB, 0x0F09}, {SDLK_SPACE, 0x3920}, {SDLK_UP, 0x4800}, {SDLK_DOWN, 0x5000},
        {SDLK_LEFT, 0x4B00}, {SDLK_RIGHT, 0x4D00}, {SDLK_HOME, 0x4700}, {SDLK_END, 0x4F00},
        {SDLK_PAGEUP, 0x4900}, {SDLK_PAGEDOWN, 0x5100}, {SDLK_INSERT, 0x5200}, {SDLK_DELETE, 0x5300},
        {SDLK_F1, 0x3B00}, {SDLK_F2, 0x3C00}, {SDLK_F3, 0x3D00}, {SDLK_F4, 0x3E00}, {SDLK_F5, 0x3F00},
        {SDLK_F6, 0x4000}, {SDLK_F7, 0x4100}, {SDLK_F8, 0x4200}, {SDLK_F9, 0x4300}, {SDLK_F10, 0x4400},
    };
    for (const auto& sp : specials) if (sp.key == c) return sp.code;
    // Letters and digits: PC set-1 scan codes.
    static const char row_q[] = "qwertyuiop", row_a[] = "asdfghjkl", row_z[] = "zxcvbnm";
    auto ascii = [&](char ch) { return static_cast<uint16_t>(shift && ch >= 'a' && ch <= 'z' ? ch - 32 : ch); };
    for (int i = 0; row_q[i]; ++i) if (c == SDL_Keycode(row_q[i])) return uint16_t(((0x10 + i) << 8) | ascii(row_q[i]));
    for (int i = 0; row_a[i]; ++i) if (c == SDL_Keycode(row_a[i])) return uint16_t(((0x1E + i) << 8) | ascii(row_a[i]));
    for (int i = 0; row_z[i]; ++i) if (c == SDL_Keycode(row_z[i])) return uint16_t(((0x2C + i) << 8) | ascii(row_z[i]));
    if (c >= SDLK_1 && c <= SDLK_9) return uint16_t(((0x02 + (c - SDLK_1)) << 8) | ('1' + (c - SDLK_1)));
    if (c == SDLK_0) return 0x0B30;
    return 0;
}

struct ScriptEvent {
    double time;
    std::string action;
    std::string arg;
    int x = 0, y = 0;
};

// Arrow keys by position (scancode), as Input scroll bits; 0 for other keys.
uint8_t scroll_bit(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_UP: return Input::kScrollUp;
    case SDL_SCANCODE_DOWN: return Input::kScrollDown;
    case SDL_SCANCODE_LEFT: return Input::kScrollLeft;
    case SDL_SCANCODE_RIGHT: return Input::kScrollRight;
    default: return 0;
    }
}

uint8_t named_scroll_bit(const std::string& n) {
    return n == "up" ? Input::kScrollUp : n == "down" ? Input::kScrollDown : n == "left" ? Input::kScrollLeft
         : n == "right" ? Input::kScrollRight : 0;
}

uint16_t named_key(const std::string& n) {
    static const struct { const char* name; uint16_t code; } keys[] = {
        {"esc", 0x011B}, {"enter", 0x1C0D}, {"space", 0x3920}, {"tab", 0x0F09}, {"backspace", 0x0E08},
        {"up", 0x4800}, {"down", 0x5000}, {"left", 0x4B00}, {"right", 0x4D00},
        {"f1", 0x3B00}, {"f2", 0x3C00}, {"f3", 0x3D00}, {"f4", 0x3E00}, {"f5", 0x3F00},
        {"f6", 0x4000}, {"f7", 0x4100}, {"f8", 0x4200}, {"f9", 0x4300}, {"f10", 0x4400},
    };
    for (const auto& k : keys) if (n == k.name) return k.code;
    if (n.size() == 1) {
        SDL_KeyboardEvent e{};
        e.key = static_cast<SDL_Keycode>(n[0] >= 'A' && n[0] <= 'Z' ? n[0] + 32 : n[0]);
        if (n[0] >= 'A' && n[0] <= 'Z') e.mod = SDL_KMOD_SHIFT;
        return bios_key(e);
    }
    return 0;
}

std::vector<ScriptEvent> parse_events(const std::string& text) {
    std::vector<ScriptEvent> out;
    std::stringstream all(text);
    std::string item;
    while (std::getline(all, item, ';')) {
        std::stringstream ss(item);
        ScriptEvent e;
        if (!(ss >> e.time >> e.action)) continue;
        if (e.action == "key" || e.action == "shot" || e.action == "hold" || e.action == "release") ss >> e.arg;
        else ss >> e.x >> e.y;
        out.push_back(e);
    }
    std::stable_sort(out.begin(), out.end(), [](const ScriptEvent& a, const ScriptEvent& b) { return a.time < b.time; });
    return out;
}

// Feed due scripted events into the emulated devices.
void run_script(std::vector<ScriptEvent>& events, size_t& next, double now) {
    while (next < events.size() && events[next].time <= now) {
        const ScriptEvent& e = events[next++];
        FG_LOG("script %.1fs: %s %s %d %d", e.time, e.action.c_str(), e.arg.c_str(), e.x, e.y);
        if (e.action == "key") {
            if (uint16_t c = named_key(e.arg)) g_input.key(c);
            else FG_LOG("unknown key name %s", e.arg.c_str());
        } else if (e.action == "hold" || e.action == "release") {  // arrow held down / let go
            if (uint8_t bit = named_scroll_bit(e.arg)) g_input.scroll_key(bit, e.action == "hold");
            else FG_LOG("hold/release takes up, down, left or right, not %s", e.arg.c_str());
        } else if (e.action == "shot") {
            take_screenshot(e.arg);
        } else if (e.action == "move") {
            g_input.mouse_moved(e.x, e.y);
        } else if (e.action == "click" || e.action == "rclick") {
            int b = e.action == "click" ? 0 : 1;
            g_input.mouse_moved(e.x, e.y);
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            g_input.mouse_button(b, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            g_input.mouse_button(b, false);
        }
    }
}

}  // namespace

bool run_ui(const UiOptions& opt, const std::atomic<bool>& game_done, const std::function<void()>& on_ready) {
    using clock = std::chrono::steady_clock;
    auto start = clock::now();
    auto elapsed = [&] { return std::chrono::duration<double>(clock::now() - start).count(); };
    bool want_shot = !opt.screenshot.empty();
    std::vector<ScriptEvent> script = parse_events(opt.events);
    size_t script_next = 0;

    if (opt.headless) {
        on_ready();
        while (!game_done) {
            run_script(script, script_next, elapsed());
            if (want_shot && elapsed() >= opt.screenshot_after) { take_screenshot(opt.screenshot); return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (want_shot) take_screenshot(opt.screenshot);
        return true;
    }

    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    auto open_window = [&](const char* video_driver) {
        if (video_driver) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, video_driver);
        SDL_SetHint(SDL_HINT_APP_ID, "fantasy-general-runner");       // Wayland: matches the desktop entry
        SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");           // the .exe's icon (fg-runner.rc)
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) { FG_LOG("SDL video: %s", SDL_GetError()); return false; }
        // High pixel density: on a scaled desktop, draw into real screen
        // pixels rather than letting the compositor stretch the window.
        if (SDL_CreateWindowAndRenderer("Fantasy General", 640 * opt.scale, 480 * opt.scale,
                                        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &win, &ren))
            return true;
        FG_LOG("no usable renderer with %s video (%s)%s", SDL_GetCurrentVideoDriver(), SDL_GetError(),
               video_driver ? "" : "; trying X11 software rendering");
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    };
    // The runner is a 32-bit program: without 32-bit GL/Vulkan drivers the
    // Wayland backend has no usable renderer, but X11 (XWayland) still offers
    // a software framebuffer.
    if (!open_window(nullptr) && !open_window("x11")) return false;
    FG_LOG("window: %s video, %s renderer", SDL_GetCurrentVideoDriver(), SDL_GetRendererName(ren));
    // The game draws its own pointer; hide the desktop's over our window.
    SDL_HideCursor();
    // Window size in screen pixels: k x 640x480 (the window size itself is in
    // desktop units, which differ from pixels on a scaled desktop).
    auto set_window_pixels = [&](int k) {
        float d = SDL_GetWindowPixelDensity(win);
        if (d <= 0) d = 1;
        SDL_SetWindowSize(win, int(640 * k / d + 0.5f), int(480 * k / d + 0.5f));
    };
    // Largest multiple that fits the desktop's usable area.
    auto max_window_multiple = [&] {
        SDL_Rect r;
        if (!SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(win), &r)) return 16;
        float d = SDL_GetWindowPixelDensity(win);
        if (d <= 0) d = 1;
        return std::max(1, std::min(int(r.w * d) / 640, int(r.h * d) / 480));
    };
    // Ctrl+F11 switches between integer and fit scaling while running.
    bool integer_scale = opt.integer_scale;
    if (integer_scale) set_window_pixels(std::min(opt.scale, max_window_multiple()));
    bool fullscreen = opt.fullscreen;
    if (fullscreen) SDL_SetWindowFullscreen(win, true);
    // After the user resizes the window, snap it to a multiple of 640x480 once
    // the resizing has stopped. If the window manager refuses the size, leave it.
    clock::time_point snap_at{};
    bool snap_pending = false;
    int refused_w = -1, refused_h = -1;
    std::string shown_scale;
    // Mouse capture: while captured, the (hidden) desktop pointer is confined
    // to the game image. Otherwise it wanders into the black border or off
    // the window while the game's own cursor stays pinned at the edge (e.g.
    // when scrolling the battle map), and moving back takes a while to have
    // any effect. Fullscreen captures on focus; a window captures on a click
    // into the image. Ctrl+F10 toggles; losing focus releases.
    bool captured = fullscreen;
    SDL_Rect mouse_rect{-1, -1, -1, -1};  // confinement in effect (w < 0: none)
    std::string shown_title;
    SDL_SetRenderVSync(ren, 1);
    SDL_Texture* tex = nullptr;
    int tex_w = 0, tex_h = 0;
    // 640x400 and 320x200 are shown at 4:3 like a CRT would.
    auto set_presentation = [&] {
        SDL_SetRenderLogicalPresentation(ren, tex_w, tex_w * 3 / 4,
                                         integer_scale ? SDL_LOGICAL_PRESENTATION_INTEGER_SCALE
                                                       : SDL_LOGICAL_PRESENTATION_LETTERBOX);
    };
    Frame frame;
    std::vector<uint32_t> uploaded;  // pixels last copied into `tex`
    // Without working vsync (some drivers, software rendering, hidden
    // windows) SDL_RenderPresent returns at once and this loop would spin a
    // core: never present more often than the display refreshes.
    auto last_present = clock::now();
    auto frame_interval = [&] {
        const SDL_DisplayMode* dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win));
        float hz = dm && dm->refresh_rate > 1 ? dm->refresh_rate : 60.0f;
        return std::chrono::nanoseconds(int64_t(1e9 / hz));
    };

    bool running = true;
    bool started = false;
    while (running && !game_done) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            SDL_ConvertEventToRenderCoordinates(ren, &e);
            switch (e.type) {
            case SDL_EVENT_QUIT: running = false; break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                captured = false;
                g_input.release_scroll_keys();  // the key-up would go to another window
                break;
            case SDL_EVENT_RENDER_TARGETS_RESET: case SDL_EVENT_RENDER_DEVICE_RESET:
                // Texture contents (device reset: the textures) are gone; upload again.
                if (e.type == SDL_EVENT_RENDER_DEVICE_RESET) {
                    if (tex) SDL_DestroyTexture(tex);
                    tex = nullptr;
                    tex_w = tex_h = 0;
                }
                uploaded.clear();
                break;
            case SDL_EVENT_KEY_UP:
                if (uint8_t bit = scroll_bit(e.key.scancode)) g_input.scroll_key(bit, false);
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED: if (fullscreen) captured = true; break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                snap_pending = true;
                snap_at = clock::now() + std::chrono::milliseconds(300);
                break;
            case SDL_EVENT_KEY_DOWN:
                // Held arrows scroll the battle map (traps.cpp); the keys still reach the game.
                if (uint8_t bit = scroll_bit(e.key.scancode)) g_input.scroll_key(bit, true);
                if (e.key.key == SDLK_F12) { take_screenshot("fg-screenshot.png"); break; }
                if ((e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) && (e.key.mod & SDL_KMOD_ALT)) {
                    if (!e.key.repeat) {
                        fullscreen = !fullscreen;
                        SDL_SetWindowFullscreen(win, fullscreen);  // borderless desktop fullscreen
                        if (fullscreen) captured = true;
                    }
                    break;  // not passed to the game
                }
                if (e.key.key == SDLK_F10 && (e.key.mod & SDL_KMOD_CTRL)) {
                    if (!e.key.repeat) captured = !captured;
                    break;  // not passed to the game
                }
                if (e.key.key == SDLK_F11 && (e.key.mod & SDL_KMOD_CTRL)) {
                    if (!e.key.repeat) {
                        integer_scale = !integer_scale;
                        if (tex_w) set_presentation();
                        // Back to integer in a window: snap it to a multiple of 640x480 now.
                        if (integer_scale) {
                            snap_pending = true;
                            snap_at = clock::now();
                            refused_w = refused_h = -1;
                        }
                    }
                    break;  // not passed to the game
                }
                if (uint16_t code = bios_key(e.key)) g_input.key(code);
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (tex_w) g_input.mouse_moved(int(e.motion.x), int(e.motion.y * tex_h / (tex_w * 3 / 4)), tex_w, tex_h);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
                int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : 2;
                // A click into the image captures the mouse (and still reaches the game).
                if (!captured && e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && tex_w && e.button.x >= 0 &&
                    e.button.y >= 0 && e.button.x < tex_w && e.button.y < tex_w * 3 / 4)
                    captured = true;
                if (tex_w) g_input.mouse_moved(int(e.button.x), int(e.button.y * tex_h / (tex_w * 3 / 4)), tex_w, tex_h);
                g_input.mouse_button(b, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                break;
            }
            default: break;
            }
        }
        if (snap_pending && clock::now() >= snap_at) {
            snap_pending = false;
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(win, &pw, &ph);
            bool maximized = SDL_GetWindowFlags(win) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN);
            int k = std::max(1, int(std::min(pw / 640.0, ph / 480.0) + 0.5));
            k = std::min(k, max_window_multiple());
            if (integer_scale && !maximized && (pw != 640 * k || ph != 480 * k)) {
                if (pw == refused_w && ph == refused_h) {
                    refused_w = refused_h = -1;  // the window manager kept its size: accept it
                } else {
                    refused_w = pw;
                    refused_h = ph;
                    set_window_pixels(k);
                }
            }
        }
        run_script(script, script_next, elapsed());
        g_video.snapshot(frame);
        if (frame.width != tex_w || frame.height != tex_h) {
            if (tex) SDL_DestroyTexture(tex);
            tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, frame.width,
                                    frame.height);
            SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
            tex_w = frame.width;
            tex_h = frame.height;
            set_presentation();
            uploaded.clear();
        }
        {  // Show the current scale in the title bar (and log it).
            int ow = 0, oh = 0;
            SDL_GetCurrentRenderOutputSize(ren, &ow, &oh);
            double sx = double(ow) / tex_w, sy = double(oh) / (tex_w * 3 / 4);
            char buf[96];
            if (integer_scale)
                std::snprintf(buf, sizeof buf, "%dx, exact pixels", std::max(1, int(std::min(sx, sy))));
            else
                std::snprintf(buf, sizeof buf, "%.2fx, uneven pixels", std::min(sx, sy));
            if (tex_h != tex_w * 3 / 4) std::snprintf(buf, sizeof buf, "%dx%d stretched to 4:3", tex_w, tex_h);
            if (shown_scale != buf) {
                shown_scale = buf;
                FG_LOG("display: %dx%d output, %s", ow, oh, buf);
            }
            std::string title = "Fantasy General (" + shown_scale + ")" +
                                (captured ? " - Ctrl+F10 releases mouse" : " - click to capture mouse");
            if (title != shown_title) {
                shown_title = title;
                SDL_SetWindowTitle(win, title.c_str());
            }
        }
        {  // Apply the mouse capture (see `captured`).
            SDL_Rect r{-1, -1, -1, -1};
            SDL_FRect img;
            if (captured && SDL_GetRenderLogicalPresentationRect(ren, &img)) {
                float d = SDL_GetWindowPixelDensity(win);
                if (d <= 0) d = 1;
                int x0 = int(std::ceil(img.x / d)), y0 = int(std::ceil(img.y / d));
                int x1 = int(std::floor((img.x + img.w) / d)), y1 = int(std::floor((img.y + img.h) / d));
                r = SDL_Rect{x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0)};
            }
            if (r.x != mouse_rect.x || r.y != mouse_rect.y || r.w != mouse_rect.w || r.h != mouse_rect.h) {
                mouse_rect = r;
                if (!SDL_SetWindowMouseRect(win, r.w < 0 ? nullptr : &r))
                    FG_LOG("cannot %s the pointer: %s", r.w < 0 ? "release" : "confine", SDL_GetError());
            }
        }
        if (frame.pixels != uploaded) {  // most frames show the same image
            SDL_UpdateTexture(tex, nullptr, frame.pixels.data(), frame.width * 4);
            uploaded = frame.pixels;
        }
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_FRect dst{0, 0, float(frame.width), float(frame.width * 3 / 4)};
        SDL_RenderTexture(ren, tex, nullptr, &dst);
        SDL_RenderPresent(ren);
        {
            auto next = last_present + frame_interval();
            if (clock::now() < next - std::chrono::milliseconds(1)) std::this_thread::sleep_until(next);
            last_present = clock::now();
        }
        if (!started) {  // first frame is on screen: start the game now
            started = true;
            start = clock::now();  // --screenshot-after / --events count from here
            on_ready();
        }
        if (want_shot && elapsed() >= opt.screenshot_after) {
            take_screenshot(opt.screenshot);
            running = false;
        }
    }
    if (tex) SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return true;
}

}  // namespace fg
