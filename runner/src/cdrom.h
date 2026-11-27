// SPDX-License-Identifier: GPL-2.0-or-later
// MSCDEX (int 2Fh AX=15xxh) and the CD-ROM device driver requests behind it.
//
// The disc layout comes from the GOG files: track 1 = data (game.gog, raw
// 2352-byte sectors), tracks 2..N = audio (MUSIC/TrackNN.ogg). Audio playback
// is tracked in time only for now; sound output comes with the audio phase.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "services.h"

namespace fg {

struct CdTrack {
    int number;
    bool audio;
    uint32_t start_lba;  // logical block address (Red Book address = LBA + 150)
    uint32_t sectors;
    std::string file;    // .ogg for audio tracks
};

class CdRom {
public:
    // drive: 0 = A:, 3 = D:
    bool init(const std::string& game_dir, int drive);
    bool present() const { return !tracks_.empty(); }
    int drive() const { return drive_; }

    // int 2Fh AX=15xxh in real mode. Returns false if unsupported.
    bool mscdex(RealModeRegs& r);

    const std::vector<CdTrack>& tracks() const { return tracks_; }
    // Currently playing track (0 = none) and position, for the audio backend.
    int playing_track() const;

private:
    void device_request(uint32_t req_lin);
    uint16_t ioctl_input(uint32_t buf_lin);
    uint16_t ioctl_output(uint32_t buf_lin);
    uint32_t now_lba() const;  // current play position (LBA)
    bool is_playing() const;

    std::vector<CdTrack> tracks_;
    uint32_t leadout_lba_ = 0;
    int drive_ = 3;
    // Play state
    uint32_t play_start_lba_ = 0, play_sectors_ = 0;
    double play_started_ = 0, paused_at_ = -1;  // seconds (monotonic)
};

extern CdRom g_cdrom;

}  // namespace fg
