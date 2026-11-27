// SPDX-License-Identifier: GPL-2.0-or-later
#include "cd_audio.h"

#include <algorithm>

#include "audio_lock.h"
#include "log.h"

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

namespace fg {

CdAudio g_cd_audio;

bool CdAudio::init(const std::vector<CdTrack>& tracks) {
    tracks_ = tracks;  // before the stream exists, so no callback can run yet
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, 44100};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &CdAudio::feed, this);
    if (!stream_) {
        FG_LOG("cannot open audio device: %s", SDL_GetError());
        return false;
    }
    SDL_ResumeAudioStreamDevice(stream_);
    FG_LOG("audio: CD tracks play through %s", SDL_GetCurrentAudioDriver());
    return true;
}

void CdAudio::shutdown() {
    if (stream_) SDL_DestroyAudioStream(stream_);  // stops the callback first
    stream_ = nullptr;
    close_track();
}

void CdAudio::close_track() {
    if (vorbis_) stb_vorbis_close(vorbis_);
    vorbis_ = nullptr;
}

bool CdAudio::open_track(size_t index, uint32_t frame_offset) {
    close_track();
    track_ = index;
    const CdTrack& t = tracks_[index];
    int err = 0;
    vorbis_ = stb_vorbis_open_filename(t.file.c_str(), &err, nullptr);
    if (!vorbis_) {
        FG_LOG("cannot decode %s (stb_vorbis error %d)", t.file.c_str(), err);
        return false;
    }
    stb_vorbis_info info = stb_vorbis_get_info(vorbis_);
    channels_ = info.channels;
    SDL_AudioSpec spec{SDL_AUDIO_F32, info.channels, static_cast<int>(info.sample_rate)};
    SDL_SetAudioStreamFormat(stream_, &spec, nullptr);
    // Offsets are in CD frames (44.1 kHz); convert for other rates.
    uint32_t sample = static_cast<uint32_t>(uint64_t(frame_offset) * info.sample_rate / 44100);
    if (sample) stb_vorbis_seek(vorbis_, sample);
    return true;
}

void CdAudio::play(uint32_t start_lba, uint32_t sectors) {
    if (!stream_) return;
    StreamLock lock(stream_);
    SDL_ClearAudioStream(stream_);
    close_track();
    paused_ = false;
    frames_left_ = 0;
    for (size_t i = 0; i < tracks_.size(); ++i) {
        const CdTrack& t = tracks_[i];
        if (!t.audio || start_lba >= t.start_lba + t.sectors) continue;
        // A start inside the pregap begins at the track start.
        uint32_t skip = start_lba > t.start_lba ? start_lba - t.start_lba : 0;
        uint32_t lost = t.start_lba > start_lba ? t.start_lba - start_lba : 0;
        if (lost >= sectors) return;
        if (open_track(i, skip * kFramesPerSector)) frames_left_ = uint64_t(sectors - lost) * kFramesPerSector;
        return;
    }
}

void CdAudio::pause() { StreamLock lock(stream_); paused_ = true; }
void CdAudio::resume() { StreamLock lock(stream_); paused_ = false; }

void CdAudio::stop() {
    StreamLock lock(stream_);
    if (stream_) SDL_ClearAudioStream(stream_);
    close_track();
    frames_left_ = 0;
    paused_ = false;
}

void CdAudio::set_volume(float v) {
    StreamLock lock(stream_);
    volume_ = std::clamp(v, 0.0f, 1.0f);
}

void SDLCALL CdAudio::feed(void* self, SDL_AudioStream* stream, int additional, int /*total*/) {
    // SDL holds the stream lock here (see audio_lock.h).
    static_cast<CdAudio*>(self)->feed_locked(stream, additional);
}

void CdAudio::feed_locked(SDL_AudioStream* stream, int bytes) {
    // `bytes` is in the stream's current input format (float, channels_).
    while (bytes > 0) {
        if (paused_ || !vorbis_ || frames_left_ == 0) return;  // SDL plays silence for missing data
        int frames = std::min<int>(bytes / int(sizeof(float) * channels_), 4096);
        frames = static_cast<int>(std::min<uint64_t>(std::max(frames, 256), frames_left_));
        buf_.resize(size_t(frames) * channels_);
        int got = stb_vorbis_get_samples_float_interleaved(vorbis_, channels_, buf_.data(), frames * channels_);
        if (got <= 0) {
            // End of this track: a play range can continue into the next audio track.
            size_t next = track_ + 1;
            if (next < tracks_.size() && tracks_[next].audio) {
                uint64_t pregap = (tracks_[next].start_lba - (tracks_[track_].start_lba + tracks_[track_].sectors)) *
                                  uint64_t(kFramesPerSector);
                frames_left_ = frames_left_ > pregap ? frames_left_ - pregap : 0;
                if (frames_left_ && open_track(next, 0)) continue;
            }
            close_track();
            frames_left_ = 0;
            return;
        }
        if (volume_ != 1.0f)
            for (size_t i = 0; i < size_t(got) * channels_; ++i) buf_[i] *= volume_;
        SDL_PutAudioStreamData(stream, buf_.data(), got * channels_ * int(sizeof(float)));
        frames_left_ -= uint64_t(got);
        bytes -= got * channels_ * int(sizeof(float));
    }
}

}  // namespace fg
