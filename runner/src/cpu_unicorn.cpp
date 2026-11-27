// SPDX-License-Identifier: GPL-2.0-or-later
// Unicorn CPU backend: the game runs in the Unicorn x86 emulator (GPL-2.0),
// so the runner works as an ordinary 64-bit program on any host.
//
// Emulator physical memory = game linear memory: the whole AddressSpace
// region is mapped at physical 0 (paging stays off), so services keep direct
// access through AddressSpace::host(). A small system area above it holds
// the GDT, the LDT (kept in step with fg::Ldt) and a ring-0 stub.
//
// The game runs at CPL 3 with its own LDT selectors. `int n` arrives at the
// interrupt hook, which hands it to the shared trap logic (traps.cpp) on a
// copy of the registers; `in`/`out` have instruction hooks of their own.
//
// Unlike the native backend, IOPL is 3: `cli`/`sti` run in the emulator and
// Machine::virtual_if follows the emulated IF. #GP must not be used for
// routine work here: QEMU only clears its "exception in progress" state when
// it delivers an exception itself, so after one hooked #GP the next fault is
// a double fault, and the one after that a triple fault.
//
// The timer runs on its own thread and only counts ticks; a block hook stops
// the emulator when one can be delivered, and cpu_run delivers it.
#include <unicorn/unicorn.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include "cpu_backend.h"
#include "log.h"
#include "machine.h"
#include "services.h"

namespace fg {

namespace {

// System area (emulator physical), just above the game's address space.
constexpr uint64_t kSysBase = AddressSpace::kSize;
constexpr uint32_t kSysSize = 0x20000;
constexpr uint32_t kGdtOff = 0x0000;        // 5 entries
constexpr uint32_t kStubOff = 0x0100;       // ring-0 entry stub: iretd
constexpr uint32_t kRing0StackOff = 0x1000; // grows down from here
constexpr uint32_t kLdtOff = 0x10000;       // Ldt::kEntries * 8 = 64 KB
constexpr uint16_t kSelRing0Code = 0x08, kSelRing0Data = 0x10, kSelLdt = 0x18;
constexpr uint64_t kNeverStop = ~0ull;  // `until` address that is never reached

// QEMU's internal descriptor flags (the descriptor's high dword).
constexpr uint32_t kDescP = 1u << 15;
constexpr uint32_t kDescTypeShift = 8;

alignas(4096) uint8_t g_sys[kSysSize];
uc_engine* g_uc = nullptr;
// Leaving the game jumps out of a Unicorn hook, across emulator and JIT
// frames. The builtin setjmp/longjmp do not unwind (a plain longjmp does on
// 64-bit Windows, and fails on the JIT frames, which have no unwind data).
void* g_exit_jmp[5];

std::atomic<double> g_timer_hz{0};
std::atomic<bool> g_timer_running{false};

uint32_t g_invalid_addr = 0;
int g_invalid_type = 0;

constexpr int kNumRegs = 16;
const int kRegIds[kNumRegs] = {
    UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_ESI, UC_X86_REG_EDI,
    UC_X86_REG_EBP, UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_EFLAGS, UC_X86_REG_CS, UC_X86_REG_DS,
    UC_X86_REG_ES,  UC_X86_REG_FS,  UC_X86_REG_GS,  UC_X86_REG_SS};
constexpr int kFirstSeg = 10;

void check(uc_err err, const char* what) {
    if (err != UC_ERR_OK) {
        FG_LOG("unicorn: %s: %s", what, uc_strerror(err));
        std::abort();
    }
}

CpuRegs read_regs() {
    uint64_t v[kNumRegs] = {};
    void* ptrs[kNumRegs];
    for (int i = 0; i < kNumRegs; ++i) ptrs[i] = &v[i];
    check(uc_reg_read_batch(g_uc, const_cast<int*>(kRegIds), ptrs, kNumRegs), "read registers");
    CpuRegs r;
    r.eax = uint32_t(v[0]); r.ebx = uint32_t(v[1]); r.ecx = uint32_t(v[2]); r.edx = uint32_t(v[3]);
    r.esi = uint32_t(v[4]); r.edi = uint32_t(v[5]); r.ebp = uint32_t(v[6]); r.esp = uint32_t(v[7]);
    r.eip = uint32_t(v[8]); r.eflags = uint32_t(v[9]);
    r.cs = uint16_t(v[10]); r.ds = uint16_t(v[11]); r.es = uint16_t(v[12]); r.fs = uint16_t(v[13]);
    r.gs = uint16_t(v[14]); r.ss = uint16_t(v[15]);
    return r;
}

// Write back what a handler changed. Segment registers are loaded (and
// checked against their descriptors) only when they changed; SS before CS so
// that a privilege mismatch can never be observed in between.
void write_regs(const CpuRegs& r, const CpuRegs& old) {
    uint32_t gp[10] = {r.eax, r.ebx, r.ecx, r.edx, r.esi, r.edi, r.ebp, r.esp, r.eip, r.eflags};
    uint64_t v[10];
    void* ptrs[10];
    for (int i = 0; i < 10; ++i) { v[i] = gp[i]; ptrs[i] = &v[i]; }
    const uint16_t now[] = {r.ss, r.ds, r.es, r.fs, r.gs, r.cs};
    const uint16_t was[] = {old.ss, old.ds, old.es, old.fs, old.gs, old.cs};
    const int ids[] = {UC_X86_REG_SS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_FS, UC_X86_REG_GS, UC_X86_REG_CS};
    for (int i = 0; i < 6; ++i) {
        if (now[i] == was[i]) continue;
        uint64_t s = now[i];
        if (uc_reg_write(g_uc, ids[i], &s) != UC_ERR_OK) {
            CpuRegs copy = r;
            Cpu cpu(copy);
            fatal_in_game(cpu, "cannot load segment register with selector %04x", now[i]);
        }
    }
    check(uc_reg_write_batch(g_uc, const_cast<int*>(kRegIds), ptrs, 10), "write registers");
}

void encode_descriptor(uint8_t* p, uint32_t base, uint32_t limit, uint8_t access, bool big) {
    bool pages = limit > 0xFFFFF;
    if (pages) limit >>= 12;
    p[0] = uint8_t(limit);
    p[1] = uint8_t(limit >> 8);
    p[2] = uint8_t(base);
    p[3] = uint8_t(base >> 8);
    p[4] = uint8_t(base >> 16);
    p[5] = access;
    p[6] = uint8_t(((limit >> 16) & 0x0F) | (pages ? 0x80 : 0) | (big ? 0x40 : 0));
    p[7] = uint8_t(base >> 24);
}

constexpr uint32_t kIF = 1u << 9, kIOPL3 = 3u << 12;

void handle_interrupt(Cpu& cpu, uint32_t intno);

// Run shared trap logic on the current registers, with the interrupt flag
// seen as Machine::virtual_if, and write back what it changed.
template <typename F>
void with_cpu(F&& f) {
    Machine& m = *g_machine;
    CpuRegs old = read_regs();
    CpuRegs r = old;
    Cpu cpu(r);
    m.virtual_if = (r.eflags & kIF) != 0;
    f(cpu);
    r.eflags = (r.eflags & ~kIF) | (m.virtual_if ? kIF : 0);
    write_regs(r, old);
}

// Software interrupts and CPU exceptions.
void on_interrupt(uc_engine*, uint32_t intno, void*) {
    with_cpu([intno](Cpu& cpu) {
        handle_interrupt(cpu, intno);
        deliver_pending(cpu);
    });
}

// Start of every translated block: if a timer tick is waiting and the game
// can take it, stop so that cpu_run delivers it. Stopping from here, on the
// emulation thread at a block boundary, leaves the CPU state exact; a
// uc_emu_stop from the timer thread does not always (the game then fails
// at random).
void on_block(uc_engine* uc, uint64_t, uint32_t, void*) {
    if (!timer_pending()) return;
    uint64_t eflags = 0, cs = 0;
    uc_reg_read(uc, UC_X86_REG_EFLAGS, &eflags);
    uc_reg_read(uc, UC_X86_REG_CS, &cs);
    if ((eflags & kIF) && cs == g_machine->code_sel) uc_emu_stop(uc);
}

void handle_interrupt(Cpu& cpu, uint32_t intno) {
    Machine& m = *g_machine;
    const CpuRegs& r = cpu.regs();
    uint32_t pc = m.linear(r.cs, r.eip);
    const uint8_t* p = m.as.host(0);
    auto byte_at = [&](uint32_t lin) -> int { return m.as.contains(lin, 1) ? p[lin] : -1; };

    if (byte_at(pc - 2) == 0xCD && byte_at(pc - 1) == int(intno)) {
        // `int n`: EIP is past the instruction; the shared decoder wants it on it.
        cpu.set_eip(r.eip - 2);
        handle_gp(cpu);
    } else if (intno == 13) {
        handle_gp(cpu);  // #GP: EIP is on the faulting instruction
    } else {
        fatal_in_game(cpu, "CPU exception %u", intno);
    }
}

// Unicorn does not check I/O permission at CPL 3: `in`/`out` come here
// instead of raising #GP.
uint32_t on_in(uc_engine*, uint32_t port, int size, void*) { return port_in(uint16_t(port), size); }
void on_out(uc_engine*, uint32_t port, int size, uint32_t value, void*) { port_out(uint16_t(port), value, size); }

bool on_invalid_memory(uc_engine*, uc_mem_type type, uint64_t address, int, int64_t, void*) {
    g_invalid_addr = uint32_t(address);
    g_invalid_type = type;
    return false;  // stop: reported by cpu_run
}

void timer_thread() {
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    while (g_timer_running.load()) {
        double hz = g_timer_hz.load();
        next += std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / hz));
        auto now = clock::now();
        if (next < now - std::chrono::milliseconds(100)) next = now;  // fell far behind: resynchronize
        std::this_thread::sleep_until(next);
        timer_tick();  // on_block picks it up
    }
}

}  // namespace

bool cpu_init() {
    Machine& m = *g_machine;
    uc_err err = uc_open(UC_ARCH_X86, UC_MODE_32, &g_uc);
    if (err != UC_ERR_OK) {
        FG_LOG("unicorn: cannot create the CPU: %s", uc_strerror(err));
        return false;
    }
    check(uc_mem_map_ptr(g_uc, 0, AddressSpace::kSize, UC_PROT_ALL, m.as.host(0)), "map game memory");
    check(uc_mem_map_ptr(g_uc, kSysBase, kSysSize, UC_PROT_ALL, g_sys), "map system area");

    uint8_t* gdt = g_sys + kGdtOff;
    encode_descriptor(gdt + kSelRing0Code, 0, 0xFFFFFFFF, 0x9A, true);  // present, DPL0, code, readable
    encode_descriptor(gdt + kSelRing0Data, 0, 0xFFFFFFFF, 0x92, true);  // present, DPL0, data, writable
    encode_descriptor(gdt + kSelLdt, uint32_t(kSysBase + kLdtOff), Ldt::kEntries * 8 - 1, 0x82, false);
    g_sys[kStubOff] = 0xCF;  // iretd

    uc_x86_mmr gdtr{0, kSysBase + kGdtOff, 5 * 8 - 1, 0};
    uc_x86_mmr ldtr{kSelLdt, kSysBase + kLdtOff, Ldt::kEntries * 8 - 1, kDescP | (2u << kDescTypeShift)};
    // A task register with no I/O permission bitmap: every `in`/`out` at CPL 3 raises #GP.
    uc_x86_mmr tr{0, 0, 0, kDescP | (9u << kDescTypeShift)};
    check(uc_reg_write(g_uc, UC_X86_REG_GDTR, &gdtr), "set GDTR");
    check(uc_reg_write(g_uc, UC_X86_REG_LDTR, &ldtr), "set LDTR");
    check(uc_reg_write(g_uc, UC_X86_REG_TR, &tr), "set TR");
    uint64_t cr0 = 0;
    check(uc_reg_read(g_uc, UC_X86_REG_CR0, &cr0), "read CR0");
    cr0 = (cr0 | 0x31) & ~(0x80000000ull | 0x0Cull);  // PE, ET, NE; no paging, no FPU emulation/task switch
    check(uc_reg_write(g_uc, UC_X86_REG_CR0, &cr0), "set CR0");

    uc_hook h;
    check(uc_hook_add(g_uc, &h, UC_HOOK_INTR, reinterpret_cast<void*>(on_interrupt), nullptr, 1, 0), "hook interrupts");
    check(uc_hook_add(g_uc, &h, UC_HOOK_INSN, reinterpret_cast<void*>(on_in), nullptr, 1, 0, UC_X86_INS_IN), "hook in");
    check(uc_hook_add(g_uc, &h, UC_HOOK_INSN, reinterpret_cast<void*>(on_out), nullptr, 1, 0, UC_X86_INS_OUT), "hook out");
    check(uc_hook_add(g_uc, &h, UC_HOOK_BLOCK, reinterpret_cast<void*>(on_block), nullptr, 1, 0), "hook blocks");
    check(uc_hook_add(g_uc, &h, UC_HOOK_MEM_INVALID, reinterpret_cast<void*>(on_invalid_memory), nullptr, 1, 0),
          "hook invalid memory");
    unsigned major, minor;
    uc_version(&major, &minor);
    FG_LOG("unicorn %u.%u", major, minor);
    return true;
}

bool cpu_thread_init() { return true; }

int cpu_run(const CpuRegs& start) {
    Machine& m = *g_machine;
    if (__builtin_setjmp(g_exit_jmp) != 0) {
        g_timer_running = false;
        return m.exit_code;
    }

    // Enter the game from ring 0 the way a DOS extender does: load its data
    // segments, then iretd to CPL 3 with a frame of EIP, CS, EFLAGS, ESP, SS.
    uint64_t v;
    v = kSelRing0Data; check(uc_reg_write(g_uc, UC_X86_REG_SS, &v), "load ring-0 SS");
    v = kSelRing0Code; check(uc_reg_write(g_uc, UC_X86_REG_CS, &v), "load ring-0 CS");
    const std::pair<int, uint16_t> data_segs[] = {
        {UC_X86_REG_DS, start.ds}, {UC_X86_REG_ES, start.es}, {UC_X86_REG_FS, start.fs}, {UC_X86_REG_GS, start.gs}};
    for (auto [id, sel] : data_segs) {
        v = sel;
        check(uc_reg_write(g_uc, id, &v), "load game data segment");
    }
    const uint32_t frame[5] = {start.eip, start.cs, start.eflags | kIOPL3, start.esp, start.ss};
    uint32_t sp = kRing0StackOff - sizeof frame;
    std::memcpy(g_sys + sp, frame, sizeof frame);
    const std::pair<int, uint32_t> gp[] = {
        {UC_X86_REG_EAX, start.eax}, {UC_X86_REG_EBX, start.ebx}, {UC_X86_REG_ECX, start.ecx},
        {UC_X86_REG_EDX, start.edx}, {UC_X86_REG_ESI, start.esi}, {UC_X86_REG_EDI, start.edi},
        {UC_X86_REG_EBP, start.ebp}, {UC_X86_REG_ESP, uint32_t(kSysBase) + sp}, {UC_X86_REG_EFLAGS, 0x2}};
    for (auto [id, val] : gp) {
        v = val;
        check(uc_reg_write(g_uc, id, &v), "set start registers");
    }
    uint64_t begin = kSysBase + kStubOff;

    for (;;) {
        uc_err err = uc_emu_start(g_uc, begin, kNeverStop, 0, 0);
        with_cpu([err](Cpu& cpu) {
            if (err == UC_ERR_READ_UNMAPPED || err == UC_ERR_WRITE_UNMAPPED || err == UC_ERR_FETCH_UNMAPPED)
                fatal_in_game(cpu, "access to unmapped memory at %08x (type %d): %s", g_invalid_addr, g_invalid_type,
                              uc_strerror(err));
            if (err != UC_ERR_OK) fatal_in_game(cpu, "emulator stopped: %s", uc_strerror(err));
            deliver_pending(cpu);
        });
        uint64_t eip = 0;
        check(uc_reg_read(g_uc, UC_X86_REG_EIP, &eip), "read EIP");
        begin = eip;
    }
}

void cpu_exit_game() { __builtin_longjmp(g_exit_jmp, 1); }

bool cpu_set_descriptor(int index, const Descriptor& d) {
    encode_descriptor(g_sys + kLdtOff + index * 8, d.base, d.limit, d.access, d.big);
    return true;
}

bool cpu_clear_descriptor(int index) {
    std::memset(g_sys + kLdtOff + index * 8, 0, 8);
    return true;
}

void cpu_code_written(uint32_t lin, uint32_t len) {
    if (g_uc && len) uc_ctl_remove_cache(g_uc, lin, uint64_t(lin) + len);
}

bool start_timer(double hz) {
    g_timer_hz = hz;
    g_timer_running = true;
    std::thread(timer_thread).detach();
    return true;
}

void set_timer_rate(double hz) {
    if (hz == g_timer_hz.load()) return;
    g_timer_hz = hz;
    FG_LOG("timer interrupt rate %.2f Hz", hz);
}

}  // namespace fg
