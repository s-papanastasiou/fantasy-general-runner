// SPDX-License-Identifier: GPL-2.0-or-later
// The CPU backend: how the game's code is executed.
//
// The game runs in the Unicorn x86 emulator (cpu_unicorn.cpp); its
// descriptor tables live in emulator memory. The emulator hands events to
// the trap logic in traps.cpp: general-protection faults on `int`,
// `in`/`out`, `hlt` and the like, and timer ticks. Everything above (DOS,
// DPMI, devices, UI) only sees these functions.
#pragma once
#include <cstdint>

#include "ldt.h"
#include "machine.h"

namespace fg {

// Once, after AddressSpace::init() and before any descriptor is made.
bool cpu_init();
// The game thread: per-thread setup, then run until the game exits.
// Returns the exit code passed to game_exit().
bool cpu_thread_init();
int cpu_run(const CpuRegs& start);
// Leave cpu_run() from inside a trap handler on the game thread.
[[noreturn]] void cpu_exit_game();

// Descriptor table entry `index` changed (Ldt keeps the master copy).
bool cpu_set_descriptor(int index, const Descriptor& d);
bool cpu_clear_descriptor(int index);
// Code at [lin, lin+len) was written by the runner (file reads, freed
// memory): drop the emulator's translation of it.
void cpu_code_written(uint32_t lin, uint32_t len);

// Hardware timer (IRQ0) on the game thread, at `hz` ticks per second.
bool start_timer(double hz);
void set_timer_rate(double hz);

// Trap logic (traps.cpp), called by the emulator.
// A general-protection fault at CS:EIP (the faulting instruction).
void handle_gp(Cpu& cpu);
// A timer tick arrived (any thread, async-signal-safe): count it.
void timer_tick();
bool timer_pending();
// Deliver counted timer ticks if the game can take an interrupt now.
void deliver_pending(Cpu& cpu);

}  // namespace fg
