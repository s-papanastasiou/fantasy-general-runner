// SPDX-License-Identifier: GPL-2.0-or-later
// fg-runner: run Fantasy General's BARENA.EXE without DOS (natively on 32-bit
// Linux, or in the Unicorn CPU emulator; see cpu_backend.h).
//
//   fg-runner [--game-dir DIR] [--cd-dir DIR] [--save-dir DIR] [-- game arguments]
//
// DIR (the GOG install, or its DOS directory holding EXE\BARENA.EXE) is
// found by itself if left out, or asked for with a folder dialog
// (game_locate.h); $FG_GAME_DIR overrides the search.
// Writes to C: go to an overlay folder when the install has GOG's
// cloud_saves or is read-only (choose_write_overlay; --save-dir forces one).
// D: is the CD: --cd-dir should point at the extracted data track
// (tools/extract_cd.py); audio tracks come from DIR/MUSIC.
// Game arguments default to "n" (as in the GOG FG.BAT: no UVBELib driver).
// Set FG_TRACE=1 to log every emulated interrupt and file access.
#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "cd_audio.h"
#include "cd_extract.h"
#include "cdrom.h"
#include "cpu_backend.h"
#include "dig_driver.h"
#include "dos_fs.h"
#include "game_locate.h"
#include "log.h"
#include "machine.h"
#include "services.h"
#include "ui.h"

using namespace fg;

namespace {

// Drive D: when no --cd-dir is given: the CD image's data track, extracted
// once into the runner's per-user folder; failing that, the game directory.
std::string default_cd_dir(const std::string& game_dir) {
    std::string image = game_dir + "/game.gog";
    std::error_code ec;
    if (std::filesystem::exists(std::filesystem::path(image), ec)) {
        if (char* pref = SDL_GetPrefPath("", "fg-runner")) {
            std::string dir = std::string(pref) + "cd";
            SDL_free(pref);
            if (extract_cd_data_track(image, dir)) return dir;
        }
    }
    return game_dir;
}

void init_bios_data_area(AddressSpace& as) {
    as.write<uint16_t>(0x410, 0x0021);  // equipment: 80x25 colour, FPU-less flag irrelevant
    as.write<uint16_t>(0x413, 640);     // conventional memory KB
    as.write<uint8_t>(0x449, 0x03);     // current video mode
    as.write<uint16_t>(0x44A, 80);      // columns
    as.write<uint16_t>(0x463, 0x03D4);  // CRTC base port (colour)
    as.write<uint8_t>(0x484, 24);       // rows - 1
}

// PSP + environment as DOS/4GW presents them: ES = PSP selector at entry,
// PSP:2Ch = environment selector, command tail at PSP:80h.
bool build_psp(Machine& m, const std::string& args) {
    std::string env;
    env += std::string("PATH=C:\\") + '\0';
    env += std::string("COMSPEC=C:\\COMMAND.COM") + '\0';
    env += '\0';
    env += std::string("\x01\x00", 2);
    env += std::string("C:\\EXE\\BARENA.EXE") + '\0';

    uint16_t env_seg = m.as.conv_alloc((env.size() + 15) / 16);
    uint16_t psp_seg = m.as.conv_alloc(16);
    if (!env_seg || !psp_seg) return false;
    uint32_t env_lin = uint32_t(env_seg) << 4, psp_lin = uint32_t(psp_seg) << 4;
    std::memcpy(m.as.host(env_lin), env.data(), env.size());

    m.env_sel = m.ldt.make(env_lin, ((env.size() + 15) & ~15u) - 1, false);
    m.psp_sel = m.ldt.make(psp_lin, 0xFF, false);
    m.psp_seg = psp_seg;
    if (!m.env_sel || !m.psp_sel) return false;

    uint8_t* psp = m.as.host(psp_lin);
    psp[0] = 0xCD;
    psp[1] = 0x20;
    std::memcpy(psp + 2, "\xFF\x9F", 2);  // top of memory segment
    std::memcpy(psp + 0x2C, &m.env_sel, 2);
    std::string tail = args.empty() ? "" : " " + args;
    if (tail.size() > 126) tail.resize(126);
    psp[0x80] = static_cast<uint8_t>(tail.size());
    std::memcpy(psp + 0x81, tail.data(), tail.size());
    psp[0x81 + tail.size()] = '\r';
    return true;
}

#ifndef FG_VERSION
#define FG_VERSION "dev"
#endif

// The log file: <pref>/fg-runner.log (~/.local/share/fg-runner/,
// %APPDATA%\fg-runner\), rewritten each run.
void open_log() {
    if (char* pref = SDL_GetPrefPath("", "fg-runner")) {
        log_open(std::string(pref) + "fg-runner.log");
        SDL_free(pref);
    }
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // A GUI-subsystem program has no console; started from a command prompt,
    // write to that prompt's console.
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
#endif
    std::string game_dir, cd_dir, save_dir;
    bool save_dir_given = false;
    std::string game_args = "n";
    UiOptions ui;
    bool sound = true;
    int audio = -1;  // -1: default (on with a window, off headless)
    double scroll_step_ms = 120;
    bool original_scrolling = false;
    double ai_step_ms = 10;
    double ai_view_ms = 200;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--game-dir" && i + 1 < argc) game_dir = argv[++i];
        else if (a == "--cd-dir" && i + 1 < argc) cd_dir = argv[++i];
        else if (a == "--save-dir" && i + 1 < argc) { save_dir = argv[++i]; save_dir_given = true; }
        else if (a == "--headless") ui.headless = true;
        else if (a == "--no-sound") sound = false;
        else if (a == "--mute") audio = 0;
        else if (a == "--audio") audio = 1;
        else if (a == "--screenshot" && i + 1 < argc) ui.screenshot = argv[++i];
        else if (a == "--screenshot-after" && i + 1 < argc) ui.screenshot_after = std::atof(argv[++i]);
        else if (a == "--events" && i + 1 < argc) ui.events = argv[++i];
        else if (a == "--fullscreen") ui.fullscreen = true;
        else if (a == "--window") ui.fullscreen = false;
        else if (a == "--scaling" && i + 1 < argc && (std::string(argv[i + 1]) == "integer" || std::string(argv[i + 1]) == "fit"))
            ui.integer_scale = std::string(argv[++i]) == "integer";
        else if (a == "--scroll-ms" && i + 1 < argc) scroll_step_ms = std::max(0.0, std::atof(argv[++i]));
        else if (a == "--original-scrolling") original_scrolling = true;
        else if (a == "--ai-move-ms" && i + 1 < argc) ai_step_ms = std::max(0.0, std::atof(argv[++i]));
        else if (a == "--ai-view-ms" && i + 1 < argc) ai_view_ms = std::max(0.0, std::atof(argv[++i]));
        else if (a == "--scale" && i + 1 < argc) ui.scale = std::max(1, std::atoi(argv[++i]));
        else if (a == "--version") {
            std::printf("fg-runner %s\n", FG_VERSION);
            return 0;
        }
        else if (a == "--") {
            game_args.clear();
            for (++i; i < argc; ++i) game_args += (game_args.empty() ? "" : " ") + std::string(argv[i]);
        } else {
            std::fprintf(stderr,
                         "usage: %s [--version] [--game-dir DIR] [--cd-dir DIR] [--save-dir DIR] [--scale N] [--window|--fullscreen (default)] [--headless] [--mute|--audio] [--no-sound]\n"
                         "          [--scaling integer|fit (default integer: exact pixels; Ctrl+F11 switches)]\n"
                         "          [--scroll-ms MS (battle-map scroll step interval, default 120)]\n"
                         "          [--original-scrolling (sides scroll diagonally, as in the original game)]\n"
                         "          [--ai-move-ms MS (computer's unit moves: time per pixel, default 10; 0 = unpaced)]\n"
                         "          [--ai-view-ms MS (computer's turn: least time each map view is shown, default 200; 0 = unpaced)]\n"
                         "          [--screenshot FILE.png --screenshot-after SECONDS] [--events SCRIPT] [-- game args]\n",
                         argv[0]);
            return 2;
        }
    }
#ifdef _WIN32
    timeBeginPeriod(1);  // 1 ms sleeps and timer ticks instead of ~15.6 ms
#endif
    open_log();
    FG_LOG("fg-runner %s (%s)", FG_VERSION, SDL_GetPlatform());
    // A failure before the game runs: log it and tell the player.
    auto fail = [&](const std::string& msg) {
        FG_LOG("%s", msg.c_str());
        set_error(msg);
        report_errors(ui.headless);
        return 1;
    };
    if (!game_dir.empty()) {
        std::string d = game_dir_in(game_dir);  // the install folder will do, too
        if (d.empty()) return fail("Fantasy General (EXE\\BARENA.EXE) was not found in " + game_dir + ".");
        game_dir = d;
    } else {
        game_dir = locate_game_dir();
        if (game_dir.empty() && !ui.headless) {
            game_dir = pick_game_dir();
            if (!game_dir.empty()) remember_game_dir(game_dir);
        }
        if (game_dir.empty()) {
            if (!ui.headless) return 1;  // the player cancelled the folder dialog
            return fail("Fantasy General not found: install it from GOG.com, then start the runner and "
                        "choose its folder, or pass it with --game-dir.");
        }
    }
    FG_LOG("game directory: %s", game_dir.c_str());
    if (!save_dir_given) save_dir = choose_write_overlay(game_dir);
    if (!save_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(save_dir), ec);
    }
    FG_LOG("saves and settings: %s", save_dir.empty() ? "the game directory" : save_dir.c_str());
    if (cd_dir.empty()) cd_dir = default_cd_dir(game_dir);
    g_trace = std::getenv("FG_TRACE") != nullptr;

    static Machine machine;
    Machine& m = machine;
    g_machine = &m;
    if (!m.as.init(kCpuMapsAllMemory)) return fail("Cannot reserve memory for the game.");
    FG_LOG("game address space at host %#llx; CPU backend: %s",
           static_cast<unsigned long long>(m.as.host_base()), kCpuBackendName);
    if (!cpu_init()) return fail("Cannot start the CPU emulator.");

    m.fs = std::make_unique<DosFs>();
    m.fs->map_drive('C', game_dir);
    if (!save_dir.empty()) m.fs->set_overlay('C', save_dir);
    m.fs->map_drive('D', cd_dir);
    g_cdrom.init(game_dir, 3);
    if (!sound) {
        // Without DIG.INI/MDI.INI the game runs with sound effects disabled.
        m.fs->hide("DIG.INI");
        m.fs->hide("MDI.INI");
    }
    m.fs->set_current_drive('C');
    if (m.fs->chdir("\\EXE")) return fail("No EXE folder in " + game_dir + ".");

    std::string exe;
    if (m.fs->resolve("C:\\EXE\\BARENA.EXE", exe)) return fail("BARENA.EXE not found in " + game_dir + ".");
    std::string err;
    if (!load_le(exe, m.as, AddressSpace::kObjectBias, m.image, err)) return fail("Cannot load the game: " + err);
    for (size_t i = 0; i < m.image.objects.size(); ++i) {
        const auto& o = m.image.objects[i];
        FG_LOG("object %zu: %08x-%08x (ghidra %08x)", i + 1, o.load_base, o.load_base + o.virtual_size,
               o.preferred_base);
    }
    FG_LOG("%u fixups applied; entry %08x, stack %08x", m.image.fixups_applied, m.image.entry(), m.image.stack_top());

    init_bios_data_area(m.as);
    m.code_sel = m.ldt.make(0, 0xFFFFFFFF, true);
    m.data_sel = m.ldt.make(0, 0xFFFFFFFF, false);
    if (!m.code_sel || !m.data_sel || !build_psp(m, game_args)) return fail("Cannot set up the game's memory segments.");
    FG_LOG("selectors: code %04x data %04x psp %04x env %04x", m.code_sel, m.data_sel, m.psp_sel, m.env_sel);

    install_interrupt_stubs();
    install_game_hooks(scroll_step_ms, original_scrolling, ai_step_ms, ai_view_ms);

    CpuRegs regs{};
    regs.eip = m.image.entry();
    regs.esp = m.image.stack_top();
    regs.eflags = 0x202;
    regs.cs = m.code_sel;
    regs.ds = regs.ss = m.data_sel;
    regs.es = m.psp_sel;
    if (audio < 0) audio = ui.headless ? 0 : 1;
    // Small device buffers: the emulated sound card must not read far ahead of
    // the half-buffer Miles is filling. When muted, SDL's dummy driver keeps
    // consuming audio in real time so the game behaves the same.
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
    if (!audio) SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO) && audio) {
        FG_LOG("audio unavailable: %s", SDL_GetError());
        FG_LOG("(the runner is 32-bit: install the 32-bit PipeWire client library, e.g. "
               "'sudo dnf install pipewire-libs.i686'); continuing without sound output");
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
        SDL_InitSubSystem(SDL_INIT_AUDIO);
    }
    if (g_cdrom.present()) g_cd_audio.init(g_cdrom.tracks());
    g_dig.init();

    // The game runs on its own thread; the main thread owns the SDL window.
    std::atomic<bool> game_done{false};
    std::thread game;
    auto start_game_thread = [&] { game = std::thread([&] {
        if (!cpu_thread_init() || !start_timer(1193182.0 / 65536)) {
            FG_LOG("cannot set up the game thread");
            game_done = true;
            return;
        }
        FG_LOG("starting game: BARENA.EXE %s", game_args.c_str());
        m.exit_code = cpu_run(regs);
        FG_LOG("game exited with code %d", m.exit_code);
        game_done = true;
    }); };
    if (!run_ui(ui, game_done, start_game_thread)) {
        FG_LOG("no window could be opened; use --headless to run without one");
        set_error("No window could be opened for the game (see the log for SDL's reason).");
        report_errors(false);
        std::fflush(nullptr);
        std::_Exit(1);
    }
    if (!game_done) {
        FG_LOG("window closed");
        g_dig.shutdown();
        g_cd_audio.shutdown();
        std::fflush(nullptr);
        std::_Exit(0);  // the game thread cannot be stopped cleanly
    }
    if (game.joinable()) game.join();
    g_dig.shutdown();
    g_cd_audio.shutdown();
    report_errors(ui.headless);  // e.g. a FATAL stop of the game
    SDL_Quit();
    return m.exit_code;
}
