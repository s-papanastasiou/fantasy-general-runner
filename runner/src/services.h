// SPDX-License-Identifier: GPL-2.0-or-later
// Emulated DOS/DPMI/BIOS services and hardware, called from the trap handler.
#pragma once
#include <cstdint>

#include "machine.h"

namespace fg {

// Software interrupt issued by protected-mode game code (`int n`).
void dispatch_int(Cpu& cpu, uint8_t vec);

void int21_dos(Cpu& cpu);
void int31_dpmi(Cpu& cpu);

// Real-mode register structure used by DPMI 0300h (simulate real-mode interrupt).
#pragma pack(push, 1)
struct RealModeRegs {
    uint32_t edi, esi, ebp, reserved, ebx, edx, ecx, eax;
    uint16_t flags, es, ds, fs, gs, ip, cs, sp, ss;
};
#pragma pack(pop)
static_assert(sizeof(RealModeRegs) == 0x32);
// Emulate real-mode interrupt `vec` on the given registers. Returns false if unsupported.
bool real_mode_int(uint8_t vec, RealModeRegs& r);

// Hardware ports.
uint32_t port_in(uint16_t port, int size);
void port_out(uint16_t port, uint32_t value, int size);

// Default interrupt handlers and the interrupt-return trampoline in DOS memory.
void install_interrupt_stubs();
// Patch runner hooks into the loaded game code: idle detection in input loops,
// battle-map scroll steps at least `scroll_step_ms` apart (eight-way unless
// `original_scrolling`), the computer's
// unit moves at least `ai_step_ms` per pixel, and each map view of the
// computer's turn shown at least `ai_view_ms`.
void install_game_hooks(double scroll_step_ms, bool original_scrolling, double ai_step_ms, double ai_view_ms);

}  // namespace fg
