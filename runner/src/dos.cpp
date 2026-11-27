// SPDX-License-Identifier: GPL-2.0-or-later
// int 21h (DOS, as extended by DOS/4GW for 32-bit callers) and the
// software-interrupt dispatcher.
#include <ctime>
#include <algorithm>
#include <cstring>

#include "dos_fs.h"
#include "cpu_backend.h"
#include "idle.h"
#include "input.h"
#include "log.h"
#include "platform.h"
#include "services.h"

namespace fg {

namespace {

IdleDetector g_key_idle;

// The game polled for a key and none was waiting.
void no_key_waiting() {
    if (g_key_idle.spinning()) g_key_idle.sleep_ns(1000000);
}

// Disk transfer area for find first/next.
uint16_t g_dta_sel = 0;
uint32_t g_dta_off = 0x80;

void ok(Cpu& cpu) { cpu.set_cf(false); }
void fail(Cpu& cpu, uint16_t err) {
    cpu.set_ax(err);
    cpu.set_cf(true);
}

uint32_t ds_edx(Cpu& cpu) { return g_machine->linear(cpu.ds(), cpu.edx()); }

void fill_dta(const FindResult& r) {
    Machine& m = *g_machine;
    uint32_t dta = m.linear(g_dta_sel, g_dta_off);
    uint8_t* p = m.as.host(dta);
    p[0x15] = r.attr;
    std::memcpy(p + 0x16, &r.time, 2);
    std::memcpy(p + 0x18, &r.date, 2);
    std::memcpy(p + 0x1A, &r.size, 4);
    std::memset(p + 0x1E, 0, 13);
    std::memcpy(p + 0x1E, r.name.data(), std::min<size_t>(r.name.size(), 12));
}

// Call a protected-mode handler the game installed, as if `int vec` reached it
// through the IDT: push an iret frame on the game stack and jump.
void call_pm_vector(Cpu& cpu, uint8_t vec) {
    Machine& m = *g_machine;
    const auto& v = m.pm_vectors[vec];
    uint32_t sp = cpu.esp() - 12;
    uint32_t lin = m.linear(cpu.ss(), sp);
    m.as.write<uint32_t>(lin, cpu.eip());
    m.as.write<uint32_t>(lin + 4, cpu.cs());
    m.as.write<uint32_t>(lin + 8, cpu.eflags());
    cpu.set_esp(sp);
    cpu.set_cs(v.sel);
    cpu.set_eip(v.off);
}

// int 10h/16h/1Ah/2Fh/33h issued from protected mode: DOS/4GW reflects these to
// real mode with the registers unchanged (no pointer translation needed for
// the functions this program uses this way).
void reflect_to_real_mode(Cpu& cpu, uint8_t vec) {
    RealModeRegs r{};
    r.eax = cpu.eax(); r.ebx = cpu.ebx(); r.ecx = cpu.ecx(); r.edx = cpu.edx();
    r.esi = cpu.esi(); r.edi = cpu.edi(); r.ebp = cpu.ebp();
    r.flags = static_cast<uint16_t>(cpu.eflags());
    if (!real_mode_int(vec, r)) fatal_in_game(cpu, "unsupported int %02xh AX=%04x", vec, cpu.ax());
    cpu.set_eax(r.eax); cpu.set_ebx(r.ebx); cpu.set_ecx(r.ecx); cpu.set_edx(r.edx);
    cpu.set_esi(r.esi); cpu.set_edi(r.edi); cpu.set_ebp(r.ebp);
    cpu.set_cf(r.flags & 1);
    cpu.set_flag(Cpu::kZF, r.flags & 0x40);
}

}  // namespace

void dispatch_int(Cpu& cpu, uint8_t vec) {
    FG_TRACE("int %02xh AX=%04x at %08x", vec, cpu.ax(), g_machine->ghidra(g_machine->linear(cpu.cs(), cpu.eip())));
    switch (vec) {
    case 0x21: int21_dos(cpu); return;
    case 0x31: int31_dpmi(cpu); return;
    default: break;
    }
    // A protected-mode handler the game installed takes the interrupt;
    // otherwise DOS/4GW's default reflects it to (emulated) real mode.
    if (g_machine->pm_vectors[vec].off >= 0x1000) { call_pm_vector(cpu, vec); return; }
    switch (vec) {
    case 0x10: case 0x16: case 0x1A: case 0x2F: case 0x33:
        reflect_to_real_mode(cpu, vec);
        return;
    default:
        fatal_in_game(cpu, "unsupported software interrupt %02xh (AX=%04x)", vec, cpu.ax());
    }
}

void int21_dos(Cpu& cpu) {
    Machine& m = *g_machine;
    DosFs& fs = *m.fs;
    if (!g_dta_sel) g_dta_sel = m.psp_sel;
    uint8_t ah = cpu.ah();

    switch (ah) {
    case 0x01: case 0x07: case 0x08: {                            // read key (waits)
        // Blocking inside the trap handler would stall timer interrupts, so
        // return to the game and retry the instruction until a key arrives.
        if (!g_input.dos_has_char()) { no_key_waiting(); cpu.set_eip(cpu.eip() - 2); return; }
        cpu.set_al(g_input.dos_read_char());
        return;
    }
    case 0x06:                                                    // direct console I/O
        if (cpu.dl() != 0xFF) { char c = static_cast<char>(cpu.dl()); fs.write(1, &c, 1); return; }
        if (g_input.dos_has_char()) { cpu.set_al(g_input.dos_read_char()); cpu.set_flag(Cpu::kZF, false); }
        else { no_key_waiting(); cpu.set_al(0); cpu.set_flag(Cpu::kZF, true); }
        return;
    case 0x02: { char c = static_cast<char>(cpu.dl()); fs.write(1, &c, 1); return; }
    case 0x09: {                                                  // print $-terminated string
        uint32_t p = ds_edx(cpu);
        std::string s;
        while (s.size() < 4096 && *m.as.host(p) != '$') s += static_cast<char>(*m.as.host(p++));
        fs.write(1, s.data(), s.size());
        return;
    }
    case 0x0B:                                                    // key waiting? (kbhit)
        if (g_input.dos_has_char()) cpu.set_al(0xFF);
        else { no_key_waiting(); cpu.set_al(0x00); }
        return;
    case 0x0E:                                                    // select drive
        fs.set_current_drive(static_cast<char>('A' + cpu.dl()));
        cpu.set_al(26);
        return;
    case 0x19: cpu.set_al(static_cast<uint8_t>(fs.current_drive() - 'A')); return;
    case 0x1A: g_dta_sel = cpu.ds(); g_dta_off = cpu.edx(); return;
    case 0x25: m.pm_vectors[cpu.al()] = {cpu.ds(), cpu.edx()}; return;
    case 0x2A: {
        time_t t = time(nullptr);
        struct tm tm;
        local_time(t, tm);
        cpu.set_cx(static_cast<uint16_t>(tm.tm_year + 1900));
        cpu.set_dh(static_cast<uint8_t>(tm.tm_mon + 1));
        cpu.set_dl(static_cast<uint8_t>(tm.tm_mday));
        cpu.set_al(static_cast<uint8_t>(tm.tm_wday));
        return;
    }
    case 0x2C: {
        timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm;
        local_time(ts.tv_sec, tm);
        cpu.set_ch(static_cast<uint8_t>(tm.tm_hour));
        cpu.set_cl(static_cast<uint8_t>(tm.tm_min));
        cpu.set_dh(static_cast<uint8_t>(tm.tm_sec));
        cpu.set_dl(static_cast<uint8_t>(ts.tv_nsec / 10000000));
        return;
    }
    case 0x2F: cpu.set_es(g_dta_sel); cpu.set_ebx(g_dta_off); return;
    case 0x30: cpu.set_eax(0x0005); cpu.set_bx(0); cpu.set_cx(0); return;  // DOS 5.0
    case 0x35: cpu.set_es(m.pm_vectors[cpu.al()].sel); cpu.set_ebx(m.pm_vectors[cpu.al()].off); return;
    case 0x37:
        if (cpu.al() == 0) { cpu.set_al(0); cpu.set_dl('/'); }
        else cpu.set_al(0xFF);
        return;
    case 0x3B: {
        uint16_t e = fs.chdir(m.read_cstring(ds_edx(cpu)));
        if (e) fail(cpu, e); else ok(cpu);
        return;
    }
    case 0x3C: case 0x3D: {
        std::string name = m.read_cstring(ds_edx(cpu));
        int h = ah == 0x3C ? fs.create(name) : fs.open(name, cpu.al());
        FG_TRACE("%s \"%s\" -> %d", ah == 0x3C ? "create" : "open", name.c_str(), h);
        if (h < 0) fail(cpu, static_cast<uint16_t>(-h)); else { cpu.set_eax(h); ok(cpu); }
        return;
    }
    case 0x3E: { uint16_t e = fs.close(cpu.bx()); if (e) fail(cpu, e); else ok(cpu); return; }
    case 0x3F: case 0x40: {
        uint32_t buf = ds_edx(cpu), n = cpu.ecx();
        if (!m.as.contains(buf, n)) { fail(cpu, doserr::kAccessDenied); return; }
        long r = ah == 0x3F ? fs.read(cpu.bx(), m.as.host(buf), n) : fs.write(cpu.bx(), m.as.host(buf), n);
        if (ah == 0x3F && r > 0) cpu_code_written(buf, static_cast<uint32_t>(r));
        if (r < 0) fail(cpu, static_cast<uint16_t>(-r)); else { cpu.set_eax(static_cast<uint32_t>(r)); ok(cpu); }
        return;
    }
    case 0x41: { uint16_t e = fs.unlink(m.read_cstring(ds_edx(cpu))); if (e) fail(cpu, e); else ok(cpu); return; }
    case 0x42: {
        int32_t off = static_cast<int32_t>((uint32_t(cpu.cx()) << 16) | cpu.dx());
        long r = fs.seek(cpu.bx(), off, cpu.al());
        if (r < 0) { fail(cpu, static_cast<uint16_t>(-r)); return; }
        cpu.set_ax(static_cast<uint16_t>(r));
        cpu.set_dx(static_cast<uint16_t>(r >> 16));
        ok(cpu);
        return;
    }
    case 0x43: {
        std::string host;
        if (uint16_t e = fs.resolve(m.read_cstring(ds_edx(cpu)), host)) { fail(cpu, e); return; }
        if (cpu.al() == 0) cpu.set_cx(0x20);
        ok(cpu);
        return;
    }
    case 0x44: {
        int h = cpu.bx();
        switch (cpu.al()) {
        case 0x00:                                  // get device information
            if (!fs.is_open(h)) { fail(cpu, doserr::kInvalidHandle); return; }
            cpu.set_dx(fs.is_device(h) ? (h == 0 ? 0x80C1 : h <= 2 ? 0x80C2 : 0x80C0)
                                       : static_cast<uint16_t>(fs.current_drive() - 'A'));
            ok(cpu);
            return;
        case 0x01: ok(cpu); return;                 // set device information
        case 0x06: cpu.set_al(fs.is_device(h) ? 0x00 : 0xFF); ok(cpu); return;
        case 0x07: cpu.set_al(0xFF); ok(cpu); return;
        case 0x08: cpu.set_ax(1); ok(cpu); return;  // not removable
        case 0x09: cpu.set_dx(0); ok(cpu); return;  // local drive
        case 0x0A: cpu.set_dx(0); ok(cpu); return;  // local handle
        default: break;
        }
        break;
    }
    case 0x47: {                                    // get current directory
        char drive = cpu.dl() ? static_cast<char>('A' + cpu.dl() - 1) : fs.current_drive();
        std::string c = fs.cwd(drive);
        uint32_t dst = m.linear(cpu.ds(), cpu.esi());
        std::memcpy(m.as.host(dst), c.c_str(), c.size() + 1);
        ok(cpu);
        return;
    }
    case 0x4C: game_exit(cpu.al());
    case 0x4E: case 0x4F: {
        FindResult r;
        uint32_t dta = m.linear(g_dta_sel, g_dta_off);
        uint16_t e = ah == 0x4E ? fs.find_first(dta, m.read_cstring(ds_edx(cpu)), cpu.cl(), r)
                                : fs.find_next(dta, r);
        if (e) { fail(cpu, e); return; }
        fill_dta(r);
        ok(cpu);
        return;
    }
    case 0x56: {
        uint16_t e = fs.rename(m.read_cstring(ds_edx(cpu)), m.read_cstring(m.linear(cpu.es(), cpu.edi())));
        if (e) fail(cpu, e); else ok(cpu);
        return;
    }
    case 0x57:
        if (cpu.al() == 0) { cpu.set_cx(0); cpu.set_dx((16 << 9) | (1 << 5) | 1); }
        ok(cpu);
        return;
    case 0x62: cpu.set_ebx(m.psp_sel); return;
    case 0xFF:
        if (cpu.al() == 0x00 && cpu.dx() == 0x78) {  // DOS/4GW presence check
            cpu.set_eax(0xFFFF3447);
            return;
        }
        break;
    default: break;
    }
    fatal_in_game(cpu, "unsupported int 21h AX=%04x", cpu.ax());
}

}  // namespace fg
