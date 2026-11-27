# Project status and how to pick it up

Last updated 2026-11-27.

## Where things stand

The **native runner** (phases 0-5 of the original plan) is done: `BARENA.EXE`
from the GOG install runs on Linux without DOSBox. Its own code executes on
the host CPU; the runner emulates DOS/4GW, DOS, DPMI, the BIOS, VGA/VESA, the
mouse, MSCDEX, the PIT and a Sound Blaster 16 driver.

Confirmed working (by the user and/or automated runs): intro with the Smacker
video and CD music in sync, main menu, campaign briefing, strategic campaign
map, mouse and keyboard, CD music (Ogg tracks), sound effects, window scaling,
Alt+Enter fullscreen. Since 2026-11-26 (commit `ab61fe9`): low idle CPU use,
paced battle-map scrolling, pixel-exact integer scaling (see `docs/RUNNER.md`).

**Unicorn CPU backend** (2026-11-26): the game also runs in the Unicorn x86
emulator as a normal 64-bit program. It is the default in `tools/run.sh`
(needs `unicorn-devel`); `FG_CPU=native tools/run.sh` runs the native one.
Checked headless only: the scripted battle route gives
the same screens as native, with less CPU time. Not yet played by hand
(window, sound, music, long sessions). See "CPU backends" in
`docs/RUNNER.md`. The project is licensed GPL-2.0-or-later (`LICENSE`).

**Finding the game** (2026-11-27, `game_locate.cpp`): the runner needs
an installed GOG copy, Windows or Linux installer (their game files are
identical except a re-encoded `MUSIC/Track16.ogg`; Windows puts them in the
install folder itself, Linux in `<install>/game/data`). Search order:
`--game-dir`, `$FG_GAME_DIR`, the remembered choice
(`<pref>/fg-runner/game-dir.txt`), next to the runner, the registry
(`HKLM\SOFTWARE\GOG.com\Games\1430136345` `path`, 32-bit view; or
`DOSBOXFOLDER`'s parent), default GOG/Galaxy/Heroic/Lutris/Wine folders;
then SDL's folder dialog (not headless), which is remembered. Checked:
headless on Linux for both layouts, the remembered file, nothing found;
the registry under Wine. The dialog itself is not tried yet.

**Saves** (2026-11-27): writes to C: go to an overlay folder like GOG's
DOSBox `mount -t overlay` (`DosFs::set_overlay`; reads prefer the overlay,
files opened for writing are copied in first, listings merge, deletes and
renames work across both). `choose_write_overlay`: `<install>/cloud_saves`
if present and writable (Windows/Galaxy), else none if `SAVES` is writable
(Linux installer, as its DOSBox), else `<pref>/fg-runner/drive-c`;
`--save-dir` forces one. Checked: a battle Fast Save on a read-only copy
lands in `drive-c/SAVES/QUICKSAV.SAV`, and the Load menu lists it next to
the install's saves; cloud_saves picked on Linux and under Wine. To reach
the save buttons in the scripted route, add
`47 click 619 47; 51 click 320 240` (end placement, dismiss the turn
screen); the left column then holds Fast Save Game (19,90), Load Game
(19,290), Save Game (19,330).

**CPU use** (2026-11-27): with no vsync the window loop spun a core; it is
now capped at the display refresh and skips unchanged uploads. Figures
per phase in "Window loop pacing" (`docs/RUNNER.md`); everything under a
quarter of one core in the worst case. The idle battle map's ~7% (Unicorn,
headless) is ~1000 idle-sleep passes a second; longer idle sleeps would
lower it if ever needed.

**Arrow-key scrolling** (2026-11-27): held arrows scroll the battle map
through the game's own edge-scroll path (pointer first; two keys
diagonal); a tap is one step. Checked headless with `hold`/`release`
events; the user confirmed the keys work.

**Edge scrolling** (2026-11-27): eight-way (sides straight, corners
diagonal) instead of the original's diagonal sides; `--original-scrolling`
restores it. See "Battle-map scrolling" in `docs/RUNNER.md`. Checked with
`FG_TRACE` (direction and view per step) on the user's save; the user
confirmed it looks good.

**Computer moves** (2026-11-27): the AI's unit moves were unpaced (the
user: "really really fast"); now held to `--ai-move-ms` per pixel (default
10 ms, the game's own wait for watched moves), visible moves on the
computer's side only. See "Computer unit moves" in `docs/RUNNER.md`.
Checked headless (Unicorn and native) on the user's saved battle; not yet
judged by hand. Combat animations during the AI turn are not paced by this.
The user confirmed the move speed; the views then flicked by too fast
between the computer's units: each view is now held `--ai-view-ms`
(default 200, the value the user found right after trying it; see
"Computer turn: map views" in `docs/RUNNER.md`).

**Cursor during fades** (2026-11-27): the cursor stuttered while the screen
faded between turns. The retrace wait slept up to a whole frame per poll,
holding off the timer interrupts that draw the cursor; it now sleeps at
most 1 ms per poll. Measured: timer interrupts during a fade 361/s with
gaps under 4 ms (before 242/s, 17 ms). The user confirmed it looks good.

**Release packages** (2026-11-28, branch `release-packaging`):
`tools/build_release.sh` builds the Linux tar.gz (Ubuntu 22.04 container;
glibc 2.35+, SDL3 bundled) and the Windows zip (Fedora 45 container); the
GitHub workflow runs it on pull requests (artifact) and `v*` tags
(GitHub Release). Runner: `--version`, log file in the per-user folder,
errors in a message box, Windows GUI subsystem and icon. Checked: the
Linux package runs the headless battle route in bare Ubuntu 22.04 and
Debian 12 containers with screenshots identical to the Fedora host; the
Windows package under Wine (log file, `--version` from `wine cmd`).
Not yet: a real window/sound session from the packages, real Windows.

**Windows build** (2026-11-27): `tools/build_windows.sh` cross-builds
the Windows package (MinGW, Unicorn backend). Under
Wine the headless battle route gives the same screenshot as on Linux. Not
yet tried on real Windows or with a window and sound. Details in
"Portability" in `docs/RUNNER.md`.

Partly exercised: battles. The user played into a battle by hand and
confirmed (2026-11-26) that unit movement, map scrolling at the default
`--scroll-ms 120` and overall battle speed look right.

Not yet exercised: the rest of battles (most sound effects and game logic), save/load,
scenario and arena modes, the rest of the campaign, the end-game screens.
Play-testing these is the next step; any unsupported operation stops with a
`FATAL` report giving the Ghidra address.

**Scope** (decided 2026-11-27): the runner is the product: executables for
Linux and Windows that run the original game files without DOSBox. A C++
rewrite of the game is no longer planned (the former `docs/DECOMP_PLAN.md`
was removed); changes to the game's behaviour are made with runner hooks.

## Resuming work

```sh
tools/setup_tooling.sh     # idempotent: JDK 25, Ghidra 12.2 (+Watcom cspec, GhidraMCP), Open Watcom -> tools/vendor/
tools/run.sh               # build + run the game (extracts the CD data track to extracted/cd on first run)
FG_CPU=native tools/run.sh # the same with the native 32-bit backend (default: Unicorn)
tools/run.sh --window --scale 3 --scaling integer|fit --scroll-ms 120   # display/feel options
```

Host packages (Fedora), Unicorn backend: `gcc-c++ cmake ninja-build
SDL3-devel unicorn-devel`. Native backend: `gcc-c++ cmake ninja-build glibc-devel.i686
libstdc++-devel.i686 libatomic.i686 SDL3-devel.i686`, plus runtime
`pipewire-libs.i686` (sound) and `mesa-dri-drivers.i686 mesa-libEGL.i686
mesa-libGL.i686` (accelerated window). See `docs/RUNNER.md` for why 32-bit.

### Ghidra

- `tools/ghidra/import.sh` builds `ghidra-project/FantasyGeneral.gpr` from the
  game executable. It refuses to overwrite an existing project, which holds
  the hand-made names. Not in git: if lost, re-import and re-apply names
  (the named functions are listed in `docs/NOTES.md`).
- Start Ghidra with `tools/ghidra.sh` (not the system/Flatpak Ghidra): it
  enables GhidraMCP scripting behind the token in `tools/vendor/ghidra-mcp.token`.
- Claude Code reaches Ghidra through `.mcp.json` -> `tools/ghidra-mcp-bridge.sh`.
  If the `ghidra` MCP tools are missing, run `/mcp` or restart Claude Code.
- `tools/ghidra/run.py SCRIPT.java [args]` runs a script from
  `tools/ghidra/scripts/` inside the running Ghidra over HTTP; works even
  without the MCP tools. Useful scripts: `FGDecompile`, `FGCallSites`
  (`ctx=N`), `FGIntInventory`, `FGRegionStats`, `FGUndefinedReport`.
- Addresses: Ghidra address = runner game-linear address - `0x1000000`
  for the program's objects (crash reports print both).

### Testing without a person at the keyboard

```sh
tools/run.sh --headless --events "6 key esc; 9 key esc; 15 click 548 400" \
    --screenshot /tmp/x.png --screenshot-after 30
FG_TRACE=1 tools/run.sh ...   # every interrupt, file open, driver call
```
`--headless` mutes audio (`--audio` to enable, e.g. with
`SDL_AUDIO_DRIVER=disk SDL_AUDIO_DISK_OUTPUT_FILE=out.raw` to capture it).
Menu coordinates: Start Game (548,400), Mini-Campaign (85,47).

## Lessons that are not obvious from the code

- **Signal context:** on i386, sigreturn reloads ESP from `REG_ESP`;
  `REG_UESP` is informational. Writing only `REG_UESP` silently breaks every
  injected interrupt.
- **%gs in signal handlers:** glibc keeps TLS and the stack-protector canary
  at `%gs`; the game loads its own selectors into GS. `fg_trap_entry`
  restores the host `%gs` before any C++ runs; the runner is built with
  `-fno-stack-protector`.
- **Audio locking:** SDL holds an audio stream's lock while running its
  callback. Use that lock (`audio_lock.h`) for state shared with the
  callback; a separate mutex deadlocks against `SDL_SetAudioStreamFormat`.
- **Start the game after the window is visible**, or the first ~0.15 s of
  video plays unseen while its music is heard.
- **Unhooked software interrupts must reflect to real mode** (DOS/4GW
  default); only vectors the game installed (offset >= 0x1000) call its handler.
- **Mouse:** the game sets the driver range to 8x the screen (0-5112 x
  0-3832) and installs an 18-byte real-mode event handler that stores
  BX/CX/DX at `0000:04F0`; the runner recognises it and does the stores.
- **`n` argument:** GOG's `FG.BAT` passes `n`; it skips UVBELib init but the
  game still loads and runs `VESA480.DLL` (from DPMI memory).
- **The CD's `BARENA.EXE` is an older build** than the installed one; scenario
  files exist only in the installed tree (unpacked from ARJ archives on the CD).
- **Sound:** Miles AIL 3.03 with `SB16.DIG`; no MIDI. The runner answers the
  driver's int 66h calls; Miles itself mixes (22050 Hz 8-bit stereo, 2 x 2 KB).
- **The game never idles.** Every wait is a spin loop: kbhit, PIT, retrace,
  or the UI input loops, which read mouse state from memory and never trap.
  The runner sleeps when a poll site repeats within 50 us, 100 times in a row
  (`runner/src/idle.h`); the input loops get an `int FDh` hook for this.
  To find a spinning loop, sample the game's EIP in the timer signal handler.
- **Speed that came from slow hardware:** battle-map scrolling has no timer
  of its own; the runner paces it (`--scroll-ms`, default 120). Expect other
  animations of this kind; the fix is a hook on the step function.
- **Pointer capture:** the game draws its own cursor from the absolute
  pointer position, clamped to the image. Unconfined, the hidden desktop
  pointer ran into the black border or off the window at the map edges, and
  moving back took a while to register (reported 2026-11-26). While the
  mouse is captured, the UI confines it to the image rectangle
  (`SDL_SetWindowMouseRect`). Fullscreen captures on focus, a window on a
  click into the image; Ctrl+F10 toggles (DOSBox's key); losing focus
  releases. The title bar shows the state.
- **Wayland + 32-bit:** with no 32-bit GL/Vulkan the Wayland backend has no
  renderer at all; the runner retries with X11 software rendering.

## Open items

- Unicorn backend (now the default): play it by hand (window, sound, a full
  battle); fall back with `FG_CPU=native` if something breaks.
- Maybe later: read the game straight from a downloaded installer (easy for
  the Linux `.sh`, which is a zip; the `.exe` via `innoextract`).
- Windows: test the zip on real Windows 10/11 (window, sound, mouse
  capture, save/load into the GOG folder's SAVES, non-ASCII user names).
  macOS not started. Smaller: verify `ins`/`outs` under Unicorn; `uc_close` on exit.
- Publishing: a **private** GitHub repository, `fantasy-general-runner`
  (2026-11-27), `main` starting from a single commit of the code at that
  point; the earlier local history was not published (kept locally on the
  branch `pre-github-history`). Before ever making it public:
  `docs/NOTES.md` and the Ghidra notes describe game internals (without
  game code); `runner/src/input.cpp` embeds the game's 18-byte mouse
  routine as a match pattern, which could be loosened; `docs/screenshots/`
  holds three screenshots of the game's artwork (for the README). The
  runner and tools contain no game code and are licensed GPL-2.0-or-later
  (`LICENSE`; SPDX headers in each file; stb_vorbis keeps its own licence;
  `tools/ghidra/x86watcom.cspec` is untagged because it may derive from
  Ghidra's Apache-2.0 specs).

- Play-test battles, save/load, scenario/arena modes, full campaign.
- Unidentified code block `0x48d53`-`0x4cc60` (28 functions; probably
  CD-audio/timer helpers next to the MSCDEX code).
- Idle CPU use: done (see "Idle detection" in `docs/RUNNER.md`). If a screen
  not yet tested still keeps a core busy, profile it the same way and add a
  hook.
- Still untested by hand (2026-11-26; scroll speed, unit movement and battle
  speed were confirmed good):
  - Keyboard scrolling in battle (if any) may bypass the scroll hook.
  - Combat animations and the rest of a full battle.
  - Window snapping after a manual resize (Wayland/X11 window managers).
  - Possible addition: a hotkey to switch between integer and fit scaling.

### Finding what the game is doing

- `FG_PROFILE=1`: once a second, the busiest addresses with likely callers
  (return addresses found on the stack). `000600` is the runner's
  interrupt-return stub and `0579aa` Miles' timer handler: background.
- `--events "... ; 65.25 shot /tmp/f1.png"`: screenshots at chosen times
  (events run in time order); a contact sheet of them shows animations.
- `tools/ghidra/scripts/FGRefs.java 0xADDR`: every reference to an address
  (headless, like `FGDecompile`); `objdump -d -M intel
  --start-address=... extracted/BARENA.elf` for raw disassembly.
- The user's own `SAVES/GAME1.SAV` (a battle, turn 1) loads from the
  scripted battle: `54 click 19 290; 57 click 19 90; 60 click 320 240`,
  then End Turn `64 click 19 455` starts the computer's turn. Use
  `--save-dir` so nothing is written into the install.

### Scripted route into a battle (headless)

```sh
tools/run.sh --headless --screenshot /tmp/b.png --screenshot-after 52 --events \
  "6 key esc; 9 key esc; 15 click 548 400; 25 click 85 47; 33 click 184 122; \
   36 click 460 456; 44 click 320 240; 48 move 320 240; 50 move 639 240"
```
Intro skipped, Start Game, Mini-Campaign, Fareach Valley, Attack, dismiss
"deploy your army", then pointer to the right edge (scrolls). Run several
instances in parallel only two at a time: many at once gave black screens.
- Possible conveniences: config file, desktop entry, pause on focus loss.
