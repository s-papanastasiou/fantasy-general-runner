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
    char what[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(what, sizeof what, fmt, ap);
    va_end(ap);
    uint32_t pc = m.linear(cpu.cs(), cpu.eip());
    FG_LOG("FATAL: %s", what);
    FG_LOG("  at eip=%08x (ghidra %08x)", cpu.eip(), m.ghidra(pc));
    FG_LOG("  eax=%08x ebx=%08x ecx=%08x edx=%08x", cpu.eax(), cpu.ebx(), cpu.ecx(), cpu.edx());
    FG_LOG("  esi=%08x edi=%08x ebp=%08x esp=%08x eflags=%08x", cpu.esi(), cpu.edi(), cpu.ebp(), cpu.esp(),
           cpu.eflags());
    FG_LOG("  cs=%04x ds=%04x es=%04x fs=%04x gs=%04x ss=%04x", cpu.cs(), cpu.ds(), cpu.es(), cpu.fs(), cpu.gs(),
           cpu.ss());
    // Return addresses on the stack, for a rough backtrace.
    if (m.ldt.valid(cpu.ss())) {
        std::string top = "  stack top:", calls = "  stack:";
        char item[32];
        uint32_t sp = m.linear(cpu.ss(), cpu.esp());
        for (int i = 0; i < 16 && m.as.contains(sp + 4 * i, 4); ++i) {
            uint32_t v = m.as.read<uint32_t>(sp + 4 * i);
            if (i < 8) {
                std::snprintf(item, sizeof item, " %08x", v);
                top += item;
            }
            uint32_t g = m.image.to_ghidra(v);
            if (g != ~0u) {
                std::snprintf(item, sizeof item, " [%d]%08x", i, g);
                calls += item;
            }
        }
        FG_LOG("%s", top.c_str());
        FG_LOG("%s", calls.c_str());
    }
    char shown[700];
    std::snprintf(shown, sizeof shown,
                  "The game stopped because the runner does not support something it did:\n%s (game address %08x).",
                  what, m.ghidra(pc));
    set_error(shown);
    game_exit(255);
}

void game_exit(int code) {
    g_machine->exit_code = code;
    cpu_exit_game();
}

}  // namespace fg
