// SPDX-License-Identifier: GPL-2.0-or-later
// int 31h (DPMI 0.9 host, as provided by DOS/4GW) and the real-mode services
// reached through it (DPMI 0300h simulate real-mode interrupt).
#include <cstring>
#include <ctime>
#include <map>

#include "cdrom.h"
#include "dig_driver.h"
#include "input.h"
#include "log.h"
#include "services.h"
#include "video.h"

namespace fg {

namespace {

void ok(Cpu& cpu) { cpu.set_cf(false); }
void fail(Cpu& cpu, uint16_t err) {
    cpu.set_ax(err);
    cpu.set_cf(true);
}

constexpr uint16_t kErrUnsupported = 0x8001, kErrNoDescriptors = 0x8011, kErrNoMemory = 0x8013,
                   kErrInvalidSelector = 0x8022, kErrInvalidHandle = 0x8023, kErrInvalidValue = 0x8021;

// DOS memory blocks: selector -> segment (DPMI 0100h).
std::map<uint16_t, uint16_t> g_dos_blocks;

// Real-mode callbacks (DPMI 0303h): fake real-mode address -> protected-mode target.
struct Callback {
    uint16_t sel;
    uint32_t off;
    uint16_t regs_sel;
    uint32_t regs_off;
};
std::map<uint32_t, Callback> g_callbacks;
uint16_t g_next_callback = 0;

uint32_t pair(uint16_t hi, uint16_t lo) { return (uint32_t(hi) << 16) | lo; }

}  // namespace

void int31_dpmi(Cpu& cpu) {
    Machine& m = *g_machine;
    Ldt& ldt = m.ldt;
    uint16_t ax = cpu.ax();

    switch (ax) {
    case 0x0000: {  // allocate descriptors
        uint16_t sel = ldt.alloc(cpu.cx() ? cpu.cx() : 1);
        if (!sel) { fail(cpu, kErrNoDescriptors); return; }
        cpu.set_ax(sel);
        ok(cpu);
        return;
    }
    case 0x0001: if (ldt.free(cpu.bx())) ok(cpu); else fail(cpu, kErrInvalidSelector); return;
    case 0x0002: {  // segment to descriptor
        uint16_t sel = ldt.make(uint32_t(cpu.bx()) << 4, 0xFFFF, false);
        if (!sel) { fail(cpu, kErrNoDescriptors); return; }
        cpu.set_ax(sel);
        ok(cpu);
        return;
    }
    case 0x0003: cpu.set_ax(8); ok(cpu); return;  // selector increment
    case 0x0006: {
        if (!ldt.valid(cpu.bx())) { fail(cpu, kErrInvalidSelector); return; }
        uint32_t b = ldt.base(cpu.bx());
        cpu.set_cx(static_cast<uint16_t>(b >> 16));
        cpu.set_dx(static_cast<uint16_t>(b));
        ok(cpu);
        return;
    }
    case 0x0007: if (ldt.set_base(cpu.bx(), pair(cpu.cx(), cpu.dx()))) ok(cpu); else fail(cpu, kErrInvalidSelector); return;
    case 0x0008: if (ldt.set_limit(cpu.bx(), pair(cpu.cx(), cpu.dx()))) ok(cpu); else fail(cpu, kErrInvalidSelector); return;
    case 0x0009: if (ldt.set_rights(cpu.bx(), cpu.cl(), cpu.ch())) ok(cpu); else fail(cpu, kErrInvalidSelector); return;
    case 0x000A: {  // create alias (data) descriptor
        if (!ldt.valid(cpu.bx())) { fail(cpu, kErrInvalidSelector); return; }
        const Descriptor& d = ldt.get(cpu.bx());
        uint16_t sel = ldt.make(d.base, d.limit, false);
        if (!sel) { fail(cpu, kErrNoDescriptors); return; }
        cpu.set_ax(sel);
        ok(cpu);
        return;
    }

    case 0x0100: {  // allocate DOS memory: BX paragraphs -> AX segment, DX selector
        uint32_t largest = 0;
        uint16_t seg = m.as.conv_alloc(cpu.bx(), &largest);
        if (!seg) { cpu.set_bx(static_cast<uint16_t>(largest)); fail(cpu, 8); return; }
        uint16_t sel = ldt.make(uint32_t(seg) << 4, uint32_t(cpu.bx()) * 16 - 1, false);
        g_dos_blocks[sel] = seg;
        cpu.set_ax(seg);
        cpu.set_dx(sel);
        ok(cpu);
        return;
    }
    case 0x0101: {
        auto it = g_dos_blocks.find(cpu.dx());
        if (it == g_dos_blocks.end()) { fail(cpu, 9); return; }
        m.as.conv_free(it->second);
        ldt.free(it->first);
        g_dos_blocks.erase(it);
        ok(cpu);
        return;
    }
    case 0x0102: {
        auto it = g_dos_blocks.find(cpu.dx());
        uint32_t largest = 0;
        if (it == g_dos_blocks.end()) { fail(cpu, 9); return; }
        if (!m.as.conv_resize(it->second, cpu.bx(), &largest)) { cpu.set_bx(static_cast<uint16_t>(largest)); fail(cpu, 8); return; }
        ldt.set_limit(cpu.dx(), uint32_t(cpu.bx()) * 16 - 1);
        ok(cpu);
        return;
    }

    case 0x0200: {  // get real-mode vector
        uint32_t v = m.rm_vectors[cpu.bl()];
        cpu.set_cx(static_cast<uint16_t>(v >> 16));
        cpu.set_dx(static_cast<uint16_t>(v));
        ok(cpu);
        return;
    }
    case 0x0201: m.rm_vectors[cpu.bl()] = pair(cpu.cx(), cpu.dx()); ok(cpu); return;
    case 0x0204:
        cpu.set_cx(m.pm_vectors[cpu.bl()].sel);
        cpu.set_edx(m.pm_vectors[cpu.bl()].off);
        ok(cpu);
        return;
    case 0x0205:
        FG_LOG("game installs protected-mode handler for int %02xh at %04x:%08x (ghidra %08x)", cpu.bl(), cpu.cx(),
               cpu.edx(), m.ghidra(m.linear(cpu.cx(), cpu.edx())));
        m.pm_vectors[cpu.bl()] = {cpu.cx(), cpu.edx()};
        ok(cpu);
        return;

    case 0x0300: case 0x0301: case 0x0302: {  // simulate real-mode interrupt / far call
        uint32_t lin = m.linear(cpu.es(), cpu.edi());
        RealModeRegs r;
        std::memcpy(&r, m.as.host(lin), sizeof r);
        if (ax == 0x0301 && r.cs == 0xF000 && r.ip == 0x2000) {  // VBE window function (bank switch)
            if (((r.ebx >> 8) & 0xFF) == 0) g_video.set_bank(r.edx & 0xFFFF);
            std::memcpy(m.as.host(lin), &r, sizeof r);
            ok(cpu);
            return;
        }
        if (ax != 0x0300 || !real_mode_int(cpu.bl(), r))
            fatal_in_game(cpu, "unsupported DPMI %04xh: real-mode int %02xh AX=%04x", ax, cpu.bl(), r.eax & 0xFFFF);
        std::memcpy(m.as.host(lin), &r, sizeof r);
        ok(cpu);
        return;
    }
    case 0x0303: {  // allocate real-mode callback: DS:ESI target, ES:EDI register struct
        uint32_t addr = pair(0xF000, static_cast<uint16_t>(0x1000 + g_next_callback++ * 4));
        g_callbacks[addr] = {cpu.ds(), cpu.esi(), cpu.es(), cpu.edi()};
        FG_LOG("game allocates real-mode callback %04x:%04x -> %04x:%08x (ghidra %08x)", addr >> 16, addr & 0xFFFF,
               cpu.ds(), cpu.esi(), m.ghidra(m.linear(cpu.ds(), cpu.esi())));
        cpu.set_cx(static_cast<uint16_t>(addr >> 16));
        cpu.set_dx(static_cast<uint16_t>(addr));
        ok(cpu);
        return;
    }
    case 0x0304: g_callbacks.erase(pair(cpu.cx(), cpu.dx())); ok(cpu); return;

    case 0x0400: cpu.set_ax(0x005A); cpu.set_bx(0x0005); cpu.set_cl(4); cpu.set_dx(0x0870); ok(cpu); return;

    case 0x0500: {  // free memory information
        uint32_t lin = m.linear(cpu.es(), cpu.edi());
        uint32_t free_bytes = m.as.heap_free_bytes();
        uint32_t info[12];
        for (auto& v : info) v = 0xFFFFFFFF;
        info[0] = free_bytes;          // largest available block
        info[1] = free_bytes / 4096;   // max unlocked page allocation
        info[2] = free_bytes / 4096;   // max locked page allocation
        info[3] = AddressSpace::kSize / 4096;
        info[4] = free_bytes / 4096;
        info[5] = free_bytes / 4096;
        info[6] = AddressSpace::kSize / 4096;
        std::memcpy(m.as.host(lin), info, sizeof info);
        ok(cpu);
        return;
    }
    case 0x0501: {  // allocate memory block: BX:CX size -> BX:CX linear, SI:DI handle
        uint32_t lin = m.as.heap_alloc(pair(cpu.bx(), cpu.cx()));
        if (!lin) { fail(cpu, kErrNoMemory); return; }
        cpu.set_bx(static_cast<uint16_t>(lin >> 16)); cpu.set_cx(static_cast<uint16_t>(lin));
        cpu.set_si(static_cast<uint16_t>(lin >> 16)); cpu.set_di(static_cast<uint16_t>(lin));
        ok(cpu);
        return;
    }
    case 0x0502: if (m.as.heap_free(pair(cpu.si(), cpu.di()))) ok(cpu); else fail(cpu, kErrInvalidHandle); return;
    case 0x0503: {
        uint32_t lin = m.as.heap_resize(pair(cpu.si(), cpu.di()), pair(cpu.bx(), cpu.cx()));
        if (!lin) { fail(cpu, kErrNoMemory); return; }
        cpu.set_bx(static_cast<uint16_t>(lin >> 16)); cpu.set_cx(static_cast<uint16_t>(lin));
        cpu.set_si(static_cast<uint16_t>(lin >> 16)); cpu.set_di(static_cast<uint16_t>(lin));
        ok(cpu);
        return;
    }
    case 0x0600: case 0x0601: case 0x0602: case 0x0603: ok(cpu); return;  // lock/unlock: no-ops
    case 0x0604: cpu.set_bx(0); cpu.set_cx(4096); ok(cpu); return;      // page size
    case 0x0702: case 0x0703: ok(cpu); return;                          // paging hints
    case 0x0800: {  // physical address mapping: BX:CX physical, SI:DI size
        uint32_t phys = pair(cpu.bx(), cpu.cx());
        if (phys < AddressSpace::kDosTop) { ok(cpu); return; }  // first MB is identity mapped
        fail(cpu, kErrInvalidValue);  // no linear frame buffer offered (yet)
        return;
    }
    case 0x0801: ok(cpu); return;
    case 0x0900: cpu.set_al(m.virtual_if); m.virtual_if = false; ok(cpu); return;
    case 0x0901: cpu.set_al(m.virtual_if); m.virtual_if = true; ok(cpu); return;
    case 0x0902: cpu.set_al(m.virtual_if); ok(cpu); return;
    default: break;
    }
    fatal_in_game(cpu, "unsupported DPMI function %04xh", ax);
}

// ---------------------------------------------------------------------------
// Real-mode services (reached via DPMI 0300h or reflected protected-mode ints)

bool real_mode_int(uint8_t vec, RealModeRegs& r) {
    uint16_t ax = r.eax & 0xFFFF;
    FG_TRACE("real-mode int %02xh AX=%04x BX=%04x CX=%04x DX=%04x", vec, ax, r.ebx & 0xFFFF, r.ecx & 0xFFFF,
             r.edx & 0xFFFF);
    switch (vec) {
    case 0x10: return g_video.int10(r);
    case 0x2F:
        if ((ax >> 8) == 0x15) return g_cdrom.mscdex(r);  // MSCDEX
        break;
    case 0x33: return g_input.int33(r);
    case 0x16: return g_input.int16(r);
    case 0x66: if (g_dig.call(r)) return true; break;  // Miles digital sound driver
    default: break;
    }
    FG_LOG("unsupported real-mode int %02xh AX=%04x", vec, ax);
    return false;
}

}  // namespace fg
