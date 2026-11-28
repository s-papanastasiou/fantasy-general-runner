// SPDX-License-Identifier: GPL-2.0-or-later
// Emulating the privileged instructions the game executes, and delivering
// hardware interrupts to it. Backend-neutral: see cpu_backend.h.
//
// The game runs at CPL 3 with IOPL 0, so `int n`, `in`/`out`, `cli`/`sti`
// and `hlt` raise #GP. handle_gp decodes the instruction at CS:EIP, emulates
// it on the register view and advances EIP.
//
// Hardware interrupts (the PIT timer) are counted by timer_tick. When the
// game has (virtual) interrupts enabled, deliver_pending saves the
// interrupted context, pushes an iret frame that returns to a trampoline
// (`int FEh` in DOS memory) and jumps to the game's handler; the trampoline
// traps back here and the saved context is restored exactly.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "cpu_backend.h"
#include "idle.h"
#include "input.h"
#include "log.h"
#include "machine.h"
#include "services.h"

namespace fg {

namespace {

// Stubs in DOS memory (game code selector has base 0, so these are offsets too).
constexpr uint32_t kIsrReturnStub = 0x600;  // int FEh: return from an injected interrupt
constexpr uint32_t kDefaultIsr = 0x610;     // iretd
constexpr uint32_t kTimerIsr = 0x618;       // int FCh (BIOS tick); iretd
constexpr uint8_t kVecIsrReturn = 0xFE, kVecBiosTick = 0xFC, kVecHook = 0xFD;

// Runner hooks patched into the game's code as `int FDh` (see install_game_hooks).
// Each replaces one instruction (`orig`), which the runner then performs
// itself (`finish`) after the hook's action.
IdleDetector g_input_loop_idle;
uint64_t g_scroll_step_ns = 120000000;
uint64_t g_last_scroll_ns = 0;
bool g_eight_way_scroll = true;
uint8_t g_scroll_last_dir = 8;         // direction of the previous step
uint32_t g_scroll_view_before = ~0u;   // view origin before it
bool g_scroll_diagonal_blocked = false;
bool g_scroll_from_keys = false;       // this pass's direction came from the arrow keys
uint64_t g_ai_step_ns = 10000000;
uint64_t g_last_ai_step_ns = 0;
uint64_t g_ai_view_ns = 200000000;
uint64_t g_last_view_change_ns = 0;
uint32_t g_last_view = ~0u;

// A game data address (Ghidra) as a linear address.
uint32_t game_lin(uint32_t ghidra) {
    for (const auto& o : g_machine->image.objects)
        if (ghidra - o.preferred_base < o.virtual_size) return o.load_base + (ghidra - o.preferred_base);
    return 0;
}

// Is the side to move (0xe98dc) played by the computer (flag 0xce24c[side])?
bool computer_side() {
    Machine& m = *g_machine;
    uint8_t side = *m.as.host(game_lin(0xe98dc));
    return side < 2 && *m.as.host(game_lin(0xce24c) + side) != 0;
}

// Hook actions return false to make the game re-execute the hook (and wait
// some more) instead of continuing. Each wait slice is at most 1 ms: timer
// interrupts (and with them Miles' audio mixing) are held off while a trap
// handler runs.

// Keep consecutive calls at least `interval` apart, waiting in 1 ms slices.
bool pace(uint64_t& last, uint64_t interval) {
    uint64_t now = idle_now_ns();
    if (now - last < interval) {
        idle_sleep_ns(std::min<uint64_t>(1000000, last + interval - now));
        return false;
    }
    last = now;
    return true;
}

// The UI's input-polling loops (campaign map, battle map, menus) read the
// mouse state from memory and never trap, so the runner cannot see them spin.
// ReadMouseState is called once per loop pass.
bool on_read_mouse_state(Cpu&) {
    if (g_input_loop_idle.spinning()) g_input_loop_idle.sleep_ns(1000000);
    return true;
}

// The battle-map loops call this scroll step on every pass while the pointer
// is at a screen edge, with no timing of their own: on 1996 hardware the
// redraw made each step slow. Keep consecutive steps g_scroll_step_ns apart.
//
// Direction (0xce233, set by UpdateEdgeScrollDirection 0x7f7b2) is a hex
// direction: 0 up, 1 up-right, 2 right, 3 down-right, 4 down, 5 down-left,
// 6 left, 7 up-left. The game uses only 0/4 for its top (y <= 35) and
// bottom (y >= 435) zones and 1/3/5/7 for the sides (x < 3, x > 636, upper
// or lower half), so the sides always scroll diagonally (2 columns, 1 row
// per step) and there is no straight sideways scroll. Unless
// --original-scrolling, choose eight-way instead: sides straight left or
// right, top/bottom straight, a side inside the top or bottom zone (a
// corner) diagonal; with the pointer in no zone, the arrow keys' direction
// (on_edge_scroll_direction). A diagonal that does not move the view (the
// map's left or right end) continues vertically.
// Direction codes by (dx + 1, dy + 1), and back.
constexpr uint8_t kDir[3][3] = {{7, 6, 5}, {0, 8, 4}, {1, 2, 3}};
constexpr int kDirDx[8] = {0, 1, 1, 1, 0, -1, -1, -1}, kDirDy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};

// The pointer's edge zone: dx, dy in -1..1 (both 0: no zone). The pointer in
// screen pixels comes from the game's ReadMouseState copy (0xd8448 x,
// 0xd844c y); the driver's own range is the game's choice (8x the screen).
void pointer_zone(int& dx, int& dy) {
    Machine& m = *g_machine;
    int x = static_cast<int>(m.as.read<uint32_t>(game_lin(0xd8448)));
    int y = static_cast<int>(m.as.read<uint32_t>(game_lin(0xd844c)));
    dx = x < 3 ? -1 : x > 636 ? 1 : 0;
    dy = y <= 35 ? -1 : y >= 435 ? 1 : 0;
}

uint8_t eight_way_direction(uint8_t game_dir) {
    Machine& m = *g_machine;
    int dx, dy;
    pointer_zone(dx, dy);
    if (dx == 0 && dy == 0) {  // not our zones: the arrow keys' direction (or the game's)
        dx = kDirDx[game_dir];
        dy = kDirDy[game_dir];
    }
    uint8_t dir = kDir[dx + 1][dy + 1];
    uint32_t view = m.as.read<uint32_t>(game_lin(0xe93d8));
    uint8_t vertical = kDir[1][dy + 1];
    if (!dx || !dy) {
        g_scroll_diagonal_blocked = false;
    } else if (g_scroll_diagonal_blocked && g_scroll_last_dir == vertical) {
        // already continuing vertically from this corner
    } else {
        // blocked if the same diagonal last time left the view where it was
        g_scroll_diagonal_blocked = g_scroll_last_dir == dir && view == g_scroll_view_before;
    }
    if (g_scroll_diagonal_blocked) dir = vertical;
    g_scroll_last_dir = dir;
    g_scroll_view_before = view;
    return dir;
}

// End of UpdateEdgeScrollDirection: with the pointer at no edge, held arrow
// keys choose the direction (two keys a diagonal; opposite keys cancel). The
// battle loops then scroll as for the mouse: `if (0xce233 != 8)
// ScrollBattleMapStep()`.
bool on_edge_scroll_direction(Cpu&) {
    Machine& m = *g_machine;
    uint8_t& dir = *m.as.host(game_lin(0xce233));
    uint8_t keys = g_input.scroll_keys();
    g_scroll_from_keys = false;
    if (dir != 8 || !keys) return true;
    int dx = ((keys & Input::kScrollRight) ? 1 : 0) - ((keys & Input::kScrollLeft) ? 1 : 0);
    int dy = ((keys & Input::kScrollDown) ? 1 : 0) - ((keys & Input::kScrollUp) ? 1 : 0);
    dir = kDir[dx + 1][dy + 1];
    g_scroll_from_keys = dir != 8;
    return true;
}

bool on_scroll_map_step(Cpu&) {
    if (!pace(g_last_scroll_ns, g_scroll_step_ns)) return false;
    Machine& m = *g_machine;
    uint8_t& dir = *m.as.host(game_lin(0xce233));
    // The cause went away while this step waited its turn (keys let go, or
    // the pointer left the edge): no step, so a tap scrolls once. Direction 8
    // makes the game's step function change nothing.
    int zx, zy;
    pointer_zone(zx, zy);
    if (g_scroll_from_keys ? !g_input.scroll_keys() : (zx == 0 && zy == 0)) dir = 8;
    if (g_eight_way_scroll && dir < 8) dir = eight_way_direction(dir);
    FG_TRACE("battle map scroll step: direction %u, view at column %u row %u",
             *m.as.host(game_lin(0xce233)), m.as.read<uint16_t>(game_lin(0xe93d8)),
             m.as.read<uint16_t>(game_lin(0xe93da)));
    return true;
}

// MoveUnitAnimated (0x7a2d3) slides a unit one pixel per loop pass. It waits
// a game tick (10 ms, FUN_00039f63(1)) per pixel only for special moves
// (mode [ebp-24h] != 0, or the saved-state flag 0xe98af); ordinary moves had
// no timing, so the computer's moves flash past. Pace the computer's visible
// moves (side 0xe98dc has computer flag 0xce24c[side]; visible = [ebp-2Ch]).
bool on_unit_move_step(Cpu& cpu) {
    Machine& m = *g_machine;
    if (!g_ai_step_ns) return true;
    bool visible = *m.as.host(m.linear(cpu.ss(), cpu.ebp() - 0x2c)) != 0;
    if (!computer_side() || !visible) return true;
    return pace(g_last_ai_step_ns, g_ai_step_ns);
}

// RedrawBattleMap (0x85a45) draws the whole map at the view origin 0xe93d8
// (x, y words). On the computer's turn the game recentres the view on each
// unit it considers and redraws, several times in a row; on 1996 hardware
// each full redraw took a sizeable fraction of a second, natively the views
// flick past like a fast scroll. Keep each view of the computer's turn on
// screen at least g_ai_view_ns before drawing a different one.
bool on_redraw_battle_map(Cpu&) {
    Machine& m = *g_machine;
    uint32_t view = m.as.read<uint32_t>(game_lin(0xe93d8));
    if (view == g_last_view) return true;
    if (g_ai_view_ns && computer_side() && !pace(g_last_view_change_ns, g_ai_view_ns)) return false;
    g_last_view_change_ns = idle_now_ns();
    g_last_view = view;
    return true;
}

// Flags of `cmp byte [mem], 0`.
void cmp_byte_zero(Cpu& cpu, uint8_t v) {
    constexpr uint32_t CF = 1u << 0, PF = 1u << 2, AF = 1u << 4, ZF = 1u << 6, SF = 1u << 7, OF = 1u << 11;
    uint32_t f = cpu.eflags() & ~(CF | PF | AF | ZF | SF | OF);
    if (v == 0) f |= ZF;
    if (v & 0x80) f |= SF;
    if (!(__builtin_popcount(v) & 1)) f |= PF;
    cpu.set_eflags(f);
}

struct GameHook {
    uint32_t ghidra;           // address of the replaced instruction
    std::vector<uint8_t> orig;  // its bytes as in the file (installed only if they match)
    int reloc;                 // offset in orig of a 4-byte Ghidra data address (relocated at load), or -1
    bool (*action)(Cpu&);
    void (*finish)(Cpu&);      // performs the replaced instruction
    uint32_t lin;              // patched address, 0 if not installed
};
GameHook g_hooks[] = {
    // ReadMouseState 0x3cbfa: sub esp, 10h
    {0x3cbff, {0x81, 0xEC, 0x10, 0, 0, 0}, -1, on_read_mouse_state, [](Cpu& c) { c.set_esp(c.esp() - 0x10); }, 0},
    // ScrollBattleMapStep 0x6ef53: sub esp, 4
    {0x6ef5b, {0x81, 0xEC, 0x04, 0, 0, 0}, -1, on_scroll_map_step, [](Cpu& c) { c.set_esp(c.esp() - 4); }, 0},
    // RedrawBattleMap 0x85a45: sub esp, 4
    {0x85a4d, {0x81, 0xEC, 0x04, 0, 0, 0}, -1, on_redraw_battle_map, [](Cpu& c) { c.set_esp(c.esp() - 4); }, 0},
    // UpdateEdgeScrollDirection 0x7f7b2, epilogue: mov esp, ebp
    {0x7f899, {0x89, 0xEC}, -1, on_edge_scroll_direction, [](Cpu& c) { c.set_esp(c.ebp()); }, 0},
    // MoveUnitAnimated 0x7a2d3, step loop: cmp byte [0xe98af], 0
    {0x7aa87, {0x80, 0x3D, 0xAF, 0x98, 0x0E, 0x00, 0x00}, 2, on_unit_move_step,
     [](Cpu& c) { cmp_byte_zero(c, *g_machine->as.host(game_lin(0xe98af))); }, 0},
};

bool run_game_hook(Cpu& cpu, uint32_t pc) {
    for (auto& h : g_hooks) {
        if (h.lin != pc) continue;
        if (!h.action(cpu)) {
            cpu.set_eip(cpu.eip() - 2);  // back onto `int FDh`: run the hook again
            return true;
        }
        h.finish(cpu);
        cpu.set_eip(cpu.eip() - 2 + static_cast<uint32_t>(h.orig.size()));  // EIP is past `int FDh`
        return true;
    }
    return false;
}

std::atomic<int> g_pending_irq0{0};  // timer ticks waiting for delivery

// FG_PROFILE=1: sample where the game is at each timer interrupt and print
// the busiest places once a second (Ghidra addresses; "<-" likely callers:
// return addresses found on the stack).
struct Profiler {
    bool on = std::getenv("FG_PROFILE") != nullptr;
    uint64_t window_start = 0;
    std::map<std::string, int> counts;
    int total = 0;
    uint64_t last_sample = 0, max_gap = 0;  // longest wait between timer interrupts

    void sample(Cpu& cpu) {
        Machine& m = *g_machine;
        char key[96];
        int n = std::snprintf(key, sizeof key, "%06x", m.ghidra(m.linear(cpu.cs(), cpu.eip())));
        // Callers: stack values in the code object (object 1) that follow a
        // call instruction (E8 rel32 or FF /2), nearest first.
        const LeObject& code = m.image.objects[0];
        uint32_t sp = m.linear(cpu.ss(), cpu.esp());
        int found = 0;
        for (int i = 0; i < 256 && found < 3 && n < int(sizeof key) - 16; ++i) {
            if (!m.as.contains(sp + 4 * i, 4)) break;
            uint32_t ret = m.as.read<uint32_t>(sp + 4 * i);
            if (ret - code.load_base < 6 || ret - code.load_base >= code.virtual_size) continue;
            const uint8_t* b = m.as.host(ret);
            bool after_call = b[-5] == 0xE8 || (b[-2] == 0xFF && (b[-1] & 0x38) == 0x10) ||
                              (b[-3] == 0xFF && (b[-2] & 0x38) == 0x10) || (b[-6] == 0xFF && (b[-5] & 0x38) == 0x10);
            if (!after_call) continue;
            n += std::snprintf(key + n, sizeof key - n, " <- %06x", m.ghidra(ret));
            ++found;
        }
        counts[key]++;
        total++;
        uint64_t now = idle_now_ns();
        if (last_sample) max_gap = std::max(max_gap, now - last_sample);
        last_sample = now;
        if (!window_start) window_start = now;
        if (now - window_start < 1000000000) return;
        std::vector<std::pair<int, std::string>> top;
        for (auto& [k, c] : counts) top.push_back({c, k});
        std::sort(top.rbegin(), top.rend());
        std::fprintf(stderr, "[profile] %.1f s, %d samples, longest gap %.1f ms\n", now / 1e9, total, max_gap / 1e6);
        max_gap = 0;
        for (size_t i = 0; i < top.size() && i < 6; ++i)
            std::fprintf(stderr, "[profile]   %3d%%  %s\n", top[i].first * 100 / total, top[i].second.c_str());
        counts.clear();
        total = 0;
        window_start = now;
    }
} g_profiler;

struct SavedContext {
    CpuRegs regs;
    bool virtual_if;
};
std::vector<SavedContext> g_isr_stack;  // nested injected interrupts

// Deliver hardware interrupt `vec` to the game now (context must be game code).
void inject_interrupt(Cpu& cpu, uint8_t vec) {
    Machine& m = *g_machine;
    if (vec == 0x08 && g_profiler.on) g_profiler.sample(cpu);
    g_isr_stack.push_back({cpu.regs(), m.virtual_if});

    uint32_t sp = cpu.esp() - 12;
    uint32_t lin = m.linear(cpu.ss(), sp);
    m.as.write<uint32_t>(lin, kIsrReturnStub);
    m.as.write<uint32_t>(lin + 4, m.code_sel);
    m.as.write<uint32_t>(lin + 8, cpu.eflags());
    cpu.set_esp(sp);
    cpu.set_cs(m.pm_vectors[vec].sel);
    cpu.set_eip(m.pm_vectors[vec].off);
    cpu.set_flag(1u << 8, false);   // TF
    cpu.set_flag(1u << 10, false);  // DF: Watcom code assumes DF=0 on entry (restored on return)
    m.virtual_if = false;
}

void isr_return(Cpu& cpu) {
    Machine& m = *g_machine;
    if (g_isr_stack.empty()) fatal_in_game(cpu, "interrupt return without an interrupt in progress");
    cpu.regs() = g_isr_stack.back().regs;
    m.virtual_if = g_isr_stack.back().virtual_if;
    g_isr_stack.pop_back();
    deliver_pending(cpu);
}

}  // namespace

void timer_tick() {
    // Like the PIC, coalesce ticks if the game falls behind.
    int n = g_pending_irq0.load(std::memory_order_relaxed);
    while (n < 4 && !g_pending_irq0.compare_exchange_weak(n, n + 1, std::memory_order_relaxed)) {}
}

bool timer_pending() { return g_pending_irq0.load(std::memory_order_relaxed) > 0; }

void deliver_pending(Cpu& cpu) {
    Machine& m = *g_machine;
    if (!m.virtual_if || cpu.cs() != m.code_sel) return;
    int n = g_pending_irq0.load(std::memory_order_relaxed);
    while (n > 0) {
        if (g_pending_irq0.compare_exchange_weak(n, n - 1, std::memory_order_relaxed)) {
            inject_interrupt(cpu, 0x08);
            return;
        }
    }
}

namespace {

// DOS/4GW gives protected-mode programs selector 0040h for the BIOS data
// area (real-mode segment 0040h). It is a GDT selector, which the runner
// cannot provide, so loading it faults; the fault handler loads this LDT
// alias of the same memory instead. The game uses it when it quits
// (0x58081: reads the CRTC port at 0040:0063 to wait for retraces).
constexpr uint16_t kBiosDataSel = 0x40;

uint16_t bios_data_alias() {
    static uint16_t sel = g_machine->ldt.make(0x400, 0xFFFF, false);
    return sel;
}

void set_sreg(Cpu& cpu, int index, uint16_t v) {
    switch (index) {
    case 0: cpu.set_es(v); break;
    case 3: cpu.set_ds(v); break;
    case 4: cpu.set_fs(v); break;
    case 5: cpu.set_gs(v); break;
    default: break;  // cs/ss: not loaded this way
    }
}

uint16_t reg16(const Cpu& cpu, int index) {
    const uint32_t r[8] = {cpu.eax(), cpu.ecx(), cpu.edx(), cpu.ebx(), cpu.esp(), cpu.ebp(), cpu.esi(), cpu.edi()};
    return static_cast<uint16_t>(r[index & 7]);
}

}  // namespace

void handle_gp(Cpu& cpu) {
    Machine& m = *g_machine;
    uint32_t pc = m.linear(cpu.cs(), cpu.eip());
    const uint8_t* p = m.as.host(pc);
    int len = 0;
    bool opsize = false, rep = false;
    uint16_t seg_override = 0;
    for (;; ++len) {  // prefixes
        uint8_t b = p[len];
        if (b == 0x66) opsize = true;
        else if (b == 0xF3 || b == 0xF2) rep = true;
        else if (b == 0x26) seg_override = cpu.es();
        else if (b == 0x2E) seg_override = cpu.cs();
        else if (b == 0x36) seg_override = cpu.ss();
        else if (b == 0x3E) seg_override = cpu.ds();
        else if (b == 0x64) seg_override = cpu.fs();
        else if (b == 0x65) seg_override = cpu.gs();
        else break;
    }
    uint8_t op = p[len++];
    int size = opsize ? 2 : 4;
    switch (op) {
    case 0xCD: {  // int imm8
        uint8_t vec = p[len++];
        cpu.set_eip(cpu.eip() + len);
        if (vec == kVecIsrReturn) { isr_return(cpu); return; }
        if (vec == kVecHook && run_game_hook(cpu, pc)) return;
        if (vec == kVecBiosTick) {
            m.as.write<uint32_t>(0x46C, m.as.read<uint32_t>(0x46C) + 1);
            return;
        }
        dispatch_int(cpu, vec);
        return;
    }
    case 0xE4: cpu.set_al(port_in(p[len], 1)); len++; break;          // in al, imm8
    case 0xE5: {                                                       // in (e)ax, imm8
        uint32_t v = port_in(p[len++], size);
        if (size == 2) cpu.set_ax(v); else cpu.set_eax(v);
        break;
    }
    case 0xE6: port_out(p[len], cpu.al(), 1); len++; break;          // out imm8, al
    case 0xE7: port_out(p[len], size == 2 ? cpu.ax() : cpu.eax(), size); len++; break;
    case 0xEC: cpu.set_al(port_in(cpu.dx(), 1)); break;               // in al, dx
    case 0xED: {
        uint32_t v = port_in(cpu.dx(), size);
        if (size == 2) cpu.set_ax(v); else cpu.set_eax(v);
        break;
    }
    case 0xEE: port_out(cpu.dx(), cpu.al(), 1); break;                // out dx, al
    case 0xEF: port_out(cpu.dx(), size == 2 ? cpu.ax() : cpu.eax(), size); break;
    case 0x6C: case 0x6D: case 0x6E: case 0x6F: {                     // ins/outs (optionally rep)
        bool out = op >= 0x6E;
        int n = (op & 1) ? size : 1;
        int step = cpu.flag(1u << 10) ? -n : n;  // DF
        uint32_t count = rep ? cpu.ecx() : 1;
        for (; count; --count) {
            if (out) {
                uint16_t sel = seg_override ? seg_override : cpu.ds();
                uint32_t lin = m.linear(sel, cpu.esi());
                uint32_t v = n == 1 ? *m.as.host(lin) : n == 2 ? m.as.read<uint16_t>(lin) : m.as.read<uint32_t>(lin);
                port_out(cpu.dx(), v, n);
                cpu.set_esi(cpu.esi() + step);
            } else {
                uint32_t lin = m.linear(cpu.es(), cpu.edi());
                uint32_t v = port_in(cpu.dx(), n);
                if (n == 1) *m.as.host(lin) = static_cast<uint8_t>(v);
                else if (n == 2) m.as.write<uint16_t>(lin, static_cast<uint16_t>(v));
                else m.as.write<uint32_t>(lin, v);
                cpu.set_edi(cpu.edi() + step);
            }
        }
        if (rep) cpu.set_ecx(0);
        break;
    }
    case 0xFA: m.virtual_if = false; break;                            // cli
    case 0xFB:                                                         // sti
        m.virtual_if = true;
        cpu.set_eip(cpu.eip() + len);
        deliver_pending(cpu);
        return;
    case 0xF4: break;                                                  // hlt: treat as a no-op wait
    case 0x8E: {                                                       // mov sreg, r16 (0040h only)
        uint8_t modrm = p[len++];
        int sreg = (modrm >> 3) & 7;
        if ((modrm >> 6) != 3 || reg16(cpu, modrm & 7) != kBiosDataSel || sreg == 1 || sreg == 2 || sreg > 5)
            fatal_in_game(cpu, "general protection fault on unhandled instruction %02x %02x %02x %02x",
                          p[0], p[1], p[2], p[3]);
        set_sreg(cpu, sreg, bios_data_alias());
        break;
    }
    case 0x07: case 0x1F: case 0x0F: {                                 // pop es/ds/fs/gs (0040h only)
        int sreg = op == 0x07 ? 0 : op == 0x1F ? 3 : p[len] == 0xA1 ? 4 : p[len] == 0xA9 ? 5 : -1;
        uint32_t sp = m.linear(cpu.ss(), cpu.esp());
        if (sreg < 0 || static_cast<uint16_t>(m.as.read<uint32_t>(sp)) != kBiosDataSel)
            fatal_in_game(cpu, "general protection fault on unhandled instruction %02x %02x %02x %02x",
                          p[0], p[1], p[2], p[3]);
        if (op == 0x0F) ++len;
        set_sreg(cpu, sreg, bios_data_alias());
        cpu.set_esp(cpu.esp() + (opsize ? 2 : 4));
        break;
    }
    default:
        fatal_in_game(cpu, "general protection fault on unhandled instruction %02x %02x %02x %02x",
                      p[0], p[1], p[2], p[3]);
    }
    cpu.set_eip(cpu.eip() + len);
}

void install_interrupt_stubs() {
    Machine& m = *g_machine;
    const uint8_t isr_return[] = {0xCD, kVecIsrReturn};
    const uint8_t default_isr[] = {0xCF};                   // iretd
    const uint8_t timer_isr[] = {0xCD, kVecBiosTick, 0xCF};  // int FCh; iretd
    std::memcpy(m.as.host(kIsrReturnStub), isr_return, sizeof isr_return);
    std::memcpy(m.as.host(kDefaultIsr), default_isr, sizeof default_isr);
    std::memcpy(m.as.host(kTimerIsr), timer_isr, sizeof timer_isr);
    for (auto& v : m.pm_vectors) v = {m.code_sel, kDefaultIsr};
    m.pm_vectors[0x08] = {m.code_sel, kTimerIsr};
}

void install_game_hooks(double scroll_step_ms, bool original_scrolling, double ai_step_ms, double ai_view_ms) {
    Machine& m = *g_machine;
    g_eight_way_scroll = !original_scrolling;
    g_scroll_step_ns = static_cast<uint64_t>(scroll_step_ms * 1e6);
    g_ai_step_ns = static_cast<uint64_t>(ai_step_ms * 1e6);
    g_ai_view_ns = static_cast<uint64_t>(ai_view_ms * 1e6);
    for (auto& h : g_hooks) {
        for (const auto& o : m.image.objects) {
            if (h.ghidra < o.preferred_base || h.ghidra - o.preferred_base >= o.virtual_size) continue;
            uint32_t lin = o.load_base + (h.ghidra - o.preferred_base);
            uint8_t* code = m.as.host(lin);
            std::vector<uint8_t> expect = h.orig;
            if (h.reloc >= 0) {
                uint32_t g;
                std::memcpy(&g, expect.data() + h.reloc, 4);
                uint32_t l = game_lin(g);
                std::memcpy(expect.data() + h.reloc, &l, 4);
            }
            if (std::memcmp(code, expect.data(), expect.size()) != 0) break;
            code[0] = 0xCD;
            code[1] = kVecHook;
            std::memset(code + 2, 0x90, h.orig.size() - 2);  // nop padding
            h.lin = lin;
        }
        if (!h.lin) FG_LOG("hook at ghidra %08x not installed (unexpected code there)", h.ghidra);
    }
}

}  // namespace fg
