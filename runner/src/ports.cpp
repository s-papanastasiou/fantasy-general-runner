// SPDX-License-Identifier: GPL-2.0-or-later
// Emulated I/O ports (PIT, PIC, VGA). Unknown ports are logged once.
#include <ctime>
#include <set>

#include "cpu_backend.h"
#include "idle.h"
#include "log.h"
#include "services.h"
#include "video.h"

namespace fg {

namespace {

std::set<uint16_t> g_reported;
uint16_t g_pit_reload = 0;  // 0 = 65536

void report_once(const char* dir, uint16_t port, uint32_t v) {
    if (g_reported.insert(port).second) FG_LOG("unhandled port %s %03xh (value %x)", dir, port, v);
}

// PIT channel 0 counter derived from host time (1.193182 MHz).
uint16_t pit_counter() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ticks = (uint64_t(ts.tv_sec) * 1193182ull) + (uint64_t(ts.tv_nsec) * 1193182ull) / 1000000000ull;
    uint32_t period = g_pit_reload ? g_pit_reload : 65536;
    return static_cast<uint16_t>(period - (ticks % period));
}

}  // namespace

uint32_t port_in(uint16_t port, int size) {
    static bool latch_low = true;
    static uint16_t latched = 0;
    switch (port) {
    case 0x40: {  // PIT channel 0, read low byte then high byte
        if (latch_low) {
            // Smacker waits for its next frame by re-reading the counter.
            static IdleDetector idle;
            if (idle.spinning()) idle.sleep_ns(500000);
            latched = pit_counter();
        }
        uint8_t v = latch_low ? latched & 0xFF : latched >> 8;
        latch_low = !latch_low;
        return v;
    }
    case 0x3DA: return g_video.input_status();
    case 0x3C9: return g_video.dac_read();
    case 0x3CC: return 0xE3;  // VGA misc output: colour, 480-line timing
    default:
        report_once("in", port, 0);
        return size == 1 ? 0xFF : 0xFFFFFFFF;
    }
}

void port_out(uint16_t port, uint32_t value, int size) {
    static bool reload_low = true;
    switch (port) {
    case 0x20: return;  // PIC EOI
    case 0x21: return;  // PIC mask
    case 0x43: reload_low = true; return;  // PIT mode
    case 0x40:
        if (reload_low) g_pit_reload = (g_pit_reload & 0xFF00) | (value & 0xFF);
        else {
            g_pit_reload = static_cast<uint16_t>((g_pit_reload & 0x00FF) | ((value & 0xFF) << 8));
            set_timer_rate(1193182.0 / (g_pit_reload ? g_pit_reload : 65536));
        }
        reload_low = !reload_low;
        return;
    case 0x3C7: g_video.dac_read_index(static_cast<uint8_t>(value)); return;
    case 0x3C8: g_video.dac_write_index(static_cast<uint8_t>(value)); return;
    case 0x3C9: g_video.dac_write(static_cast<uint8_t>(value)); return;
    default: report_once("out", port, value);
    }
}

}  // namespace fg
