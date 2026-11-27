// SPDX-License-Identifier: GPL-2.0-or-later
// Finding the GOG install of Fantasy General: the "game directory" is the
// DOS directory that holds EXE\BARENA.EXE (on Windows the install folder
// itself, in the Linux installer's layout <install>/game/data).
#pragma once
#include <string>

namespace fg {

// The game directory inside `root` (root itself or one of the GOG
// sub-layouts), or "" if there is none.
std::string game_dir_in(const std::string& root);

// $FG_GAME_DIR, the remembered choice, next to the runner, the GOG registry
// key (Windows), then the default folders of GOG, Galaxy, Heroic, Lutris and
// Wine. "" if nothing is found.
std::string locate_game_dir();

// Asks the user for the folder with SDL's folder dialog until a valid one is
// chosen; "" if they cancel or no dialog can be shown. Needs a desktop.
std::string pick_game_dir();

// Saves the choice in the per-user folder for locate_game_dir().
void remember_game_dir(const std::string& dir);

// Where the game's writes (saves, settings) go: "" to write into the game
// directory itself, or a folder to overlay on drive C:. Like GOG's own
// DOSBox setup: <install>/cloud_saves if the install has one (Windows/Galaxy;
// Galaxy syncs it), otherwise the game directory if its SAVES folder is
// writable (Linux installer), otherwise <pref>/fg-runner/drive-c.
std::string choose_write_overlay(const std::string& game_dir);

}  // namespace fg
