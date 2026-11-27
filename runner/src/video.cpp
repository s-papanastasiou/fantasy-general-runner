// SPDX-License-Identifier: GPL-2.0-or-later
#include "video.h"

#include <algorithm>
#include <cstring>
#include <ctime>

#include "idle.h"
#include "log.h"

namespace fg {

Video g_video;

namespace {

// Where the emulated VBE BIOS keeps its strings and mode list (DOS memory,
// below the conventional-memory pool).
constexpr uint32_t kVbeData = 0x0700;
constexpr uint32_t kVbeOem = kVbeData, kVbeModes = kVbeData + 0x40;
// Fake real-mode address of the VBE window function (DPMI 0301h target).
constexpr uint16_t kWinFuncSeg = 0xF000, kWinFuncOff = 0x2000;

struct ModeInfo { uint16_t mode, width, height; };
constexpr ModeInfo kVbeModeList[] = {{0x100, 640, 400}, {0x101, 640, 480}};

const ModeInfo* find_vbe_mode(uint16_t m) {
    for (const auto& i : kVbeModeList) if (i.mode == (m & 0x1FF)) return &i;
    return nullptr;
}

}  // namespace

void Video::sync_window_to_vram() {
    if (mode_ >= 0x100) {
        uint32_t off = uint32_t(bank_) * kWindowSize;
        if (off + kWindowSize <= kVramSize) std::memcpy(&vram_[off], g_machine->as.host(kWindow), kWindowSize);
    }
}

void Video::set_mode(uint16_t mode, bool clear) {
    std::lock_guard lock(mu_);
    uint16_t m = mode & 0x1FF;
    mode_ = m;
    bank_ = 0;
    display_start_ = 0;
    if (const ModeInfo* vi = find_vbe_mode(m)) { width_ = vi->width; height_ = vi->height; pitch_ = vi->width; }
    else if (m == 0x13) { width_ = 320; height_ = 200; pitch_ = 320; }
    else { width_ = 80; height_ = 25; pitch_ = 0; }
    if (clear) {
        std::memset(vram_.data(), 0, vram_.size());
        std::memset(g_machine->as.host(kWindow), 0, kWindowSize);
    }
    g_machine->as.write<uint8_t>(0x449, static_cast<uint8_t>(m < 0x100 ? m : 0x62));
    FG_LOG("video mode %03xh (%ux%u)", m, width_, height_);
}

void Video::set_bank(uint16_t bank) {
    std::lock_guard lock(mu_);
    if (bank == bank_ || uint32_t(bank) * kWindowSize >= kVramSize) return;
    sync_window_to_vram();
    bank_ = bank;
    std::memcpy(g_machine->as.host(kWindow), &vram_[uint32_t(bank) * kWindowSize], kWindowSize);
}

bool Video::vbe(RealModeRegs& r) {
    AddressSpace& as = g_machine->as;
    uint16_t ax = r.eax & 0xFFFF;
    uint32_t es_di = (uint32_t(r.es) << 4) + (r.edi & 0xFFFF);
    auto set_ax = [&](uint16_t v) { r.eax = (r.eax & 0xFFFF0000u) | v; };
    auto set_bx = [&](uint16_t v) { r.ebx = (r.ebx & 0xFFFF0000u) | v; };
    auto set_cx = [&](uint16_t v) { r.ecx = (r.ecx & 0xFFFF0000u) | v; };
    auto set_dx = [&](uint16_t v) { r.edx = (r.edx & 0xFFFF0000u) | v; };
    constexpr uint16_t kOk = 0x004F, kFail = 0x014F;

    switch (ax) {
    case 0x4F00: {  // controller information
        static const char oem[] = "FG runner VBE";
        std::memcpy(as.host(kVbeOem), oem, sizeof oem);
        uint32_t n = 0;
        for (const auto& m : kVbeModeList) as.write<uint16_t>(kVbeModes + 2 * n++, m.mode);
        as.write<uint16_t>(kVbeModes + 2 * n, 0xFFFF);
        uint8_t* p = as.host(es_di);
        std::memset(p, 0, 256);
        std::memcpy(p, "VESA", 4);
        as.write<uint16_t>(es_di + 4, 0x0102);
        as.write<uint32_t>(es_di + 6, (kVbeOem >> 4 << 16) | (kVbeOem & 0xF));
        as.write<uint32_t>(es_di + 10, 0);  // capabilities: 6-bit DAC
        as.write<uint32_t>(es_di + 14, (kVbeModes >> 4 << 16) | (kVbeModes & 0xF));
        as.write<uint16_t>(es_di + 18, static_cast<uint16_t>(kVramSize / 65536));
        set_ax(kOk);
        return true;
    }
    case 0x4F01: {  // mode information
        const ModeInfo* vi = find_vbe_mode(r.ecx & 0xFFFF);
        if (!vi) { set_ax(kFail); return true; }
        uint8_t* p = as.host(es_di);
        std::memset(p, 0, 256);
        as.write<uint16_t>(es_di + 0x00, 0x001F);  // supported, extended info, BIOS output, colour, graphics
        p[0x02] = 0x07;                            // window A: exists, readable, writable
        p[0x03] = 0x00;
        as.write<uint16_t>(es_di + 0x04, 64);      // granularity KB
        as.write<uint16_t>(es_di + 0x06, 64);      // window size KB
        as.write<uint16_t>(es_di + 0x08, 0xA000);  // window A segment
        as.write<uint16_t>(es_di + 0x0A, 0);
        as.write<uint32_t>(es_di + 0x0C, (uint32_t(kWinFuncSeg) << 16) | kWinFuncOff);
        as.write<uint16_t>(es_di + 0x10, vi->width);  // bytes per scan line
        as.write<uint16_t>(es_di + 0x12, vi->width);
        as.write<uint16_t>(es_di + 0x14, vi->height);
        p[0x16] = 8; p[0x17] = 16;                 // character cell
        p[0x18] = 1;                               // planes
        p[0x19] = 8;                               // bits per pixel
        p[0x1A] = 1;                               // banks
        p[0x1B] = 4;                               // memory model: packed pixel
        p[0x1C] = 0;                               // bank size
        p[0x1D] = static_cast<uint8_t>(kVramSize / (uint32_t(vi->width) * vi->height) - 1);  // image pages
        p[0x1E] = 1;
        set_ax(kOk);
        return true;
    }
    case 0x4F02: {
        uint16_t m = r.ebx & 0xFFFF;
        if (!find_vbe_mode(m) && (m & 0x1FF) >= 0x100) { set_ax(kFail); return true; }
        set_mode(m, !(m & 0x8000));
        set_ax(kOk);
        return true;
    }
    case 0x4F03: set_bx(mode_); set_ax(kOk); return true;
    case 0x4F05: {
        uint8_t bh = (r.ebx >> 8) & 0xFF;
        if ((r.ebx & 0xFF) != 0) { set_ax(kFail); return true; }  // only window A
        if (bh == 0) set_bank(r.edx & 0xFFFF);
        else set_dx(bank_);
        set_ax(kOk);
        return true;
    }
    case 0x4F06: {  // logical scan line length
        uint8_t bl = r.ebx & 0xFF;
        if (bl == 0) pitch_ = r.ecx & 0xFFFF;
        set_bx(pitch_); set_cx(pitch_); set_dx(static_cast<uint16_t>(kVramSize / (pitch_ ? pitch_ : 1)));
        set_ax(kOk);
        return true;
    }
    case 0x4F07: {  // display start
        uint8_t bl = r.ebx & 0xFF;
        if (bl == 0x00 || bl == 0x80) {
            std::lock_guard lock(mu_);
            display_start_ = (r.edx & 0xFFFF) * uint32_t(pitch_) + (r.ecx & 0xFFFF);
        } else if (bl == 0x01) {
            set_cx(static_cast<uint16_t>(pitch_ ? display_start_ % pitch_ : 0));
            set_dx(static_cast<uint16_t>(pitch_ ? display_start_ / pitch_ : 0));
        }
        set_ax(kOk);
        return true;
    }
    case 0x4F08:  // DAC width: always 6 bits
        r.ebx = (r.ebx & 0xFFFF00FFu) | (6 << 8);
        set_ax(kOk);
        return true;
    default:
        set_ax(kFail);  // 4F04 (state), 4F0A (PM interface), ...: report unsupported to the caller
        FG_TRACE("VBE function %04xh not supported", ax);
        return true;
    }
}

bool Video::int10(RealModeRegs& r) {
    AddressSpace& as = g_machine->as;
    uint16_t ax = r.eax & 0xFFFF;
    uint8_t ah = ax >> 8, al = ax & 0xFF;
    auto set_ax = [&](uint16_t v) { r.eax = (r.eax & 0xFFFF0000u) | v; };
    auto set_bx = [&](uint16_t v) { r.ebx = (r.ebx & 0xFFFF0000u) | v; };
    if (ah == 0x4F) return vbe(r);
    switch (ah) {
    case 0x00: set_mode(al & 0x7F, !(al & 0x80)); return true;
    case 0x0F:
        set_ax(static_cast<uint16_t>(((mode_ == 0x13 ? 40 : 80) << 8) | (mode_ < 0x100 ? mode_ : 0x62)));
        set_bx(0);
        return true;
    case 0x10: {  // palette
        uint32_t es_dx = (uint32_t(r.es) << 4) + (r.edx & 0xFFFF);
        uint16_t first = r.ebx & 0xFFFF, count = r.ecx & 0xFFFF;
        std::lock_guard lock(mu_);
        switch (al) {
        case 0x10:
            palette_[first & 0xFF][0] = (r.edx >> 8) & 0x3F;
            palette_[first & 0xFF][1] = (r.ecx >> 8) & 0x3F;
            palette_[first & 0xFF][2] = r.ecx & 0x3F;
            return true;
        case 0x12:
            for (uint16_t i = 0; i < count && first + i < 256; ++i)
                for (int c = 0; c < 3; ++c) palette_[first + i][c] = as.host(es_dx)[3 * i + c] & 0x3F;
            return true;
        case 0x15:
            r.edx = (r.edx & 0xFFFF00FFu) | (palette_[first & 0xFF][0] << 8);
            r.ecx = (r.ecx & 0xFFFF0000u) | (palette_[first & 0xFF][1] << 8) | palette_[first & 0xFF][2];
            return true;
        case 0x17:
            for (uint16_t i = 0; i < count && first + i < 256; ++i)
                for (int c = 0; c < 3; ++c) as.host(es_dx)[3 * i + c] = palette_[first + i][c];
            return true;
        default: return true;  // EGA palette registers, border colour: nothing to show
        }
    }
    case 0x1A:
        if (al == 0x00) { set_ax(0x001A); set_bx(0x0008); return true; }  // VGA, colour display
        break;
    case 0x12:
        if ((r.ebx & 0xFF) == 0x10) { set_bx(0x0003); r.ecx = (r.ecx & 0xFFFF0000u) | 0x0009; return true; }
        break;
    default: break;
    }
    FG_LOG("unsupported int 10h AX=%04x", ax);
    return false;
}

void Video::dac_write_index(uint8_t i) { std::lock_guard lock(mu_); dac_windex_ = i; dac_wcomp_ = 0; }
void Video::dac_read_index(uint8_t i) { std::lock_guard lock(mu_); dac_rindex_ = i; dac_rcomp_ = 0; }

void Video::dac_write(uint8_t v) {
    std::lock_guard lock(mu_);
    palette_[dac_windex_][dac_wcomp_] = v & 0x3F;
    if (++dac_wcomp_ == 3) { dac_wcomp_ = 0; ++dac_windex_; }
}

uint8_t Video::dac_read() {
    std::lock_guard lock(mu_);
    uint8_t v = palette_[dac_rindex_][dac_rcomp_];
    if (++dac_rcomp_ == 3) { dac_rcomp_ = 0; ++dac_rindex_; }
    return v;
}

uint8_t Video::input_status() {
    // 60 Hz refresh with a ~1.4 ms vertical retrace; bit 0 (display disabled)
    // is set during retrace too.
    constexpr uint64_t kFrameNs = 1000000000ull / 60, kRetraceNs = kFrameNs * 85 / 1000;
    static IdleDetector idle;
    uint64_t phase = idle_now_ns() % kFrameNs;
    if (phase < kRetraceNs) {
        idle.reset();
        return 0x09;
    }
    // A loop waiting for the retrace to begin: sleep towards it in slices of
    // at most 1 ms. Timer interrupts wait while a trap handler sleeps, and
    // the game's mouse cursor is drawn from a timer callback (0x3cd78):
    // sleeping a whole frame at once made the cursor stutter during palette
    // fades, which wait for a retrace after every step.
    if (idle.spinning()) idle.sleep_ns(std::min<uint64_t>(1000000, kFrameNs - phase));
    return 0x00;
}

bool Video::snapshot(Frame& out) {
    std::lock_guard lock(mu_);
    uint32_t pal[256];
    for (int i = 0; i < 256; ++i) {
        auto c8 = [&](int c) { uint8_t v = palette_[i][c]; return uint32_t((v << 2) | (v >> 4)); };
        pal[i] = (c8(0) << 16) | (c8(1) << 8) | c8(2);
    }
    const uint8_t* window = g_machine->as.host(kWindow);
    if (mode_ < 0x100 && mode_ != 0x13) {  // text mode: show nothing
        out.width = 640; out.height = 400;
        out.pixels.assign(640 * 400, 0);
        return true;
    }
    out.width = width_;
    out.height = height_;
    out.pixels.resize(size_t(width_) * height_);
    uint32_t pitch = pitch_ ? pitch_ : width_;
    uint32_t bank_lo = uint32_t(bank_) * kWindowSize;
    for (uint32_t y = 0; y < height_; ++y) {
        uint32_t row = display_start_ + y * pitch;
        uint32_t* dst = &out.pixels[size_t(y) * width_];
        for (uint32_t x = 0; x < width_; ++x) {
            uint32_t o = row + x;
            uint8_t v;
            if (mode_ == 0x13) v = o < kWindowSize ? window[o] : 0;
            else if (o - bank_lo < kWindowSize) v = window[o - bank_lo];  // currently mapped bank
            else v = o < kVramSize ? vram_[o] : 0;
            dst[x] = pal[v];
        }
    }
    return true;
}

}  // namespace fg
