// SPDX-License-Identifier: GPL-2.0-or-later
// Stand-in for a Miles AIL 3 digital sound driver (.DIG).
//
// Miles (linked into the game) loads a real-mode driver image such as
// SB16.DIG into DOS memory, points int 66h at it and calls it through DPMI
// 0300h. The runner cannot execute real-mode code, so it answers those calls
// itself, the way SB16.DIG does, and plays the driver's double-buffered DMA
// output through SDL3:
//
//   AX=300h describe driver      AX=400h hardware volume (DX, -1 = query)
//   AX=301h get DDT/DST pointers AX=401h start playback (DX format, CX rate)
//   AX=302h detect               AX=402h stop request (at end of half buffer)
//   AX=303h parse environment    AX=403h-405h recording (unsupported)
//   AX=304h verify device
//   AX=305h init device, 306h shut down
//
// The digital status table (DST, image offset 4FCh) holds two far pointers
// to the halves of the DMA buffer and, at +8, the half being played
// (0/1, -1 = stopped). Miles mixes into the other half; the "IRQ" here flips
// the index whenever a half has been played.
#pragma once
#include <SDL3/SDL.h>

#include <cstdint>

#include "services.h"

namespace fg {

class DigDriver {
public:
    // Call after the SDL audio subsystem is up (main thread).
    bool init();
    void shutdown();

    // Game thread: int 66h in (emulated) real mode. Returns false if int 66h
    // does not point at an AIL3DIG driver image.
    bool call(RealModeRegs& r);

private:
    static void SDLCALL feed(void* self, SDL_AudioStream* stream, int additional, int total);
    void feed_locked(SDL_AudioStream* stream, int bytes);
    void start(uint16_t format, uint16_t rate);  // stream lock held

    static constexpr uint32_t kDdt = 0x40C, kDst = 0x4FC;

    SDL_AudioStream* stream_ = nullptr;
    uint32_t image_ = 0;     // linear address of the driver image
    uint16_t image_seg_ = 0;
    enum State { kIdle = 0, kStopRequested = 2, kPlaying = 3 } state_ = kIdle;
    uint32_t half_[2] = {};  // linear addresses of the two halves
    uint32_t half_size_ = 0;
    uint32_t pos_ = 0;       // bytes played in the current half
    int cur_ = 0;
    uint16_t format_ = 0, rate_ = 0;
    int volume_ = 127;
    uint64_t played_ = 0;  // statistics for FG_TRACE
    int peak_ = 0;
};

extern DigDriver g_dig;

}  // namespace fg
