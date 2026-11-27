// SPDX-License-Identifier: GPL-2.0-or-later
#include "game_locate.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

#include "log.h"

namespace fg {

namespace {

constexpr const char* kGogGameId = "1430136345";

bool has_game(const std::string& d) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(d + "/EXE/BARENA.EXE"), ec);
}

std::string remembered_file() {
    char* pref = SDL_GetPrefPath("", "fg-runner");
    if (!pref) return "";
    std::string f = std::string(pref) + "game-dir.txt";
    SDL_free(pref);
    return f;
}

std::string remembered_game_dir() {
    std::string f = remembered_file();
    if (f.empty()) return "";
    std::ifstream in(std::filesystem::path(f), std::ios::binary);
    std::string dir;
    std::getline(in, dir);
    while (!dir.empty() && (dir.back() == '\r' || dir.back() == '\n')) dir.pop_back();
    return dir;
}

#ifdef _WIN32
// GOG's installer and Galaxy record the install under
// HKLM\SOFTWARE\GOG.com\Games\<id> (32-bit view: WOW6432Node on 64-bit
// Windows): "path" is the install folder; the game's own script also writes
// DOSBOXFOLDER = <install>\DOSBox. The UTF-8 manifest makes the A calls UTF-8.
std::string registry_value(const char* name) {
    std::string key = std::string("SOFTWARE\\GOG.com\\Games\\") + kGogGameId;
    HKEY h;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ | KEY_WOW64_32KEY, &h) != ERROR_SUCCESS)
        return "";
    char buf[1024];
    DWORD type = 0, size = sizeof(buf) - 1;
    std::string out;
    if (RegQueryValueExA(h, name, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS &&
        type == REG_SZ) {
        buf[size] = '\0';
        out = buf;
    }
    RegCloseKey(h);
    return out;
}
#endif

std::vector<std::string> candidate_roots() {
    std::vector<std::string> roots;
    if (const char* base = SDL_GetBasePath()) {  // next to the runner, or one level up
        roots.push_back(std::string(base) + ".");
        roots.push_back(std::string(base) + "..");
    }
#ifdef _WIN32
    if (std::string p = registry_value("path"); !p.empty()) roots.push_back(p);
    if (std::string p = registry_value("DOSBOXFOLDER"); !p.empty()) roots.push_back(p + "/..");
    roots.push_back("C:/GOG Games/Fantasy General");
    roots.push_back("C:/Program Files (x86)/GOG Galaxy/Games/Fantasy General");
    roots.push_back("C:/Program Files/GOG Galaxy/Games/Fantasy General");
    roots.push_back("C:/Program Files (x86)/GOG.com/Fantasy General");
#else
    const char* h = std::getenv("HOME");
    std::string home = h ? h : ".";
    roots.push_back(home + "/GOG Games/Fantasy General");       // GOG's Linux installer
    roots.push_back(home + "/Games/Heroic/Fantasy General");     // Heroic Games Launcher
    roots.push_back(home + "/Games/fantasy-general");            // Lutris
    roots.push_back(home + "/Games/gog/fantasy-general");        // Lutris, GOG source
    roots.push_back(home + "/.wine/drive_c/GOG Games/Fantasy General");  // Windows installer in Wine
#endif
    return roots;
}

// Case-insensitive lookup of one entry in a directory ("" if missing).
std::string entry_in(const std::string& dir, const std::string& name) {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(std::filesystem::path(dir), ec)) {
        std::string n = e.path().filename().string();
        if (SDL_strcasecmp(n.c_str(), name.c_str()) == 0) return dir + "/" + n;
    }
    return "";
}

// Can a file be created in `dir`? (Tried for real: permissions, ACLs and
// read-only media all show up this way.)
bool writable(const std::string& dir) {
    if (dir.empty()) return false;
    std::filesystem::path probe = std::filesystem::path(dir) / "fg-runner-write-test.tmp";
    {
        std::ofstream f(probe, std::ios::binary);
        if (!f || !(f << 'x') || !f.flush()) return false;
    }
    std::error_code ec;
    std::filesystem::remove(probe, ec);
    return true;
}

}  // namespace

std::string choose_write_overlay(const std::string& game_dir) {
    std::string cloud = entry_in(game_dir, "cloud_saves");
    if (!cloud.empty() && writable(cloud)) return cloud;
    if (writable(entry_in(game_dir, "SAVES"))) return "";
    char* pref = SDL_GetPrefPath("", "fg-runner");
    if (!pref) return "";
    std::string dir = std::string(pref) + "drive-c";
    SDL_free(pref);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(dir), ec);
    if (ec) FG_LOG("cannot create %s: %s", dir.c_str(), ec.message().c_str());
    return dir;
}

std::string game_dir_in(const std::string& root) {
    if (root.empty()) return "";
    for (const char* sub : {"", "/game/data", "/data", "/game"})
        if (has_game(root + sub)) return root + sub;
    return "";
}

std::string locate_game_dir() {
    if (const char* e = std::getenv("FG_GAME_DIR")) return game_dir_in(e);
    if (std::string d = game_dir_in(remembered_game_dir()); !d.empty()) return d;
    for (const auto& r : candidate_roots())
        if (std::string d = game_dir_in(r); !d.empty()) return d;
    return "";
}

void remember_game_dir(const std::string& dir) {
    std::string f = remembered_file();
    if (f.empty()) return;
    std::ofstream out(std::filesystem::path(f), std::ios::binary | std::ios::trunc);
    out << dir << '\n';
    if (!out) FG_LOG("cannot write %s", f.c_str());
}

std::string pick_game_dir() {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        FG_LOG("SDL video: %s", SDL_GetError());
        return "";
    }
    const char* title = "Fantasy General";
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, title,
                             "Fantasy General was not found.\n\n"
                             "Install it from GOG.com first (the Windows or the Linux installer), "
                             "then choose the folder it was installed to.",
                             nullptr);
    std::string result;
    for (;;) {
        struct Pick {
            std::atomic<bool> done{false};
            bool cancelled = false;
            std::string path;
        } pick;
        SDL_ShowOpenFolderDialog(
            [](void* ud, const char* const* files, int) {
                auto* p = static_cast<Pick*>(ud);
                if (!files) FG_LOG("folder dialog: %s", SDL_GetError());
                if (!files || !files[0]) p->cancelled = true;
                else p->path = files[0];
                p->done = true;
            },
            &pick, nullptr, nullptr, false);
        while (!pick.done) {  // the dialog reports back through the event loop
            SDL_Event ev;
            SDL_WaitEventTimeout(&ev, 50);
        }
        if (pick.cancelled) break;
        result = game_dir_in(pick.path);
        if (!result.empty()) break;
        FG_LOG("no EXE\\BARENA.EXE in %s", pick.path.c_str());
        std::string msg = "No Fantasy General files (EXE\\BARENA.EXE) in\n" + pick.path +
                          "\n\nChoose the folder the GOG installer installed the game to.";
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, title, msg.c_str(), nullptr);
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return result;
}

}  // namespace fg
