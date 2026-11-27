// SPDX-License-Identifier: GPL-2.0-or-later
// Locking for state shared between the game thread and SDL's audio thread.
//
// SDL3 holds an audio stream's own (recursive) lock while it runs the
// stream's get-callback, so that lock is the only one used for the state the
// callback touches. A separate mutex would deadlock: callback (stream lock ->
// our mutex) against game thread (our mutex -> SDL_SetAudioStreamFormat ->
// stream lock).
#pragma once
#include <SDL3/SDL.h>

namespace fg {

class StreamLock {
public:
    explicit StreamLock(SDL_AudioStream* s) : s_(s) { if (s_) SDL_LockAudioStream(s_); }
    ~StreamLock() { if (s_) SDL_UnlockAudioStream(s_); }
    StreamLock(const StreamLock&) = delete;
    StreamLock& operator=(const StreamLock&) = delete;

private:
    SDL_AudioStream* s_;
};

}  // namespace fg
