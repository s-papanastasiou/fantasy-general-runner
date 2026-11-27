// SPDX-License-Identifier: GPL-2.0-or-later
#include "dig_driver.h"

#include <algorithm>
#include <cstring>

#include "audio_lock.h"
#include "log.h"

namespace fg {

DigDriver g_dig;

namespace {

SDL_AudioSpec spec_for(uint16_t format, uint16_t rate) {
    // Format bits as in SB16.DIG's tables: bit 0 = 16-bit signed (else 8-bit
    // unsigned), bit 1 = stereo.
    return SDL_AudioSpec{(format & 1) ? SDL_AUDIO_S16LE : SDL_AUDIO_U8, (format & 2) ? 2 : 1,
                         rate ? rate : 22050};
}

}  // namespace

bool DigDriver::init() {
    SDL_AudioSpec spec = spec_for(0, 22050);
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &DigDriver::feed, this);
    if (!stream_) {
        FG_LOG("cannot open audio device for sound effects: %s", SDL_GetError());
        return false;
    }
    SDL_ResumeAudioStreamDevice(stream_);
    return true;
}

void DigDriver::shutdown() {
    if (stream_) SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
}

bool DigDriver::call(RealModeRegs& r) {
    Machine& m = *g_machine;
    AddressSpace& as = m.as;
    uint32_t vec = m.rm_vectors[0x66];
    uint16_t seg = static_cast<uint16_t>(vec >> 16);
    uint32_t image = uint32_t(seg) << 4;
    if (std::memcmp(as.host(image), "AIL3DIG", 7) != 0) return false;

    StreamLock lock(stream_);
    if (image != image_) {
        image_ = image;
        image_seg_ = seg;
        FG_LOG("sound: Miles loaded \"%s\" at %04x:0000; emulating it", reinterpret_cast<char*>(as.host(image + 0xBA)),
               seg);
    }
    uint16_t ax = r.eax & 0xFFFF;
    auto set = [](uint32_t& reg, uint16_t v) { reg = (reg & 0xFFFF0000u) | v; };
    auto w16 = [&](uint32_t off, uint16_t v) { as.write<uint16_t>(image + off, v); };
    uint16_t result = 0xFFFF;

    switch (ax) {
    case 0x300:  // describe driver: config list, environment variable name, no service calls needed
        w16(0x0C, 0x50E); w16(0x0E, seg);
        w16(0x10, 3);
        w16(0x12, 0x506); w16(0x14, seg);
        w16(0x2E, 0xFFFF);
        w16(kDst + 8, 0xFFFF);
        result = 1;
        break;
    case 0x301:  // DDT and DST addresses
        result = kDdt;
        set(r.edx, seg);
        set(r.ebx, kDst);
        set(r.ecx, seg);
        break;
    case 0x302: result = 1; break;
    case 0x303:  // parse environment: IO block from defaults
        std::memset(as.host(image + 0x16), 0xFF, 0x18);
        w16(0x16, 0x220);
        result = 0x220;
        break;
    case 0x304:  // verify: report a SB16 (DSP 4.12) with IRQ 5, DMA 1/5
        w16(0x16, 0x220); w16(0x18, 5); w16(0x1A, 1); w16(0x1C, 5);
        result = 0x040C;
        break;
    case 0x305: result = 1; break;
    case 0x306: state_ = kIdle; w16(kDst + 8, 0xFFFF); result = 1; break;
    case 0x400: {
        int16_t v = static_cast<int16_t>(r.edx & 0xFFFF);
        if (v != -1) {
            volume_ = std::clamp<int>(v, 0, 127);
            if (stream_) SDL_SetAudioStreamGain(stream_, volume_ / 127.0f);
        }
        result = static_cast<uint16_t>(volume_);
        break;
    }
    case 0x401: {
        uint16_t fmt = r.edx & 0xFFFF;
        if (fmt >= 4) break;
        start(fmt, r.ecx & 0xFFFF);
        result = 1;
        break;
    }
    case 0x402:
        result = state_ == kPlaying ? 1 : 0;
        if (state_ == kPlaying) state_ = kStopRequested;
        break;
    case 0x403: case 0x404: result = 1; break;  // recording: accepted, never happens
    default: break;
    }
    FG_TRACE("DIG driver %03xh DX=%04x CX=%04x -> %04x", ax, r.edx & 0xFFFF, r.ecx & 0xFFFF, result);
    set(r.eax, result);
    return true;
}

void DigDriver::start(uint16_t format, uint16_t rate) {
    AddressSpace& as = g_machine->as;
    bool restart = state_ == kIdle || format != format_;
    format_ = format;
    rate_ = rate;
    if (stream_) {
        SDL_AudioSpec spec = spec_for(format, rate);
        SDL_SetAudioStreamFormat(stream_, &spec, nullptr);
    }
    state_ = kPlaying;
    if (!restart) return;  // rate change while playing (SB16.DIG applies it at the next IRQ)
    auto far_ptr = [&](uint32_t off) {
        return (uint32_t(as.read<uint16_t>(image_ + off + 2)) << 4) + as.read<uint16_t>(image_ + off);
    };
    half_[0] = far_ptr(kDst);
    half_[1] = far_ptr(kDst + 4);
    half_size_ = half_[1] > half_[0] ? half_[1] - half_[0] : 0;
    if (!as.contains(half_[0], 2 * half_size_) || half_size_ > 0x10000) {
        FG_LOG("sound: invalid DMA buffer %08x/%08x; not playing", half_[0], half_[1]);
        half_size_ = 0;
    }
    pos_ = 0;
    cur_ = 0;
    as.write<uint16_t>(image_ + kDst + 8, 0);
    if (stream_) SDL_ClearAudioStream(stream_);
    FG_LOG("sound: playback %u Hz, %s %s, 2 x %u bytes", rate, (format & 1) ? "16-bit" : "8-bit",
           (format & 2) ? "stereo" : "mono", half_size_);
}

void SDLCALL DigDriver::feed(void* self, SDL_AudioStream* stream, int additional, int /*total*/) {
    // SDL holds the stream lock here (see audio_lock.h).
    static_cast<DigDriver*>(self)->feed_locked(stream, additional);
}

void DigDriver::feed_locked(SDL_AudioStream* stream, int bytes) {
    AddressSpace& as = g_machine->as;
    while (bytes > 0 && state_ != kIdle && half_size_) {
        uint32_t n = std::min<uint32_t>(half_size_ - pos_, static_cast<uint32_t>(bytes));
        const uint8_t* src = as.host(half_[cur_] + pos_);
        SDL_PutAudioStreamData(stream, src, static_cast<int>(n));
        if (g_trace) {
            played_ += n;
            for (uint32_t i = 0; i < n; i += (format_ & 1) ? 2 : 1) {
                int v = (format_ & 1) ? std::abs(int(int16_t(src[i] | (src[i + 1] << 8)))) >> 8 : std::abs(int(src[i]) - 128);
                peak_ = std::max(peak_, v);
            }
        }
        pos_ += n;
        bytes -= static_cast<int>(n);
        if (pos_ < half_size_) continue;
        // End of a half: what the card's interrupt handler would do.
        pos_ = 0;
        if (state_ == kStopRequested) {
            state_ = kIdle;
            FG_TRACE("sound: stopped after %llu bytes, peak %d/128", static_cast<unsigned long long>(played_), peak_);
            played_ = 0;
            peak_ = 0;
            as.write<uint16_t>(image_ + kDst + 8, 0xFFFF);
            break;
        }
        cur_ ^= 1;
        as.write<uint16_t>(image_ + kDst + 8, static_cast<uint16_t>(cur_));
    }
}

}  // namespace fg
