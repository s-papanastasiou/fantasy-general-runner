// SPDX-License-Identifier: GPL-2.0-or-later
#include "input.h"

#include <algorithm>
#include <cstring>

#include "idle.h"
#include "log.h"

namespace fg {

Input g_input;

void Input::run_event_handler() {
    // Equivalent of the game's real-mode handler (see int33 function 0Ch).
    if (!handler_store_) return;
    AddressSpace& as = g_machine->as;
    as.write<uint16_t>(handler_store_, buttons_);
    as.write<uint16_t>(handler_store_ + 2, static_cast<uint16_t>(x_));
    as.write<uint16_t>(handler_store_ + 4, static_cast<uint16_t>(y_));
}

void Input::mouse_moved(int px, int py, int width, int height) {
    std::lock_guard lock(mu_);
    px = std::clamp(px, 0, width - 1);
    py = std::clamp(py, 0, height - 1);
    int x = min_x_ + px * (max_x_ - min_x_ + 1) / width;
    int y = min_y_ + py * (max_y_ - min_y_ + 1) / height;
    if (x == x_ && y == y_) return;
    x_ = x;
    y_ = y;
    if (handler_mask_ & 0x01) run_event_handler();
}

void Input::mouse_button(int b, bool down) {
    if (b < 0 || b > 2) return;
    std::lock_guard lock(mu_);
    uint16_t bit = static_cast<uint16_t>(1 << b);
    if (down && !(buttons_ & bit)) { ++press_count_[b]; press_x_[b] = x_; press_y_[b] = y_; buttons_ |= bit; }
    if (!down && (buttons_ & bit)) { ++release_count_[b]; release_x_[b] = x_; release_y_[b] = y_; buttons_ &= ~bit; }
    if (handler_mask_ & 0x7E) run_event_handler();
}

void Input::key(uint16_t code) {
    std::lock_guard lock(mu_);
    if (keys_.size() < 16) keys_.push_back(code);  // BIOS buffer holds 15 keys
}

bool Input::has_key() { std::lock_guard lock(mu_); return !keys_.empty(); }

uint16_t Input::pop_key() {
    std::lock_guard lock(mu_);
    if (keys_.empty()) return 0;
    uint16_t k = keys_.front();
    keys_.pop_front();
    return k;
}

bool Input::dos_has_char() {
    std::lock_guard lock(mu_);
    return dos_pending_scan_ || !keys_.empty();
}

uint8_t Input::dos_read_char() {
    {
        std::lock_guard lock(mu_);
        if (dos_pending_scan_) { uint8_t s = dos_pending_scan_; dos_pending_scan_ = 0; return s; }
    }
    uint16_t k = pop_key();
    if (!k) return 0;
    if ((k & 0xFF) == 0 || (k & 0xFF) == 0xE0) {
        std::lock_guard lock(mu_);
        dos_pending_scan_ = static_cast<uint8_t>(k >> 8);
        return 0;
    }
    return static_cast<uint8_t>(k);
}

bool Input::int33(RealModeRegs& r) {
    std::lock_guard lock(mu_);
    uint16_t ax = r.eax & 0xFFFF;
    auto set = [](uint32_t& reg, uint16_t v) { reg = (reg & 0xFFFF0000u) | v; };
    auto s16 = [](uint32_t v) { return static_cast<int16_t>(v & 0xFFFF); };
    switch (ax) {
    case 0x0000: case 0x0021:  // reset: installed, 2 buttons
        cursor_shown_ = -1;
        min_x_ = 0; max_x_ = 639; min_y_ = 0; max_y_ = 479;
        set(r.eax, 0xFFFF);
        set(r.ebx, 2);
        return true;
    case 0x0001: if (cursor_shown_ < 0) ++cursor_shown_; return true;
    case 0x0002: --cursor_shown_; return true;
    case 0x0003:
        set(r.ebx, buttons_);
        set(r.ecx, static_cast<uint16_t>(x_));
        set(r.edx, static_cast<uint16_t>(y_));
        return true;
    case 0x0004:
        x_ = std::clamp<int>(s16(r.ecx), min_x_, max_x_);
        y_ = std::clamp<int>(s16(r.edx), min_y_, max_y_);
        return true;
    case 0x0005: case 0x0006: {  // button press / release data
        int b = std::min<int>(r.ebx & 0xFFFF, 2);
        bool press = ax == 0x0005;
        set(r.eax, buttons_);
        set(r.ebx, press ? press_count_[b] : release_count_[b]);
        set(r.ecx, static_cast<uint16_t>(press ? press_x_[b] : release_x_[b]));
        set(r.edx, static_cast<uint16_t>(press ? press_y_[b] : release_y_[b]));
        (press ? press_count_[b] : release_count_[b]) = 0;
        return true;
    }
    case 0x0007:
        min_x_ = std::min(s16(r.ecx), s16(r.edx)); max_x_ = std::max(s16(r.ecx), s16(r.edx));
        x_ = std::clamp(x_, min_x_, max_x_);
        return true;
    case 0x0008:
        min_y_ = std::min(s16(r.ecx), s16(r.edx)); max_y_ = std::max(s16(r.ecx), s16(r.edx));
        y_ = std::clamp(y_, min_y_, max_y_);
        return true;
    case 0x0009: case 0x000A: return true;  // cursor shape: the game draws its own cursor
    case 0x000B: {                          // motion counters (mickeys, 8 per 8 pixels)
        set(r.ecx, static_cast<uint16_t>(x_ - last_mickey_x_));
        set(r.edx, static_cast<uint16_t>(y_ - last_mickey_y_));
        last_mickey_x_ = x_;
        last_mickey_y_ = y_;
        return true;
    }
    case 0x000C: {  // event handler: CX mask, ES:DX real-mode address
        handler_mask_ = r.ecx & 0xFFFF;
        handler_addr_ = (uint32_t(r.es) << 16) | (r.edx & 0xFFFF);
        handler_store_ = 0;
        if (!handler_mask_) return true;
        // Real-mode code cannot run here. Recognise the game's handler
        // (mov si,0; mov ds,si; mov [a],bx; mov [a+2],cx; mov [a+4],dx; retf)
        // and perform its effect directly.
        const uint8_t* p = g_machine->as.host((uint32_t(r.es) << 4) + (r.edx & 0xFFFF));
        uint16_t a = 0, b = 0, c = 0;
        std::memcpy(&a, p + 7, 2);
        std::memcpy(&b, p + 11, 2);
        std::memcpy(&c, p + 15, 2);
        if (std::memcmp(p, "\xBE\x00\x00\x8E\xDE\x89\x1E", 7) == 0 && p[9] == 0x89 && p[10] == 0x0E &&
            p[13] == 0x89 && p[14] == 0x16 && p[17] == 0xCB && b == a + 2 && c == a + 4) {
            handler_store_ = a;
            run_event_handler();
            FG_LOG("mouse event handler %04x:%04x (stores state at 0000:%04x), mask %04x", r.es, r.edx & 0xFFFF, a,
                   handler_mask_);
        } else {
            FG_LOG("unrecognised real-mode mouse event handler %04x:%04x, mask %04x: ignored", r.es,
                   r.edx & 0xFFFF, handler_mask_);
        }
        return true;
    }
    case 0x000F: case 0x0013: case 0x001A: case 0x001D: return true;  // ratios, sensitivity, page
    case 0x001B: set(r.ebx, 50); set(r.ecx, 50); set(r.edx, 50); return true;
    case 0x0024: set(r.ebx, 0x0626); set(r.ecx, 0x0400); return true;  // driver 6.26, PS/2
    default:
        FG_LOG("unsupported int 33h AX=%04x", ax);
        return false;
    }
}

bool Input::int16(RealModeRegs& r) {
    static IdleDetector idle;
    std::unique_lock lock(mu_);
    uint8_t ah = (r.eax >> 8) & 0xFF;
    switch (ah) {
    case 0x00: case 0x10:  // read key (non-blocking here: returns 0 if none)
        if (!keys_.empty()) { r.eax = (r.eax & 0xFFFF0000u) | keys_.front(); keys_.pop_front(); }
        else r.eax &= 0xFFFF0000u;
        return true;
    case 0x01: case 0x11:  // key available? ZF=1 if none
        if (keys_.empty()) {
            r.flags |= 0x40;
            if (idle.spinning()) { lock.unlock(); idle.sleep_ns(1000000); }
        } else { r.flags &= ~0x40; r.eax = (r.eax & 0xFFFF0000u) | keys_.front(); }
        return true;
    case 0x02: case 0x12: r.eax &= 0xFFFFFF00u; return true;  // shift flags
    default:
        FG_LOG("unsupported int 16h AX=%04x", r.eax & 0xFFFF);
        return false;
    }
}

}  // namespace fg
