// SPDX-License-Identifier: GPL-2.0-or-later
#include "cdrom.h"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>

#include "cd_audio.h"
#include "dos_fs.h"
#include "log.h"

namespace fg {

CdRom g_cdrom;

namespace {

constexpr uint16_t kDone = 0x0100, kBusy = 0x0200, kError = 0x8000;
constexpr uint16_t kErrUnknownCommand = 0x03;

double mono_seconds() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// Duration of an Ogg Vorbis file in seconds (last granule position / sample rate).
double ogg_duration(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0;
    std::vector<char> head(64 * 1024);
    f.read(head.data(), head.size());
    head.resize(f.gcount());
    // Vorbis identification header: "\x01vorbis", sample rate at +12 (after 4-byte version, 1-byte channels).
    uint32_t rate = 0;
    for (size_t i = 0; i + 16 < head.size(); ++i)
        if (head[i] == 1 && std::memcmp(&head[i + 1], "vorbis", 6) == 0) { std::memcpy(&rate, &head[i + 12], 4); break; }
    if (!rate) return 0;
    f.clear();
    f.seekg(0, std::ios::end);
    std::streamoff size = f.tellg();
    std::streamoff from = size > 65536 ? size - 65536 : 0;
    std::vector<char> tail(size - from);
    f.seekg(from);
    f.read(tail.data(), tail.size());
    uint64_t granule = 0;
    for (size_t i = 0; i + 14 <= tail.size(); ++i)
        if (std::memcmp(&tail[i], "OggS", 4) == 0) std::memcpy(&granule, &tail[i + 6], 8);
    return double(granule) / rate;
}

uint32_t to_redbook(uint32_t lba) {  // LBA -> 00:MM:SS:FF (as stored little endian: frame, sec, min, 0)
    uint32_t a = lba + 150;
    return (a % 75) | ((a / 75 % 60) << 8) | ((a / 75 / 60) << 16);
}
uint32_t from_redbook(uint32_t rb) {
    uint32_t f = rb & 0xFF, s = (rb >> 8) & 0xFF, m = (rb >> 16) & 0xFF;
    uint32_t a = (m * 60 + s) * 75 + f;
    return a >= 150 ? a - 150 : 0;
}

}  // namespace

bool CdRom::init(const std::string& game_dir, int drive) {
    drive_ = drive;
    tracks_.clear();
    struct stat st;
    std::string image = game_dir + "/game.gog";
    if (stat(image.c_str(), &st) != 0) {
        FG_LOG("CD image %s not found: no CD drive", image.c_str());
        return false;
    }
    uint32_t lba = 0;
    tracks_.push_back({1, false, 0, static_cast<uint32_t>(st.st_size / 2352), image});
    lba = tracks_[0].sectors;
    for (int n = 2; n < 100; ++n) {
        char name[32];
        std::snprintf(name, sizeof name, "/MUSIC/Track%02d.ogg", n);
        std::string path = game_dir + name;
        if (stat(path.c_str(), &st) != 0) break;
        lba += 150;  // 2 s pregap
        uint32_t sectors = static_cast<uint32_t>(ogg_duration(path) * 75 + 0.5);
        tracks_.push_back({n, true, lba, sectors, path});
        lba += sectors;
    }
    leadout_lba_ = lba;
    FG_LOG("CD drive %c: %zu tracks (%zu audio), %u:%02u total", 'A' + drive_, tracks_.size(), tracks_.size() - 1,
           (lba / 75) / 60, (lba / 75) % 60);
    return true;
}

bool CdRom::is_playing() const {
    return play_sectors_ && paused_at_ < 0 && now_lba() < play_start_lba_ + play_sectors_;
}

uint32_t CdRom::now_lba() const {
    double t = (paused_at_ >= 0 ? paused_at_ : mono_seconds()) - play_started_;
    uint32_t pos = play_start_lba_ + static_cast<uint32_t>(t * 75);
    return std::min(pos, play_start_lba_ + play_sectors_);
}

int CdRom::playing_track() const {
    if (!is_playing()) return 0;
    uint32_t pos = now_lba();
    for (const auto& t : tracks_)
        if (t.audio && pos >= t.start_lba && pos < t.start_lba + t.sectors) return t.number;
    return 0;
}

bool CdRom::mscdex(RealModeRegs& r) {
    uint16_t ax = r.eax & 0xFFFF;
    auto set_ax = [&](uint16_t v) { r.eax = (r.eax & 0xFFFF0000u) | v; };
    auto set_bx = [&](uint16_t v) { r.ebx = (r.ebx & 0xFFFF0000u) | v; };
    auto set_cx = [&](uint16_t v) { r.ecx = (r.ecx & 0xFFFF0000u) | v; };
    uint32_t es_bx = (uint32_t(r.es) << 4) + (r.ebx & 0xFFFF);
    switch (ax) {
    case 0x1500:  // installation check: BX = number of drives, CX = first drive letter
        set_bx(present() ? 1 : 0);
        set_cx(static_cast<uint16_t>(drive_));
        return true;
    case 0x150B:  // drive check
        set_ax((r.ecx & 0xFFFF) == static_cast<uint16_t>(drive_) && present() ? 0x5AD8 : 0);
        set_bx(0xADAD);
        return true;
    case 0x150C: set_bx(0x0215); return true;  // MSCDEX 2.21
    case 0x150D: *g_machine->as.host(es_bx) = static_cast<uint8_t>(drive_); return true;
    case 0x1510: device_request(es_bx); r.flags &= ~1; return true;
    default:
        FG_LOG("unsupported MSCDEX function %04xh", ax);
        return false;
    }
}

void CdRom::device_request(uint32_t req) {
    AddressSpace& as = g_machine->as;
    uint8_t cmd = *as.host(req + 2);
    uint16_t status = kDone;
    auto far_ptr = [&](uint32_t at) {
        uint16_t off = as.read<uint16_t>(at), seg = as.read<uint16_t>(at + 2);
        return (uint32_t(seg) << 4) + off;
    };
    switch (cmd) {
    case 0x03: status = ioctl_input(far_ptr(req + 0x0E)); break;
    case 0x0C: status = ioctl_output(far_ptr(req + 0x0E)); break;
    case 0x0D: case 0x0E: break;  // device open/close
    case 0x84: {                  // play audio
        uint8_t mode = *as.host(req + 0x0D);
        uint32_t start = as.read<uint32_t>(req + 0x0E), count = as.read<uint32_t>(req + 0x12);
        play_start_lba_ = mode == 1 ? from_redbook(start) : start;
        play_sectors_ = count;
        play_started_ = mono_seconds();
        paused_at_ = -1;
        FG_LOG("CD play: LBA %u, %u sectors (track %d)", play_start_lba_, count, playing_track());
        g_cd_audio.play(play_start_lba_, count);
        break;
    }
    case 0x85:  // stop: first call pauses, a second one resets
        if (is_playing()) { paused_at_ = mono_seconds(); g_cd_audio.pause(); }
        else { play_sectors_ = 0; paused_at_ = -1; g_cd_audio.stop(); }
        break;
    case 0x88:  // resume
        if (paused_at_ >= 0) {
            play_started_ += mono_seconds() - paused_at_;
            paused_at_ = -1;
            g_cd_audio.resume();
        }
        break;
    default:
        FG_LOG("unsupported CD device request %02xh", cmd);
        status = kDone | kError | kErrUnknownCommand;
    }
    if (is_playing()) status |= kBusy;
    as.write<uint16_t>(req + 3, status);
}

uint16_t CdRom::ioctl_input(uint32_t b) {
    AddressSpace& as = g_machine->as;
    uint8_t* p = as.host(b);
    switch (p[0]) {
    case 0x00: as.write<uint32_t>(b + 1, 0xF0000000); break;  // device header address (fake)
    case 0x01: {                                               // location of head
        uint32_t pos = now_lba();
        as.write<uint32_t>(b + 2, p[1] == 1 ? to_redbook(pos) : pos);
        break;
    }
    case 0x04:  // audio channel info: inputs 0-3 to outputs 0-3, full volume
        for (int i = 0; i < 4; ++i) { p[1 + 2 * i] = static_cast<uint8_t>(i); p[2 + 2 * i] = 0xFF; }
        break;
    case 0x06: as.write<uint32_t>(b + 1, 0x00000210); break;   // device status: audio, raw+cooked, door closed
    case 0x07: as.write<uint16_t>(b + 2, 2048); break;          // sector size
    case 0x08: as.write<uint32_t>(b + 1, leadout_lba_); break;  // volume size
    case 0x09: p[1] = 1; break;                                 // media not changed
    case 0x0A:                                                  // audio disk info
        p[1] = 1;
        p[2] = static_cast<uint8_t>(tracks_.size());
        as.write<uint32_t>(b + 3, to_redbook(leadout_lba_));
        break;
    case 0x0B: {  // audio track info
        int n = p[1];
        if (n < 1 || n > int(tracks_.size())) return kDone | kError | 0x08;  // sector not found
        const CdTrack& t = tracks_[n - 1];
        as.write<uint32_t>(b + 2, to_redbook(t.start_lba));
        p[6] = t.audio ? 0x00 : 0x40;  // control: data track bit
        break;
    }
    case 0x0C: {  // audio Q-channel info
        uint32_t pos = now_lba();
        int track = 1;
        for (const auto& t : tracks_) if (pos >= t.start_lba) track = t.number;
        const CdTrack& t = tracks_[track - 1];
        uint32_t rel = pos - t.start_lba;
        p[1] = t.audio ? 0x01 : 0x41;
        p[2] = static_cast<uint8_t>(((track / 10) << 4) | (track % 10));  // BCD
        p[3] = 1;
        p[4] = static_cast<uint8_t>(rel / 75 / 60); p[5] = static_cast<uint8_t>(rel / 75 % 60); p[6] = static_cast<uint8_t>(rel % 75);
        p[7] = 0;
        uint32_t a = pos + 150;
        p[8] = static_cast<uint8_t>(a / 75 / 60); p[9] = static_cast<uint8_t>(a / 75 % 60); p[10] = static_cast<uint8_t>(a % 75);
        break;
    }
    case 0x0F: {  // audio status info
        as.write<uint16_t>(b + 1, paused_at_ >= 0 ? 1 : 0);
        as.write<uint32_t>(b + 3, to_redbook(play_start_lba_));
        as.write<uint32_t>(b + 7, to_redbook(play_start_lba_ + play_sectors_));
        break;
    }
    default:
        FG_LOG("unsupported CD IOCTL input %02xh", p[0]);
        return kDone | kError | kErrUnknownCommand;
    }
    return kDone;
}

uint16_t CdRom::ioctl_output(uint32_t b) {
    uint8_t code = *g_machine->as.host(b);
    switch (code) {
    case 0x00: case 0x01: case 0x02: case 0x05: return kDone;  // eject, lock, reset, close tray
    case 0x03: {                                                // audio channel control
        const uint8_t* p = g_machine->as.host(b);
        g_cd_audio.set_volume(std::max(p[2], p[4]) / 255.0f);    // output channels 0/1 volume
        return kDone;
    }
    default:
        FG_LOG("unsupported CD IOCTL output %02xh", code);
        return kDone | kError | kErrUnknownCommand;
    }
}

}  // namespace fg
