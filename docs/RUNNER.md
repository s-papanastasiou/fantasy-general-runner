# Runner specification

What the runner has to provide so that BARENA.EXE's original code runs
without DOS, either natively in a 32-bit Linux process or in the Unicorn CPU
emulator (see "CPU backends"). Derived from `tools/ghidra/scripts/FGIntInventory.java`
and call-site analysis (`FGCallSites.java`); addresses are Ghidra addresses
(preferred object bases).

## Process layout (as implemented)

- The main thread runs the SDL3 window; the game runs on its own thread.
- Game memory is one reserved 256 MB host region, so game linear address X
  is host `base + X`. Native backend: the game's segments (LDT descriptors
  via `modify_ldt`) are based at that region. Unicorn backend: the region is
  the emulator's physical memory from address 0. The game therefore sees a full DOS first megabyte (IVT, BIOS
  data area, conventional memory, VGA window at `0xa0000`) as ordinary
  memory; nothing needs trapping for low-memory access.
- LE objects are loaded at **Ghidra address + `0x1000000`** with fixups
  applied; the DPMI heap is `0x2000000`–`0x10000000`.
- Conventional memory `0x1000`–`0x9FFFF` serves DOS-memory allocations
  (DPMI `0100h`); `0x600`–`0x6FF` holds the runner's interrupt stubs,
  `0x700` the VBE BIOS data.
- Segment descriptors (`Ldt`, installed by the backend): DPMI `0000h` (allocate), `0006h`
  (get base), `0007h`/`0008h`/`0009h` (set base/limit/rights). Miles builds
  selectors; the CD-audio code writes MSCDEX requests through `GS:` with a
  DOS-memory selector.

## Trapping and interrupts

This section describes the native backend; `traps.cpp` holds the part both
backends share (instruction decoding, interrupt injection, game hooks).

`int`, `in`/`out`, `ins`/`outs`, `cli`/`sti`, `hlt` fault in user mode
(SIGSEGV, trap 13); the handler (entered through `fg_trap_entry`, which
restores glibc's `%gs`) decodes the instruction at CS:EIP, emulates it on
the saved context and advances EIP. Note: sigreturn reloads ESP from
`REG_ESP`, not `REG_UESP`.

The PIT interrupt is a POSIX timer signal (SIGRTMIN) aimed at the game
thread at the rate programmed into port 40h. If the virtual IF is set, the
handler saves the context, pushes an iret frame returning to a trampoline
(`int FEh` at `0x600`) and jumps to the game's int 08h handler; the
trampoline traps back and the context is restored exactly. Ticks arriving
with IF clear are delivered on `sti` or at interrupt return. Unhooked
vectors point at an `iretd` stub (`0x610`); int 08h's default (`0x618`) also
advances the BIOS tick count at `0x46c`.

Software interrupts to vectors the game hooked call its handler; others
behave like DOS/4GW's defaults (int 21h/31h emulated, 10h/16h/1Ah/2Fh/33h
reflected to the emulated real-mode services).

## CPU backends

Chosen at build time with CMake `-DFG_CPU=native|unicorn` (default: native
for a 32-bit build, unicorn for 64-bit). `tools/run.sh` uses Unicorn by
default (since 2026-11-26), built in `build-unicorn/`; `FG_CPU=native
tools/run.sh` builds and runs the native variant in `build/`. The interface is
`src/cpu_backend.h`; DOS, DPMI, devices and UI do not know which one runs.

- **native** (`cpu_native.cpp`, `cpu_native_entry.S`): as described above.
  32-bit Linux only.
- **unicorn** (`cpu_unicorn.cpp`, needs `unicorn-devel` >= 2.0): any 64-bit
  host. Game memory is mapped with `uc_mem_map_ptr` (all readable/writable:
  no guard pages). A system area above it (`0x10000000`) holds a GDT, the
  LDT (kept in step with `Ldt`) and a ring-0 `iretd` stub that enters the
  game at CPL 3. Measured on the scripted battle route: same screens as
  native and less CPU time (2.1 s vs 5.3 s over 52 s; no signal per trap).

Unicorn details learned the hard way:

- `int n` reaches the `UC_HOOK_INTR` hook with EIP past the instruction;
  the hook steps back 2 bytes and uses the shared decoder.
- **No #GP for routine work.** QEMU clears its "exception in progress"
  state only when it delivers an exception itself; after one hooked #GP the
  next fault is a double fault (8) and the one after that a triple fault.
  So the game runs with IOPL 3: `cli`/`sti`/`pushf`/`popf` act on the
  emulated IF, which `Machine::virtual_if` mirrors around every handler.
- **`in`/`out` do not fault** at CPL 3 in Unicorn (no I/O permission
  check); `UC_HOOK_INSN` hooks for IN/OUT call the port emulation.
  Not verified: whether `ins`/`outs` reach the same hooks.
- **Never `uc_emu_stop` from another thread.** It leaves the CPU state
  slightly wrong now and then (the game failed at random, about 1 run in 3).
  The timer thread only counts ticks; a `UC_HOOK_BLOCK` hook stops the
  emulator at the next block boundary when a tick can be delivered.
  Software interrupts also deliver pending ticks on the way out.
- Code the runner writes (DOS file reads, freed heap) is dropped from the
  translation cache (`cpu_code_written` -> `uc_ctl_remove_cache`).
- Leaving the game (`game_exit`) longjmps out of the Unicorn hook; the
  emulator is not used afterwards.

## Devices

- **Video**: VBE 1.2 BIOS with modes 100h/101h (banked, 64 KB window over
  1 MB VRAM; bank switch copies window <-> VRAM), 4F07h display start, VGA
  DAC ports, int 10h palette functions. The UI thread snapshots VRAM +
  window under a mutex.
- **Mouse**: int 33h driver. The game sets its range to 8× the screen size
  and installs a 18-byte *real-mode* event handler (copied to DOS memory)
  that stores BX/CX/DX at `0000:04F0`; the runner recognises that code and
  performs the stores itself on every event.
- **Keyboard**: int 16h and DOS 01h/06h/07h/08h/0Bh from an SDL key queue.
- **CD-ROM**: MSCDEX + device requests (see `cdrom.cpp`). Play/stop/resume
  and volume drive `cd_audio.cpp`, which decodes `MUSIC/TrackNN.ogg`
  (stb_vorbis) on SDL's audio thread; a play range runs on into the next
  track as on a disc. Status/position queries are answered from a clock.
- **Sound effects**: Miles AIL 3.03 (inside the game) loads `SB16.DIG` (per
  `DIG.INI`) into DOS memory and calls it via int 66h (DPMI 0300h). The
  runner answers those calls as SB16.DIG does (`dig_driver.cpp`, functions
  300h-306h and 400h-405h; see the header comment there) and keeps the real
  image in memory for its tables. Miles mixes natively into a DMA double
  buffer described by the image's status table (DST at +4FCh: two far
  pointers, +8 = half being played); an SDL3 stream plays the halves from DOS
  memory and flips the index at each half boundary, like the card's IRQ.
  Format index: bit 0 = 16-bit signed (else 8-bit unsigned), bit 1 = stereo.
  The game uses 22050 Hz 8-bit stereo, 2 x 2048-byte halves. There is no
  MIDI (`MDI.INI` absent): music is CD audio. When muted or headless, SDL's
  dummy audio driver keeps the same real-time consumption.

## Software interrupts in use (61 distinct services)

### int 21h — DOS (almost all from the Watcom C runtime)

| Function | Use |
|---|---|
| `3Ch`/`3Dh`/`3Eh`/`3Fh`/`40h`/`41h`/`42h` | create/open/close/read/write/delete/seek |
| `4400h`/`4401h`/`440Ah` | IOCTL device info (isatty etc.) |
| `5700h`/`5701h` | get/set file time |
| `1Ah`/`2Fh`/`4Eh`/`4Fh` | DTA + find first/next |
| `48h`/`49h`/`4Ah`/`4800h` | memory alloc/free/resize |
| `2Ah`/`2Ch` | date/time |
| `01h`/`08h`/`0Bh` | keyboard (CRT getch/kbhit) |
| `25h`/`35h` | set/get interrupt vector (CRT: 06h, 24h; Miles: timer) |
| `30h`, `FF00h`, `EDh`, `3700h` | version, DOS/4GW detection, switch char |
| `4Bh`/`4Dh`/`0B04h` | spawn/exit code (CRT `spawn`/`system`, probably unused) |
| `4Ch` | exit |

Paths: drive letters `C:` = game data dir (with a writable overlay
folder for saves when needed, see "Saves" in `docs/STATUS.md`), `D:` = CD (`game.gog` data
track, plus CD-audio tracks as `.ogg`). The game builds paths like
`%c:\%s\dat\%d.smk` and `..\dat\fgpal01.lbm` (cwd = `EXE`).

### int 31h — DPMI

| Function | Use |
|---|---|
| `0000h`, `0006h`–`0009h` | descriptors (Miles, CRT) |
| `0100h`/`0101h` | DOS memory blocks (real-mode buffers: mouse, MSCDEX, VESA info) |
| `0200h`/`0201h` | get/set real-mode interrupt vector |
| `0205h` | set protected-mode interrupt vector (Miles timer ISR, keyboard?) |
| `0300h` | simulate real-mode interrupt — carries int 10h, 2Fh, 33h below |
| `0303h`/`0304h` | allocate/free real-mode callback (mouse event handler) |
| `0501h`–`0503h` | allocate/free/resize memory block |
| `0800h` | physical address mapping (Smacker, VESA LFB) |

### Via DPMI 0300h (simulated real-mode interrupts)

- **int 10h** video BIOS / VESA: `1A00h` (display combination),
  `4F00h`/`4F01h`/`4F02h`/`4F05h`/`4F0Ah` (+ UVBELib's `4F0Fh` probe).
  Plan: emulate a VBE 1.2/2.0 BIOS offering mode `101h` (640×480×8, banked
  64 KB window at `0xa0000`); bank switches copy between the window and a
  shadow framebuffer that SDL presents.
- **int 33h** mouse (game: `MOUSE_CallDriverA/B` `0x3c7f5`/`0x3c8f4`; the
  function number is in a register block, so support the usual set:
  `00h`–`0Ch`, `0Fh`, `1Ah`…) plus `0Ch` set event handler through a DPMI
  real-mode callback (`0x8aae9`).
- **int 2Fh** MSCDEX `1510h` send device request (`CD_SendMscdexIoctl`
  `0x48570`, `CD_SendAudioCommand` `0x486c6`): IOCTL input/output,
  `84h` play audio, `85h` stop, `88h` resume. Back with `MUSIC/TrackNN.ogg`.

### Others

- `int 00h`-based `int N; ret` stub tables at `0x8b327` (UVBELib) and
  `0x929ab` (Watcom `int386`): the vector is chosen at run time.

## Hardware ports

| Port | Use | Where |
|---|---|---|
| `40h`/`43h` | PIT channel 0 rate | Miles `0x57a48`, Smacker `0x8f550`–`0x8fbd3` |
| `20h` | PIC EOI | Miles ISR |
| `3C8h`/`3C9h` | VGA DAC palette | `SMK_SetVgaPalette256` `0x8f410` |
| `3DAh` (from BDA `0x463`+6) | retrace wait | `WaitVerticalRetraces` `0x58081` |
| via `inp`/`outp` | unknown yet | callers `0x10a23`, `0x10a9d` |

## Asynchronous events

- Timer: Miles reprograms the PIT and hooks the timer interrupt through DPMI
  `0205h`; the runner must call the installed handler at the programmed rate.
- Keyboard: check for a keyboard ISR hook (int 09h via `0205h`/`25h`) vs. BIOS
  calls; feed scancodes on port `60h` if hooked.
- Mouse: invoke the real-mode-callback event handler on mouse events.

## Idle detection

The game waits by spinning, as DOS programs did, which kept a host core at
100% (an idle campaign map: ~45 s of CPU per 45 s). The runner recognises a
poll site that is hit again within 50 us of its previous empty poll, 100
times in a row, and sleeps before answering (`runner/src/idle.h`); loops that
do real work between polls are slower than that and never sleep.

| Spin | Site | Sleep |
|---|---|---|
| kbhit (`int 21h` 0Bh; also 01h/06h/07h/08h, `int 16h` 01h/11h) | CRT `0x9043d`, called by `PollKeyboard` `0x3a008` / `FlushKeyboard` `0x3a08b` (main menu) | 1 ms |
| PIT counter read (`in 40h`) | `SMK_TimerMs` `0x8fbc8`: Smacker waiting for its next frame | 0.5 ms |
| VGA status `3DAh` | `WaitVerticalRetraces` `0x58081`; VESA480.DLL's retrace wait (palette fades, `FUN_00043bd5`) | towards the next retrace, at most 1 ms per poll |
| UI input loops (campaign map, battle map, screens built on `UI_HitTest` `0x6d968`) | `int FDh` hook replacing `sub esp,10h` at `0x3cbff` in `ReadMouseState` `0x3cbfa`; installed only if the bytes match | 1 ms |

Sleeping in a trap handler delays a timer interrupt by at most the sleep;
input arrives on the UI thread meanwhile. Keep each sleep short (1 ms): the
game draws its mouse cursor from a Miles timer callback (`0x3cd78`, 60/70
Hz), so a whole-frame sleep in the retrace wait made the cursor stutter
during palette fades (timer interrupts 17 ms apart, a third of them lost;
reported 2026-11-27). `FG_PROFILE` prints the longest gap between timer
interrupts each second; normally under 4 ms. After this change, the scripted
45 s intro/menu/map run uses ~0.6 s of CPU.

### Battle-map scrolling

The battle loops (`FUN_0002be3a` and six others) call
`ScrollBattleMapStep` (`0x6ef53`) on every pass while the pointer is in an
edge zone (`FUN_0007f7b2` sets the direction in `0xce233`, 8 = none). There
is no timer: on 1996 hardware the redraw after each step (~90 px, two hex
columns) set the speed, while natively the map crossed its whole width in
under 0.1 s. A second `int FDh` hook (replacing `sub esp,4` at `0x6ef5b`)
keeps steps at least `--scroll-ms` apart (default 120 ms, ~750 px/s; the
user confirmed it feels right). It
waits in 1 ms slices, re-executing the hook between them, so timer
interrupts (Miles audio) keep running during the wait.

Directions: `0xce233` holds a hex direction (0 up, 1 up-right, 2 right,
3 down-right, 4 down, 5 down-left, 6 left, 7 up-left; 8 none), which
`ScrollBattleMapStep` applies once or twice (`0x3c17e` via the neighbour
step `0x3b6ac`) so the view stays on an odd column: 2 columns and, for a
diagonal, 1 row per step. `UpdateEdgeScrollDirection` uses only 0/4 for
the top (y <= 35, the status bar) and bottom (y >= 435, the panel) zones,
and 1/3/5/7 for the sides (x < 3, x > 636; upper or lower half), checked
first, so the original has no straight sideways scroll and no corner of
its own. At the map's left/right end the upper-half diagonals (1, 7) do
not move at all (odd-column rule), the lower-half ones only vertically.
The hook rewrites the direction to eight-way (default; reported
2026-11-27 that corners did not scroll diagonally): sides 2/6, top/bottom
0/4, a side inside the top or bottom zone diagonal; a diagonal step that
left the view unmoved continues vertically. The pointer comes from the
game's mouse copy (`0xd8448` x, `0xd844c` y, screen pixels): the driver's
own coordinates are in the game's range, 8x the screen.
`--original-scrolling` keeps the game's choice.

Arrow keys (added 2026-11-27; the game itself has none): the battle loops
do `if (0xce233 != 8) ScrollBattleMapStep()` right after
`UpdateEdgeScrollDirection`, so a hook on its epilogue (`mov esp,ebp` at
`0x7f899`) sets the direction from the held arrows when the pointer set
none (the pointer wins; opposite keys cancel). The UI tracks held arrows
by scancode (cleared on focus loss) and still passes them to the game,
which uses arrows only in text entry (left arrow at `0x39c3d`/`0x39ec1`).
Because the runner paces steps, a step decided by the game can wait up to
`--scroll-ms`; if its cause is gone by then (keys let go, pointer left the
edge zone), the step hook sets direction 8 and the step does nothing, so a
tap scrolls once and nothing runs on after leaving the edge (both modes).
`--events` takes `T hold up|down|left|right` and `T release ...`.

### Computer unit moves

`MoveUnitAnimated` (`0x7a2d3`) slides a unit one pixel per loop pass. Only
its special modes wait (`DelayTicks(1)`, 10 ms at the game's 100 Hz tick,
per pixel); ordinary moves have no timer, so the computer's whole turn
flashed past in ~2 s. A hook replacing `cmp byte [0xe98af],0` at `0x7aa87`
in the step loop (the runner performs the compare itself) holds each pixel
step of a *visible* move on the *computer's* side (`0xce24c[0xe98dc]`) to
`--ai-move-ms` (default 10, the game's own per-pixel wait; 0 = unpaced):
about 0.4 s per hex, and the same scenario turn took ~14 s. Hidden moves
and the player's own moves are unchanged. The hook's operand is relocated
at load, so the hook table can name a 4-byte data address inside the
replaced bytes, converted before comparing.

### Computer turn: map views

On the computer's turn the game recentres the view on each unit it
considers and redraws the whole map (`RedrawBattleMap` `0x85a45`, view
origin `0xe93d8`), often several units in a row. A full redraw once took
a good fraction of a second; natively the views flicked by every 50-150 ms
and looked like very fast scrolling (reported 2026-11-27). A hook on the
redraw (`sub esp,4` at `0x85a4d`) keeps each view of the computer's turn on
screen at least `--ai-view-ms` (default 200, chosen by the user by hand; 0 = unpaced) before drawing a
different one; redraws without a view change and the player's turn are not
delayed. Measured on the user's save with 400 ms: view changes at least
~0.4 s apart (before: 50-150 ms); the user preferred 200.

## Window loop pacing

The UI thread converts the game's screen, uploads it and presents once per
display refresh, paced by vsync. Without working vsync (offscreen or
software rendering, some drivers, hidden windows) presenting returns at
once and the loop spun a core (measured 60-100% of one core with window
and audio). It now never presents faster than the display's refresh rate
(60 Hz if unknown), and uploads the image only when it changed; a render
device or target reset forces a new upload. Measured without vsync,
window and audio (2026-11-27, Ryzen 7 9700X), % of one core, Unicorn /
native: intro video 5.5/6.2, idle main menu 2.6/3.1, idle battle map
11/4.6, scrolling 21/6.7, computer's turn 15/10. Headless (no window,
no audio): idle menu ~1, idle battle map 7.4/2.2.

## Display scaling

The game's 640x480 has square pixels on a 4:3 screen, so the aspect ratio is
right whenever the picture is 4:3. By default (`--scaling integer`) the
picture is drawn at a whole multiple, each game pixel an exact N x N block,
with black borders; the window snaps to k x 640x480 screen pixels ~0.3 s
after a resize (not when maximised or fullscreen), and the title bar shows
the scale. The window is high-pixel-density so a scaled desktop does not
stretch it. Fullscreen is borderless desktop fullscreen at the monitor's
native resolution, with the same integer scaling: changing the display mode
would not help, since an LCD's own scaler resamples any non-native mode
(and Wayland only emulates mode changes). `--scaling fit` fills the screen at
4:3 with uneven pixel sizes. 640x400 modes (only the brief text screen at
start) are stretched to 4:3 like a CRT, so their pixels cannot be exact.

## Libraries that may be replaced wholesale instead of emulated

- **Miles AIL 3** (`0x4cc60`–`0x58200`): hook its API entry points and
  play through SDL3 audio (MIDI music via a synth, digital samples direct).
- **Smacker** (`0x8c100`–`0x8fdff`): hook `PlaySmackerMovie` callees and use
  a native Smacker decoder.
- **UVBELib**: not needed if the runner always passes `n` (the GOG default).

## Why the runner is a 32-bit program (and the alternatives)

The game's code runs natively inside the runner's own process, on the host
CPU, in 32-bit protected mode with its own LDT segments. A Linux process is
either 32-bit or 64-bit and every library it loads must match, so the runner
is built with `-m32` and needs 32-bit builds of SDL3 and of the client
libraries SDL loads at run time: Mesa (`mesa-dri-drivers.i686`,
`mesa-libEGL.i686`, `mesa-libGL.i686`) for an accelerated Wayland window and
PipeWire (`pipewire-libs.i686`) for sound. The kernel, the PipeWire server
and the GPU stack stay 64-bit. Without 32-bit Mesa the runner falls back to
SDL's X11 backend with software rendering; without 32-bit PipeWire it runs
silent (SDL dummy audio driver).

Alternatives if 32-bit packages become a problem:

1. **64-bit runner, game code in compatibility mode** (what Wine's WoW64
   does). x86-64 CPUs run 32-bit code segments inside a 64-bit process, so
   SDL/Mesa/PipeWire would be 64-bit. Costs: the trap path gets harder: a
   64-bit signal frame does not save DS/ES (the handler must read and set
   them directly), the game's FS/GS loads clobber glibc's 64-bit TLS base
   (restore it with `wrfsbase`/`arch_prctl` on every trap), entry/exit need
   far transfers between 64- and 32-bit code segments, and the game memory
   must sit below 4 GB (it already is a single 256 MB reservation).
2. **Two processes**: a minimal 32-bit process that only runs the game code
   (needs only `glibc.i686`, no SDL), and a 64-bit process for window, audio
   and input, sharing the frame buffer, palette, audio buffers and input
   state through shared memory. Moderate plumbing; the trap code is
   unchanged. The cheapest way out of the i686 graphics/audio dependencies.

Decision (2026-11-25): stay 32-bit. It works on Fedora today. Revisit with
option 2 if multilib support goes away. Since 2026-11-26 the Unicorn
backend (below) is the default and runs as an ordinary 64-bit program, so
the native backend's 32-bit needs only matter when it is chosen.

## Portability: Windows, 64-bit-only Linux, macOS

The native backend depends on Linux specifics: `modify_ldt` (custom segment
bases), asynchronous signals for the timer interrupt, and a 32-bit process.

**Native Windows port of this design: not recommended.** The device and DOS
emulation (about two thirds of the code) is portable C++/SDL3, and trapping
maps onto vectored exception handlers, but 64-bit Windows offers no LDT at all
(why it cannot run 16-bit programs). The game would have to run flat, with
its accesses below 64 KB (BIOS data area, mouse state at `0x4F0`) patched or
emulated, every access through Miles'/the CD code's own selectors emulated,
and timer interrupts injected by suspending the game thread and editing its
context. Estimate 15-25 sessions, risky.

**CPU-emulator backend (the portable option, chosen below).** Replace only the CPU layer (then `trap.cpp`,
`trap_entry.S`, `ldt.cpp`, timer glue) with an embedded x86 emulator; keep
all services and the UI. The emulator handles descriptor tables, so DOS/4GW,
Miles and the CD code's selectors work unchanged; `int`, `in`/`out` become
hooks; the timer is injected between short emulation slices; the game memory
can stay our own buffer (mapped into the emulator), so services keep direct
access. Runs as a normal 64-bit program on Linux, Windows and macOS, x86-64
or ARM. Roughly 5-20x slower than native, which this 486/Pentium-era game can
afford (DOSBox runs it fine). Keep the native backend on Linux and select at
build time. Estimate 8-15 sessions. Candidates (game uses only ~30 x87 instructions):

| Emulator | Licence | Notes |
|---|---|---|
| **Unicorn** (QEMU CPU core) | GPL-2.0 | Noted option. Full x86 + FPU, JIT, all platforms, hooks. Quickest route; makes the distributed runner GPL-2.0 (own files may stay MIT/BSD, combined work is GPLv2; not GPLv3-only). |
| Bochs CPU core | LGPL-2.1 | Accurate, FPU; must be extracted from a full PC emulator (+5-10 sessions); as a shared library the runner can stay permissive. |
| libx86emu | X11/MIT-style | Small interpreter, 32-bit + basic protected mode, no FPU; segment support to verify/extend (+3-8 sessions). Cheapest permissive route if it holds up. |
| Own 386 interpreter (+ Zydis decoder, MIT) | ours | Only what the game needs; full control (+10-15 sessions). |
| DOSBox core | GPL-2.0 | Same licence as Unicorn, harder to extract. |
| Blink | ISC | Not suitable (x86-64 Linux user space and real mode only). |

Static recompilation (x86 -> C ahead of time) is the other route: fastest
and portable, but `VESA480.DLL` is loaded and run at run time and the
startup code writes into the code object, so it needs special handling.

Decision (2026-11-26): Unicorn, implemented as a second backend (see "CPU
backends"); the project is licensed GPL-2.0-or-later.

**Windows (2026-11-27):** `tools/build_windows.sh` cross-compiles with
MinGW-w64 and packages `dist/fg-runner-windows.zip`. Platform notes:
- Game memory: `VirtualAlloc` (reserve + commit, touched pages only);
  freeing decommits and recommits so the memory reads as zeros.
- Files are opened with `O_BINARY` (no line-ending translation).
- `timeBeginPeriod(1)`: otherwise sleeps and the timer thread run in
  ~15.6 ms steps, too coarse for the 200-360 Hz game timer and 1 ms idle
  sleeps.
- Leaving the game jumps out of a Unicorn hook with
  `__builtin_setjmp`/`__builtin_longjmp` (all platforms): a plain `longjmp`
  unwinds on 64-bit Windows and fails on JIT frames without unwind data
  (QEMU does the same).
- An embedded manifest makes UTF-8 the process code page (Windows 10 1903+),
  so paths from SDL (UTF-8) work with the narrow C file calls.
- Without `--cd-dir`, the runner extracts the CD data track from
  `game.gog` itself (`cd_extract.cpp`, same as `tools/extract_cd.py`) into
  the per-user folder (`%APPDATA%\fg-runner\cd`,
  `~/.local/share/fg-runner/cd`).
- The game folder is found through the GOG registry key or asked for with
  a folder dialog (`game_locate.cpp`; see "Finding the game" in
  `docs/STATUS.md`).
- It is a console program: the console shows the runner's log.
Tested under Wine only (headless battle route, identical screenshots).
macOS: not done (would need an ARM64/x86-64 build with SDL3 and Unicorn;
the code has no Linux-only parts left in the Unicorn configuration).
