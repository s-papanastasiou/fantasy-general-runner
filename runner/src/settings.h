// SPDX-License-Identifier: GPL-2.0-or-later
// The player's settings as the launcher keeps them: <pref>/fg-runner/settings.ini
// (~/.local/share/fg-runner/, %APPDATA%\fg-runner\). Each one is a
// command-line option; the file is read only when the launcher is shown
// (no options, or --launcher), so command-line runs behave as before.
#pragma once
#include <string>
#include <vector>

namespace fg {

struct Settings {
    std::string save_dir;          // --save-dir; "" = automatic (choose_write_overlay)
    bool fullscreen = true;        // --window when false
    int scale = 2;                 // --scale
    bool integer_scale = true;     // --scaling integer|fit
    int music_volume = 100;        // --music-volume, 10..100
    bool sound = true;             // --no-sound when false
    bool original_scrolling = false;  // --original-scrolling
    int scroll_ms = 120;           // --scroll-ms
    int ai_move_ms = 10;           // --ai-move-ms
    int ai_view_ms = 200;          // --ai-view-ms

    bool operator==(const Settings&) const = default;
};

// The saved settings; defaults for anything missing or unreadable.
Settings load_settings();
bool save_settings(const Settings& s);

// The command-line options that give the same run: one per setting that
// differs from the default (paths quoted when they hold spaces).
std::vector<std::string> settings_args(const Settings& s);

}  // namespace fg
