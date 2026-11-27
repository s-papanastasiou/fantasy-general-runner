// SPDX-License-Identifier: GPL-2.0-or-later
// Global emulator state and the CPU register view used by trap handlers.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "address_space.h"
#include "ldt.h"
#include "le_image.h"

namespace fg {

class DosFs;

// The game's registers as the trap handlers see and change them. Each CPU
// backend copies its saved context in before a handler runs and back after.
struct CpuRegs {
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp;
    uint32_t eip, eflags;
    uint16_t cs, ds, es, fs, gs, ss;
};

// Register accessors over a CpuRegs.
class Cpu {
public:
    explicit Cpu(CpuRegs& r) : r_(r) {}
    CpuRegs& regs() { return r_; }

#define FG_REG32(name) \
    uint32_t name() const { return r_.name; } \
    void set_##name(uint32_t v) { r_.name = v; }
    FG_REG32(eax) FG_REG32(ebx) FG_REG32(ecx) FG_REG32(edx)
    FG_REG32(esi) FG_REG32(edi) FG_REG32(ebp) FG_REG32(esp)
    FG_REG32(eip) FG_REG32(eflags)
#undef FG_REG32
#define FG_SREG(name) \
    uint16_t name() const { return r_.name; } \
    void set_##name(uint16_t v) { r_.name = v; }
    FG_SREG(cs) FG_SREG(ds) FG_SREG(es) FG_SREG(fs) FG_SREG(gs) FG_SREG(ss)
#undef FG_SREG

    uint16_t ax() const { return eax() & 0xFFFF; }
    uint16_t bx() const { return ebx() & 0xFFFF; }
    uint16_t cx() const { return ecx() & 0xFFFF; }
    uint16_t dx() const { return edx() & 0xFFFF; }
    uint16_t si() const { return esi() & 0xFFFF; }
    uint16_t di() const { return edi() & 0xFFFF; }
    uint8_t al() const { return eax() & 0xFF; }
    uint8_t ah() const { return (eax() >> 8) & 0xFF; }
    uint8_t bl() const { return ebx() & 0xFF; }
    uint8_t bh() const { return (ebx() >> 8) & 0xFF; }
    uint8_t cl() const { return ecx() & 0xFF; }
    uint8_t ch() const { return (ecx() >> 8) & 0xFF; }
    uint8_t dl() const { return edx() & 0xFF; }
    uint8_t dh() const { return (edx() >> 8) & 0xFF; }

    void set_ax(uint16_t v) { set_eax((eax() & 0xFFFF0000u) | v); }
    void set_bx(uint16_t v) { set_ebx((ebx() & 0xFFFF0000u) | v); }
    void set_cx(uint16_t v) { set_ecx((ecx() & 0xFFFF0000u) | v); }
    void set_dx(uint16_t v) { set_edx((edx() & 0xFFFF0000u) | v); }
    void set_si(uint16_t v) { set_esi((esi() & 0xFFFF0000u) | v); }
    void set_di(uint16_t v) { set_edi((edi() & 0xFFFF0000u) | v); }
    void set_al(uint8_t v) { set_eax((eax() & 0xFFFFFF00u) | v); }
    void set_ah(uint8_t v) { set_eax((eax() & 0xFFFF00FFu) | (uint32_t(v) << 8)); }
    void set_bl(uint8_t v) { set_ebx((ebx() & 0xFFFFFF00u) | v); }
    void set_bh(uint8_t v) { set_ebx((ebx() & 0xFFFF00FFu) | (uint32_t(v) << 8)); }
    void set_cl(uint8_t v) { set_ecx((ecx() & 0xFFFFFF00u) | v); }
    void set_ch(uint8_t v) { set_ecx((ecx() & 0xFFFF00FFu) | (uint32_t(v) << 8)); }
    void set_dl(uint8_t v) { set_edx((edx() & 0xFFFFFF00u) | v); }
    void set_dh(uint8_t v) { set_edx((edx() & 0xFFFF00FFu) | (uint32_t(v) << 8)); }

    static constexpr uint32_t kCF = 1u << 0, kZF = 1u << 6, kIF = 1u << 9;
    bool flag(uint32_t f) const { return (eflags() & f) != 0; }
    void set_flag(uint32_t f, bool on) { set_eflags(on ? (eflags() | f) : (eflags() & ~f)); }
    void set_cf(bool on) { set_flag(kCF, on); }

private:
    CpuRegs& r_;
};

struct Machine {
    AddressSpace as;
    Ldt ldt;
    LeImage image;
    std::unique_ptr<DosFs> fs;

    uint16_t code_sel = 0, data_sel = 0, psp_sel = 0, env_sel = 0;
    uint16_t psp_seg = 0;
    bool virtual_if = true;

    // Interrupt vectors the game installs.
    struct FarPtr32 { uint16_t sel = 0; uint32_t off = 0; };
    FarPtr32 pm_vectors[256];
    uint32_t rm_vectors[256] = {};  // seg:off

    int exit_code = 0;  // set by game_exit()

    // Linear address of selector:offset (game linear).
    uint32_t linear(uint16_t sel, uint32_t off) const { return ldt.base(sel) + off; }
    // Ghidra address for messages (or the raw linear address).
    uint32_t ghidra(uint32_t lin) const { uint32_t g = image.to_ghidra(lin); return g == ~0u ? lin : g; }
    std::string read_cstring(uint32_t lin, size_t max = 260) const;
};

extern Machine* g_machine;

[[noreturn]] void fatal_in_game(Cpu& cpu, const char* fmt, ...);
[[noreturn]] void game_exit(int code);

}  // namespace fg
