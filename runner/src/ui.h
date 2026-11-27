// SPDX-License-Identifier: GPL-2.0-or-later
// The main-thread UI: SDL3 window presenting the emulated display.
#pragma once
#include <atomic>
#include <functional>
#include <string>

namespace fg {

struct UiOptions {
    bool headless = false;          // no window (screenshots only)
    std::string screenshot;         // write a PNG of the screen here...
    double screenshot_after = 0;    // ...this many seconds after start, then quit
    int scale = 2;                  // initial window scale
    // Integer scaling: each game pixel becomes an exact N x N block of screen
    // pixels (black borders fill the rest), and the window snaps to multiples
    // of 640x480 after a resize. Otherwise the picture fills the window at 4:3
    // with uneven pixel sizes.
    bool integer_scale = true;
    bool fullscreen = true;         // start in (desktop) fullscreen (--window: not); Alt+Enter toggles
    // Scripted input for automated runs: "TIME ACTION; ..." where ACTION is
    // "key NAME" (esc, enter, space, tab, f1..f10, up/down/left/right, a letter
    // or digit), "click X Y", "rclick X Y" or "move X Y". TIME in seconds.
    std::string events;
};

// Runs until the window is closed, the screenshot is taken, or `game_done`
// becomes true. `on_ready` is called once the window is on screen (at once
// when headless), so the game starts only when its output can be seen.
// Returns false if the window could not be created.
bool run_ui(const UiOptions& opt, const std::atomic<bool>& game_done, const std::function<void()>& on_ready);

}  // namespace fg
