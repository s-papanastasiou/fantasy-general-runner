// SPDX-License-Identifier: GPL-2.0-or-later
#include "settings.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "log.h"

namespace fg {

namespace {

std::string settings_file() {
    char* pref = SDL_GetPrefPath("", "fg-runner");
    if (!pref) return "";
    std::string f = std::string(pref) + "settings.ini";
    SDL_free(pref);
    return f;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

int to_int(const std::string& v, int lo, int hi, int fallback) {
    char* end = nullptr;
    long n = std::strtol(v.c_str(), &end, 10);
    if (v.empty() || *end) return fallback;
    return int(std::clamp<long>(n, lo, hi));
}

std::string quoted(const std::string& s) {
    return s.find_first_of(" \t'\"") == std::string::npos ? s : "\"" + s + "\"";
}

}  // namespace

Settings load_settings() {
    Settings s;
    std::string f = settings_file();
    if (f.empty()) return s;
    std::ifstream in(std::filesystem::path(f), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "save-dir") s.save_dir = v;
        else if (k == "fullscreen") s.fullscreen = v != "false";
        else if (k == "scale") s.scale = to_int(v, 1, 16, s.scale);
        else if (k == "scaling") s.integer_scale = v != "fit";
        else if (k == "crt") s.crt = v == "true";
        else if (k == "music-volume") s.music_volume = to_int(v, 10, 100, s.music_volume);
        else if (k == "sound") s.sound = v != "false";
        else if (k == "scrolling") s.original_scrolling = v == "original";
        else if (k == "scroll-ms") s.scroll_ms = to_int(v, 0, 10000, s.scroll_ms);
        else if (k == "ai-move-ms") s.ai_move_ms = to_int(v, 0, 10000, s.ai_move_ms);
        else if (k == "ai-view-ms") s.ai_view_ms = to_int(v, 0, 100000, s.ai_view_ms);
    }
    return s;
}

bool save_settings(const Settings& s) {
    std::string f = settings_file();
    if (f.empty()) return false;
    std::ofstream out(std::filesystem::path(f), std::ios::binary | std::ios::trunc);
    out << "# Fantasy General runner: the launcher's settings (each is a command-line option)\n"
        << "save-dir = " << s.save_dir << "\n"
        << "fullscreen = " << (s.fullscreen ? "true" : "false") << "\n"
        << "scale = " << s.scale << "\n"
        << "scaling = " << (s.integer_scale ? "integer" : "fit") << "\n"
        << "crt = " << (s.crt ? "true" : "false") << "\n"
        << "music-volume = " << s.music_volume << "\n"
        << "sound = " << (s.sound ? "true" : "false") << "\n"
        << "scrolling = " << (s.original_scrolling ? "original" : "eight-way") << "\n"
        << "scroll-ms = " << s.scroll_ms << "\n"
        << "ai-move-ms = " << s.ai_move_ms << "\n"
        << "ai-view-ms = " << s.ai_view_ms << "\n";
    if (!out) {
        FG_LOG("cannot write %s", f.c_str());
        return false;
    }
    return true;
}

std::vector<std::string> settings_args(const Settings& s) {
    const Settings d;
    std::vector<std::string> a;
    if (s.save_dir != d.save_dir) a.push_back("--save-dir " + quoted(s.save_dir));
    if (s.fullscreen != d.fullscreen) a.push_back(s.fullscreen ? "--fullscreen" : "--window");
    if (s.scale != d.scale) a.push_back("--scale " + std::to_string(s.scale));
    if (s.integer_scale != d.integer_scale) a.push_back(s.integer_scale ? "--scaling integer" : "--scaling fit");
    if (s.crt != d.crt) a.push_back("--crt");
    if (s.music_volume != d.music_volume) a.push_back("--music-volume " + std::to_string(s.music_volume));
    if (s.sound != d.sound) a.push_back("--no-sound");
    if (s.original_scrolling != d.original_scrolling) a.push_back("--original-scrolling");
    if (s.scroll_ms != d.scroll_ms) a.push_back("--scroll-ms " + std::to_string(s.scroll_ms));
    if (s.ai_move_ms != d.ai_move_ms) a.push_back("--ai-move-ms " + std::to_string(s.ai_move_ms));
    if (s.ai_view_ms != d.ai_view_ms) a.push_back("--ai-view-ms " + std::to_string(s.ai_view_ms));
    return a;
}

}  // namespace fg
