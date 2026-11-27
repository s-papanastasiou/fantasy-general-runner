// SPDX-License-Identifier: GPL-2.0-or-later
// Mouse (int 33h driver) and keyboard (BIOS int 16h) emulation.
// The UI thread feeds events; the game thread reads them in trap handlers.
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>

#include "services.h"

namespace fg {

class Input {
public:
    // UI thread.
    // Host pointer position in screen pixels of the current mode (width x height);
    // scaled into the coordinate range the game set (functions 07h/08h).
    void mouse_moved(int x, int y, int width = 640, int height = 480);
    void mouse_button(int button, bool down);  // 0 left, 1 right, 2 middle
    void key(uint16_t bios_code);          // (scancode << 8) | ascii
    // Arrow keys held down (kScrollUp...), for scrolling the battle map.
    static constexpr uint8_t kScrollUp = 1, kScrollDown = 2, kScrollLeft = 4, kScrollRight = 8;
    void scroll_key(uint8_t bit, bool down) {
        if (down) scroll_keys_.fetch_or(bit);
        else scroll_keys_.fetch_and(static_cast<uint8_t>(~bit));
    }
    void release_scroll_keys() { scroll_keys_ = 0; }
    uint8_t scroll_keys() const { return scroll_keys_; }

    // Game thread.
    bool has_key();
    uint16_t pop_key();  // 0 if none
    bool int33(RealModeRegs& r);
    bool int16(RealModeRegs& r);

    // Event-handler registration (function 0Ch), for asynchronous delivery.
    uint16_t handler_mask() const { return handler_mask_; }
    uint32_t handler_addr() const { return handler_addr_; }

private:
    std::atomic<uint8_t> scroll_keys_{0};
    std::mutex mu_;
    int x_ = 320, y_ = 240;
    int min_x_ = 0, max_x_ = 639, min_y_ = 0, max_y_ = 479;
    uint16_t buttons_ = 0;
    uint16_t press_count_[3] = {}, release_count_[3] = {};
    int press_x_[3] = {}, press_y_[3] = {}, release_x_[3] = {}, release_y_[3] = {};
    int cursor_shown_ = -1;  // driver's hide counter (show when >= 0)
    uint16_t handler_mask_ = 0;
    uint32_t handler_addr_ = 0;
    int last_mickey_x_ = 320, last_mickey_y_ = 240;
    // The game's real-mode event handler stores BX/CX/DX at this linear
    // address (0 = no recognised handler installed).
    uint32_t handler_store_ = 0;
    void run_event_handler();  // caller holds mu_
    std::deque<uint16_t> keys_;
    uint8_t dos_pending_scan_ = 0;  // second half of an extended key for DOS reads
public:
    // DOS character input (int 21h 01h/06h/07h/08h): ASCII, or 0 then scan code.
    bool dos_has_char();
    uint8_t dos_read_char();
};

extern Input g_input;

}  // namespace fg
