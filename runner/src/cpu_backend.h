// SPDX-License-Identifier: GPL-2.0-or-later
// The CPU backend: how the game's code is executed.
//
// Two implementations, chosen at build time (CMake option FG_CPU):
//   native  (cpu_native.cpp)  the game runs directly on the host CPU, in a
//           32-bit Linux process; privileged instructions trap as signals
//           and the game's segments are real LDT entries (modify_ldt).
//   unicorn (cpu_unicorn.cpp) the game runs in the Unicorn x86 emulator;
//           any 64-bit host. Its descriptor tables live in emulator memory.
//
// Both hand the same events to the shared trap logic in traps.cpp:
// general-protection faults on `int`, `in`/`out`, `cli`/`sti`, `hlt`, and
// timer ticks. Everything above (DOS, DPMI, devices, UI) is backend-neutral.
#pragma once
#include <cstdint>

#include "ldt.h"
#include "machine.h"

namespace fg {

#if defined(FG_CPU_UNICORN)
constexpr const char* kCpuBackendName = "unicorn";
constexpr bool kCpuMapsAllMemory = true;  // for AddressSpace::init
#else
constexpr const char* kCpuBackendName = "native";
constexpr bool kCpuMapsAllMemory = false;
#endif

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
// memory): drop any translation of it (emulator backends only).
void cpu_code_written(uint32_t lin, uint32_t len);

// Hardware timer (IRQ0) on the game thread, at `hz` ticks per second.
bool start_timer(double hz);
void set_timer_rate(double hz);

// Shared trap logic (traps.cpp), called by the backends.
// A general-protection fault at CS:EIP (the faulting instruction).
void handle_gp(Cpu& cpu);
// A timer tick arrived (any thread, async-signal-safe): count it.
void timer_tick();
bool timer_pending();
// Deliver counted timer ticks if the game can take an interrupt now.
void deliver_pending(Cpu& cpu);

}  // namespace fg
