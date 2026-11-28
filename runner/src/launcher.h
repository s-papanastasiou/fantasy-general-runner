// SPDX-License-Identifier: GPL-2.0-or-later
// The launcher: a window shown when the runner starts without options (a
// double-click, the desktop entry) or with --launcher. It shows where the
// game is, lets the player change the settings (each one a command-line
// option, shown as the equivalent command), and starts the game or quits.
#pragma once
#include <string>

#include "settings.h"

namespace fg {

enum class LauncherResult { Launch, Quit, NoWindow };

// `s` is edited in place. `game_dir`: the game directory found so far ("" if
// none); on Launch, the one to use (a folder the player picked is remembered
// for next time with remember_game_dir).
LauncherResult run_launcher(Settings& s, std::string& game_dir);

}  // namespace fg
