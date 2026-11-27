// SPDX-License-Identifier: GPL-2.0-or-later
// VGA/VESA video emulation.
//
// Mode 101h (640x480x256) is a banked VBE 1.2 mode: the game draws into the
// 64 KB window at linear 0xA0000 (ordinary game memory); switching banks
// copies the window to and from `vram_`. The UI thread takes snapshots of
// the visible picture through `snapshot()`.
#pragma once
#include <cstdint>
#include <mutex>
#include <vector>

#include "services.h"

namespace fg {

struct Frame {
    int width = 0, height = 0;
    std::vector<uint32_t> pixels;  // XRGB8888
};

class Video {
public:
    static constexpr uint32_t kVramSize = 1024 * 1024;
    static constexpr uint32_t kWindow = 0xA0000, kWindowSize = 0x10000;

    Video() : vram_(kVramSize) {}

    // Game thread (trap handlers).
    bool int10(RealModeRegs& r);
    void set_bank(uint16_t bank);
    void dac_write_index(uint8_t i);
    void dac_read_index(uint8_t i);
    void dac_write(uint8_t v);
    uint8_t dac_read();
    uint8_t input_status();  // port 3DAh

    // UI thread.
    bool snapshot(Frame& out);

    uint16_t mode() const { return mode_; }

private:
    void set_mode(uint16_t mode, bool clear);
    bool vbe(RealModeRegs& r);
    void sync_window_to_vram();  // caller holds mu_

    std::mutex mu_;
    std::vector<uint8_t> vram_;
    uint16_t mode_ = 0x03;
    uint16_t bank_ = 0;
    uint32_t display_start_ = 0;  // byte offset into vram
    uint16_t width_ = 80, height_ = 25, pitch_ = 0;
    uint8_t palette_[256][3] = {};  // 6-bit DAC values
    uint8_t dac_windex_ = 0, dac_wcomp_ = 0, dac_rindex_ = 0, dac_rcomp_ = 0;
};

extern Video g_video;

}  // namespace fg
