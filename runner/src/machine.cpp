// SPDX-License-Identifier: GPL-2.0-or-later
#include "machine.h"

#include <cstdarg>
#include <cstdio>

#include "cpu_backend.h"
#include "log.h"

namespace fg {

Machine* g_machine = nullptr;
bool g_trace = false;

std::string Machine::read_cstring(uint32_t lin, size_t max) const {
    std::string s;
    while (s.size() < max && as.contains(lin, 1)) {
        char c = static_cast<char>(*as.host(lin++));
        if (!c) break;
        s += c;
    }
    return s;
}

void fatal_in_game(Cpu& cpu, const char* fmt, ...) {
    Machine& m = *g_machine;
    std::fprintf(stderr, "[fg] FATAL: ");
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    uint32_t pc = m.linear(cpu.cs(), cpu.eip());
    std::fprintf(stderr,
                 "\n  at eip=%08x (ghidra %08x)\n"
                 "  eax=%08x ebx=%08x ecx=%08x edx=%08x\n"
                 "  esi=%08x edi=%08x ebp=%08x esp=%08x eflags=%08x\n"
                 "  cs=%04x ds=%04x es=%04x fs=%04x gs=%04x ss=%04x\n",
                 cpu.eip(), m.ghidra(pc), cpu.eax(), cpu.ebx(), cpu.ecx(), cpu.edx(), cpu.esi(), cpu.edi(),
                 cpu.ebp(), cpu.esp(), cpu.eflags(), cpu.cs(), cpu.ds(), cpu.es(), cpu.fs(), cpu.gs(), cpu.ss());
    // Return addresses on the stack, for a rough backtrace.
    if (m.ldt.valid(cpu.ss())) {
        std::fprintf(stderr, "  stack top:");
        uint32_t top = m.linear(cpu.ss(), cpu.esp());
        for (int i = 0; i < 8 && m.as.contains(top + 4 * i, 4); ++i)
            std::fprintf(stderr, " %08x", m.as.read<uint32_t>(top + 4 * i));
        std::fprintf(stderr, "\n");
        std::fprintf(stderr, "  stack:");
        uint32_t sp = m.linear(cpu.ss(), cpu.esp());
        for (int i = 0; i < 16 && m.as.contains(sp + 4 * i, 4); ++i) {
            uint32_t v = m.as.read<uint32_t>(sp + 4 * i);
            uint32_t g = m.image.to_ghidra(v);
            if (g != ~0u) std::fprintf(stderr, " [%d]%08x", i, g);
        }
        std::fprintf(stderr, "\n");
    }
    std::fflush(stderr);
    game_exit(255);
}

void game_exit(int code) {
    g_machine->exit_code = code;
    cpu_exit_game();
}

}  // namespace fg
