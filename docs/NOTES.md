# Notes on BARENA.EXE

## Install layout (GOG)

- `start.sh` → GOG DOSBox 0.74 with `dosbox_fg.conf` + `dosbox_fg_single.conf`.
  C: = `game/data`, D: = CD image `game.ins` (cue) / `game.gog` (raw MODE2/2352
  data track) with CD audio tracks 2–16 as `MUSIC/TrackNN.ogg`.
- `FG.BAT` runs `EXE\BARENA.EXE n`; `FGVESA.BAT` runs `BARENA.EXE` with no
  argument. `main` sets a flag when argv[1] starts with `n` and then skips
  `UVBE_InitDriver`, i.e. `n` = don't load SciTech UVBELib, rely on the
  card's own VESA BIOS.
- GOG DOSBox CPU: `cycles=auto limit 26000`.
- The CD's `FG/EXE/BARENA.EXE` (1,137,797 bytes) is an **older build** than the
  installed one (1,146,357 bytes, the GOG/"addon" version); we analyse and run
  the installed one. Scenario files (`*.SCN` etc.) are not on the CD as loose
  files: the installer unpacks them from `DATA.A00`/`DATA.A01` (ARJ).
- At startup the game checks for the CD by opening `D:\FG\FG.BAT`, and always
  loads `\EXE\VESA480.DLL` (its own loader at `0x38592`) into DPMI memory and
  runs it from there, even with `n`; the driver then queries VESA mode `101h`.
- Files only on the CD (not installed): `DAT/1.SMK`–`6.SMK` (intro/cutscenes,
  no `5.SMK`), `DAT/PAL2.LBM`, `DAT/TFONT2.BAK`, installer files.
  The game falls back to the CD drive for missing data files, so the runner
  needs to provide a D: drive backed by `game.gog`.

## Executable

- MZ stub (DOS/4GW 1.97-era, Rational Systems) + bound LE image.
  Inner MZ at file offset `0x344a4`, LE header at `0x3635c`.
  LE data-page offsets are relative to the inner MZ.
- Compiler: WATCOM C/C++32, runtime copyright 1988–1994 (Watcom 10.x).
- No debug info, no packing.
- Objects:

  | obj | base | size | flags |
  |---|---|---|---|
  | 1 | `0x10000` | `0x88d40` | code (R-X) |
  | 2 | `0xa0000` | `0x12` | data (RW-), tiny |
  | 3 | `0xb0000` | `0x3b040` | data/BSS (RW-) |

- Entry `0x910b8` (`_cstart_`). Fixups: 22,814, all `off32` (flat model;
  no selector fixups).
- Middleware linked in: Miles AIL 3 (audio; `.DIG`/`.MDI` drivers on disk),
  Smacker (`SMK2` video), SciTech UVBELib/Ultra (universal VESA driver,
  loads `\exe\VESA480.dll` = `erunivbe.drv`-style driver).
- Object 2 is an 18-byte table of CP437 box-drawing characters; its preferred
  base happens to be `0xa0000`, so in Ghidra it overlaps the VGA window.
  Only one fixup refers to it (`0x106b0`). The runner relocates it.

## Sound

- Miles AIL **3.03** (18-Jun-95). GOG's `DIG.INI` selects `SB16.DIG`; no
  `MDI.INI`, so no MIDI music (music is the CD audio tracks).
- `.DIG` driver layout (AIL3 VDI): signature `AIL3DIG\x1a`, version `0x0112`,
  `+0x0C` IO config list, `+0x12` env var name, `+0x16` IO/IRQ/DMA block,
  `+0x2E` service rate, `+0x30` busy, `+0x32` driver number, `+0x34` ISR
  (int 66h entry, `0x16A` in SB16.DIG), `+0x36` previous ISR, `+0x3A`
  scratch[128], `+0xBA` device name, `+0x10A` handler table (300h-306h),
  `+0x118` (400h-405h). SB16.DIG: DDT at `+0x40C`, DST at `+0x4FC`.
- `SOUND/*.MEL`: 12-byte header (`u32 0`, `u32` sample rate = 11025, `u32`),
  then sample data (some files, e.g. `200.MEL`, have a different header).
  `SOUND/1.BNK`: sample bank index loaded at startup.

## Calling convention

Watcom register convention (`__watcall`): args EAX, EDX, EBX, ECX, then stack;
result EAX; callee saves everything but EAX; callee pops stack args.
Typical prologue `push ebx; push ecx; push edx; push esi; push edi; push ebp;
mov ebp, esp` (`53 51 52 56 57 55 89 e5`). Library code (Miles, Smacker)
uses `push ebp; mov ebp, esp` with stack arguments (cdecl).

## Code map (first pass, 2026-11-25)

The linker interleaves library modules with game modules. Ranges are
approximate; "watcall" = Watcom C code with the `53 51 52 56 57 55 89 e5`
prologue.

| Range | What | Evidence |
|---|---|---|
| `0x10000`–`0x3e3ae` | Game code (watcall) | prologue scan |
| `0x3e3ae`–`0x43de6` | Game graphics engine in assembly (`push ebp; mov ebp,esp; … push es; cld; push ds; pop es`, stack args) | called from game code (sprite/blit helpers) |
| `0x43de6`–`0x48d53` | Game code (watcall) | prologue scan |
| `0x47xxx`–`0x48d53` | CD audio via MSCDEX (`CD_SendMscdexIoctl` `0x48570`, `CD_SendAudioCommand` `0x486c6`) | int 2Fh AX=1510h via DPMI 0300h |
| `0x48d53`–`0x4cc60` | unidentified | — |
| `0x4cc60`–`0x58200` | Miles AIL 3 (cdecl, stack args), incl. its timer/DPMI layer `0x579a7`–`0x58036` | `DIG.INI`, `MDI.INI`, `AIL_startup()` strings; PIT + PIC EOI |
| `0x58081` | `WaitVerticalRetraces` (reads CRTC port from BIOS data area `0x463`) | |
| `0x58324`–`0x89f9f` | Game code (watcall), incl. `main` `0x7ad49` | prologue scan |
| `0x89fa0`–`0x8c040` | SciTech UVBELib | "Universal VESA VBE" strings, `UVBE_InitDriver` `0x8a3e4` |
| `0x8c041`–`0x8c0ff` | Watcom CRT pieces: `outp`, `inp`, `memset`, `sprintf`(`0x8c08b`) | |
| `0x8c100`–`0x8fdff` | Smacker player + its SVGA blitter, timer (`0x8f550`–`0x8fbd3`), palette (`0x8f410`) | callees of `PlaySmackerMovie` |
| `0x8fe00`–`0x95xxx` | Watcom C runtime incl. `_cstart_` `0x910b8`, `int386`-style `int N` stub table `0x929ab` | int 21h density, cstart |

## Platform surface (what the runner must emulate)

From a full scan of disassembled code:

- 108 `INT` sites: `int 21h` (DOS, mostly Watcom CRT), `int 31h` (DPMI: CRT,
  Miles, Smacker), `int 10h` (video BIOS: UVBELib, Smacker, CRT), `int 33h`
  (mouse, `0x8aafe`), plus two `int N; ret` stub tables (`0x8b327`,
  `0x929ab`) for interrupts chosen at run time.
- 33 port I/O instructions: PIT `0x40/0x43` (Miles, Smacker), PIC EOI
  `0x20` (Miles ISR), VGA palette `0x3c8/0x3c9`, VGA status `0x3da`
  (retrace waits), and Watcom `inp`/`outp` (callers `0x10a23`, `0x10a9d`).
- Direct memory: VGA window `0xa0000` (absolute, Smacker/UVBELib blitters),
  BIOS data area (`0x463`). DOS/4GW maps the first MB 1:1, so the runner
  must map a real VGA window at `0xa0000`, keep the game's objects out of the
  first MB, and emulate reads below `0x10000` (Linux `vm.mmap_min_addr`).

## Known addresses

| Address | Name | Notes |
|---|---|---|
| `0x910b8` | `_cstart_` | Watcom startup: extender detection (int 21h AX=3000h), command line, environment, BSS clear |
| `0x944aa` | `__CMain` (probable) | sets up stack bounds, calls `main(argc, argv)` |
| `0x7ad49` | `main` | argc/argv at `0xea030`/`0xea034`; `n` arg skips `UVBE_InitDriver` |
| `0x10b2e` | `PlaySmackerMovie` | builds `%c:\%s\dat\%d.smk`, plays it with Smacker |
| `0x8a3e4` | `UVBE_InitDriver` | loads the UVBELib driver from `VESA480.dll` |
| `0x8dd90` | `SMK_BlitFrameToVga` | writes the VGA window directly |
| `0x8f410` | `SMK_SetVgaPalette256` | 256× `out 3c8/3c9` |
| `0x57a48` | `SetPitChannel0Divisor` | cdecl |
| `0x58081` | `WaitVerticalRetraces` | cdecl, polls CRTC status |
| `0x9043d` | `kbhit` | Watcom CRT, int 21h AH=0Bh |
| `0x3a008`/`0x3a08b` | `PollKeyboard`/`FlushKeyboard` | kbhit + getch |
| `0x3a0b0` | `PollInput` | mouse state + keyboard; once per UI loop pass |
| `0x3cbfa` | `ReadMouseState` | copies mouse state from `0xd8440`-`0xd8450`; runner hook at `0x3cbff` |
| `0x6d968` | `UI_HitTest` | mouse vs. hotspot table `0xd02e9` (19-byte entries); 500 = none |
| `0x8fbc8` | `SMK_TimerMs` | Smacker clock from PIT channel 0 |
| `0x6ef53` | `ScrollBattleMapStep` | one edge-scroll step in direction `0xce233`; runner hook at `0x6ef5b` |
| `0x7f7b2` | `UpdateEdgeScrollDirection` | pointer in an edge zone -> `0xce233` (8 = none); zones are hotspots `0xe98da` left, `0xe98de` right (plus x < 3 / x > 636), `0xe98d9` top (y <= 35), `0xe98d8` bottom (y >= 435) |
| `0x7f899` | (in `UpdateEdgeScrollDirection`) epilogue | runner hook: arrow keys -> `0xce233` |
| `0x3b6ac` | `HexNeighbour(dir, &col, &row)` | 0 up ... 7 up-left (clockwise); diagonals change the row on odd/even columns |
| `0x3c17e` | `StepView(dir)` | moves the view copy `0xd7e88` one hex, clamped to the map |
| `0xd8440` | (data) game mouse state | copied by `ReadMouseState`; `0xd8448` x, `0xd844c` y (dwords, screen pixels) |
| `0x2be3a` | `RunBattleTurnLoop` (probable) | battle input loop |
| `0xcb243` | (data) tick counter | 100 Hz: Miles timer callback `0x39f4e` (registered in `0x38339`, `AIL_set_timer_frequency(t, 100)`) |
| `0x39f63` | `DelayTicks(n)` | spins until `0xcb243` advances by n (memory poll: no trap, not idle-detected) |
| `0x7b6cb` | `RunBattleTurns` (probable) | alternates sides (`0xe98dc`); a side with computer flag `0xce24c[side]` plays through `0x58324`, a human one through `0x73262` |
| `0x7a2d3` | `MoveUnitAnimated` | slides a unit one pixel per pass between hex centres; waits `DelayTicks(1)` per pixel only in modes 1/2 (`CL`, `[ebp-24h]`) or with state flag `0xe98af`; visible flag `[ebp-2Ch]`; runner hook at `0x7aa87`. Callers: AI `0x5b5cc` (mode 0), `0x71e83` (mode 0), `0x75557`/`0x6ab63` (mode 1), `0x876e8` (mode 2) |
| `0x43bd5` | `FadePalette` (probable) | steps the used colours towards a target palette via VESA480.DLL (`cb414` set colour, `cb404` wait for retrace); `0x800a8`/`0x800df` call it around turn changes |
| `0x3cd78` | mouse cursor timer callback | Miles timer at 60/70 Hz (registered at `0x3dc93`); draws the cursor even while the main loop is busy |
| `0x85a45` | `RedrawBattleMap` | whole map at view origin `0xe93d8` (x, y words; `0x7cc18` recentres on a hex via `0x3c2e0`); 60+ callers; runner hook at `0x85a4d` |
| `0xe98ae` | (data) saved battle state block | `fread`/`fwrite` from here in the save/load code (`0x82e0c`, `0x83793`, ...) |
| `0x8c041`/`0x8c04f`/`0x8c060` | `outp`/`inp`/`memset` | Watcom CRT |
| `0x904fe` | `int386` | Watcom CRT; used for DPMI 0300h calls |
| `0x3c7f5`/`0x3c8f4` | `MOUSE_CallDriverA/B` | int 33h via DPMI 0300h |
| `0x48570`/`0x486c6` | `CD_SendMscdexIoctl` / `CD_SendAudioCommand` | MSCDEX 1510h requests |

See `docs/RUNNER.md` for the full list of services the program uses.

## Ghidra coverage (after `tools/ghidra/import.sh`)

2,092 functions; 78.7% of obj1 inside functions, 91.4% disassembled.
Remaining undefined bytes in obj1 are mostly lookup tables, `int N; ret`
stub tables (dynamic interrupt calls), and a few switch tails.
