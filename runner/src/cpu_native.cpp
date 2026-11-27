// SPDX-License-Identifier: GPL-2.0-or-later
// Native CPU backend: the game runs directly on the host CPU (32-bit Linux).
//
// In user mode, `int n` (for n other than 3/4/0x80), `in`/`out`, `cli`/`sti`
// and `hlt` raise #GP, which Linux delivers as SIGSEGV with trapno 13; the
// shared handler (traps.cpp) emulates it on the saved register context and
// sigreturn resumes the game. The game's segments are LDT entries installed
// with modify_ldt, based at the host address of the game address space.
//
// Hardware interrupts (the PIT timer) arrive as a real-time signal on the
// game thread and are delivered from the signal handler.
#include <asm/ldt.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>

#include "cpu_backend.h"
#include "log.h"
#include "machine.h"

namespace fg {
// Registers used to enter the game (layout shared with cpu_native_entry.S).
struct EntryRegs {
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp;  // offsets 0..28
    uint32_t eip, eflags;                             // 32, 36
    uint32_t cs, ds, es, fs, gs, ss;                  // 40..60
};
static_assert(sizeof(EntryRegs) == 64, "layout shared with cpu_native_entry.S");
}  // namespace fg

extern "C" {
uint16_t fg_host_gs;
void fg_trap_entry(int, siginfo_t*, void*);
void fg_trap_handler(int sig, siginfo_t* si, void* ucv);
[[noreturn]] void fg_enter_game(const fg::EntryRegs*);
}

namespace fg {

namespace {

constexpr int kTrapGP = 13, kTrapPF = 14;
alignas(16) uint8_t g_alt_stack[256 * 1024];

int g_timer_sig = 0;
timer_t g_timer;
bool g_timer_armed = false;
double g_timer_hz = 0;
sigjmp_buf g_exit_jmp;

CpuRegs load_regs(const greg_t* g) {
    CpuRegs r;
    r.eax = g[REG_EAX]; r.ebx = g[REG_EBX]; r.ecx = g[REG_ECX]; r.edx = g[REG_EDX];
    r.esi = g[REG_ESI]; r.edi = g[REG_EDI]; r.ebp = g[REG_EBP]; r.esp = g[REG_ESP];
    r.eip = g[REG_EIP]; r.eflags = g[REG_EFL];
    r.cs = g[REG_CS]; r.ds = g[REG_DS]; r.es = g[REG_ES]; r.fs = g[REG_FS]; r.gs = g[REG_GS]; r.ss = g[REG_SS];
    return r;
}

void store_regs(const CpuRegs& r, greg_t* g) {
    g[REG_EAX] = r.eax; g[REG_EBX] = r.ebx; g[REG_ECX] = r.ecx; g[REG_EDX] = r.edx;
    g[REG_ESI] = r.esi; g[REG_EDI] = r.edi; g[REG_EBP] = r.ebp;
    // sigreturn reloads ESP from REG_ESP; REG_UESP is only informational.
    g[REG_ESP] = r.esp; g[REG_UESP] = r.esp;
    g[REG_EIP] = r.eip; g[REG_EFL] = r.eflags;
    g[REG_CS] = r.cs; g[REG_DS] = r.ds; g[REG_ES] = r.es; g[REG_FS] = r.fs; g[REG_GS] = r.gs; g[REG_SS] = r.ss;
}

void rearm_timer() {
    if (!g_timer_armed) return;
    itimerspec its{};
    double period = 1.0 / g_timer_hz;
    its.it_interval.tv_sec = static_cast<time_t>(period);
    its.it_interval.tv_nsec = static_cast<long>(std::fmod(period, 1.0) * 1e9);
    its.it_value = its.it_interval;
    timer_settime(g_timer, 0, &its, nullptr);
}

}  // namespace

bool cpu_init() {
    g_timer_sig = SIGRTMIN;
    struct sigaction sa {};
    sa.sa_sigaction = fg_trap_entry;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, g_timer_sig);  // never interrupt a trap handler with a timer tick
    for (int sig : {SIGSEGV, SIGILL, SIGTRAP, SIGFPE, SIGBUS, g_timer_sig})
        if (sigaction(sig, &sa, nullptr) != 0) {
            FG_LOG("cannot install signal handlers");
            return false;
        }
    return true;
}

bool cpu_thread_init() {
    uint16_t gs;
    asm volatile("movw %%gs, %0" : "=r"(gs));
    fg_host_gs = gs;
    stack_t ss{};
    ss.ss_sp = g_alt_stack;
    ss.ss_size = sizeof g_alt_stack;
    return sigaltstack(&ss, nullptr) == 0;
}

int cpu_run(const CpuRegs& start) {
    Machine& m = *g_machine;
    if (sigsetjmp(g_exit_jmp, 1) != 0) return m.exit_code;
    EntryRegs e{start.eax, start.ebx, start.ecx, start.edx, start.esi, start.edi, start.ebp, start.esp,
                start.eip, start.eflags, start.cs, start.ds, start.es, start.fs, start.gs, start.ss};
    // iret frame: EIP, CS, EFLAGS (low to high).
    e.esp -= 12;
    uint32_t sp = m.linear(start.ss, e.esp);
    m.as.write<uint32_t>(sp, e.eip);
    m.as.write<uint32_t>(sp + 4, e.cs);
    m.as.write<uint32_t>(sp + 8, e.eflags);
    fg_enter_game(&e);
}

void cpu_exit_game() { siglongjmp(g_exit_jmp, 1); }

bool cpu_set_descriptor(int index, const Descriptor& s) {
    user_desc d{};
    d.entry_number = index;
    d.base_addr = static_cast<uint32_t>(g_machine->as.host_base() + s.base);  // wraps mod 2^32, as on the real CPU
    bool pages = s.limit > 0xFFFFF;
    d.limit = pages ? (s.limit >> 12) : s.limit;
    d.limit_in_pages = pages;
    d.seg_32bit = s.big;
    bool code = (s.access & 0x08) != 0;
    // contents: 0 = data, 1 = expand-down data, 2 = code (non-conforming)
    d.contents = code ? 2 : ((s.access & 0x04) ? 1 : 0);
    // read_exec_only: for data = read-only, for code = execute-only
    d.read_exec_only = !(s.access & 0x02);
    d.seg_not_present = !(s.access & 0x80);
    d.useable = 1;
    if (syscall(SYS_modify_ldt, 0x11, &d, sizeof d) != 0) {
        FG_LOG("modify_ldt failed for entry %d (base %#x limit %#x access %#x)", index, s.base, s.limit, s.access);
        return false;
    }
    return true;
}

bool cpu_clear_descriptor(int index) {
    user_desc d{};
    d.entry_number = index;
    d.read_exec_only = 1;
    d.seg_not_present = 1;
    return syscall(SYS_modify_ldt, 0x11, &d, sizeof d) == 0;
}

void cpu_code_written(uint32_t, uint32_t) {}  // the host CPU keeps its caches coherent

bool start_timer(double hz) {
    sigevent sev{};
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = g_timer_sig;
    sev._sigev_un._tid = static_cast<pid_t>(syscall(SYS_gettid));
    if (timer_create(CLOCK_MONOTONIC, &sev, &g_timer) != 0) return false;
    g_timer_armed = true;
    g_timer_hz = hz;
    rearm_timer();
    return true;
}

void set_timer_rate(double hz) {
    if (hz == g_timer_hz) return;
    g_timer_hz = hz;
    FG_LOG("timer interrupt rate %.2f Hz", hz);
    rearm_timer();
}

}  // namespace fg

extern "C" void fg_trap_handler(int sig, siginfo_t* si, void* ucv) {
    using namespace fg;
    auto* uc = static_cast<ucontext_t*>(ucv);
    greg_t* g = uc->uc_mcontext.gregs;
    Machine& m = *g_machine;
    bool in_game = m.ldt.valid(static_cast<uint16_t>(g[REG_CS]));

    if (sig == g_timer_sig) {
        timer_tick();
        if (in_game) {
            CpuRegs r = load_regs(g);
            Cpu cpu(r);
            deliver_pending(cpu);
            store_regs(r, g);
        }
        return;
    }
    if (!in_game) {
        // A fault in the runner itself: report and die (no recovery possible).
        std::fprintf(stderr, "[fg] runner crashed: signal %d at host eip %08x, address %p\n", sig,
                     static_cast<uint32_t>(g[REG_EIP]), si->si_addr);
        std::_Exit(134);
    }

    CpuRegs r = load_regs(g);
    Cpu cpu(r);
    int trapno = static_cast<int>(g[REG_TRAPNO]);
    if (sig == SIGSEGV && trapno == kTrapGP) {
        handle_gp(cpu);
        store_regs(r, g);
        return;
    }
    if (sig == SIGSEGV && trapno == kTrapPF) {
        uint32_t lin = m.as.linear_of(si->si_addr);
        fatal_in_game(cpu, "page fault at game linear %08x (ghidra %08x), error %lx", lin, m.ghidra(lin),
                      static_cast<unsigned long>(g[REG_ERR]));
    }
    fatal_in_game(cpu, "signal %d (trap %d, code %d)", sig, trapno, si->si_code);
}
