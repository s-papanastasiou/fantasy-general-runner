// SPDX-License-Identifier: GPL-2.0-or-later
// CD-audio playback: decodes the Ogg tracks standing in for the CD's audio
// tracks and plays them through SDL3. Controlled by the MSCDEX emulation
// (game thread); decoding runs on SDL's audio thread.
#pragma once
#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

#include "cdrom.h"

struct stb_vorbis;

namespace fg {

class CdAudio {
public:
    // Opens an audio stream (SDL audio must be initialised). Call once from
    // the main thread after the CD track list is known.
    bool init(const std::vector<CdTrack>& tracks);
    void shutdown();

    // Game thread.
    void play(uint32_t start_lba, uint32_t sectors);
    void pause();
    void resume();
    void stop();
    void set_volume(float v);  // 0..1

private:
    static void SDLCALL feed(void* self, SDL_AudioStream* stream, int additional, int total);
    void feed_locked(SDL_AudioStream* stream, int bytes);
    bool open_track(size_t index, uint32_t frame_offset);  // stream lock held
    void close_track();                                    // stream lock held

    static constexpr uint32_t kFramesPerSector = 588;  // 44100 / 75

    std::vector<CdTrack> tracks_;
    SDL_AudioStream* stream_ = nullptr;
    stb_vorbis* vorbis_ = nullptr;
    size_t track_ = 0;            // index into tracks_ of the open track
    int channels_ = 2;
    uint64_t frames_left_ = 0;    // frames still to play in the current play request
    bool paused_ = false;
    float volume_ = 1.0f;
    std::vector<float> buf_;
};

extern CdAudio g_cd_audio;

}  // namespace fg
