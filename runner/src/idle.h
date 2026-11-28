// SPDX-License-Identifier: GPL-2.0-or-later
// Idle detection for the game's busy-wait loops.
//
// DOS games wait by spinning: polling the keyboard (int 21h 0Bh), the PIT
// counter or the VGA retrace bit until something changes. Each poll is a
// trapped instruction, so an idle game keeps a host core fully busy.
// A poll site that is hit again within microseconds of its last empty poll,
// many times in a row, is a loop doing nothing else; the runner then sleeps
// briefly before answering. Loops that do real work between polls are slower
// than the threshold and never sleep.
//
// Sleeping in a trap handler delays timer interrupts (the signal is blocked
// there) by at most the sleep time, and input keeps arriving on the UI thread.
#pragma once
#include <time.h>

#include <cstdint>

namespace fg {

inline uint64_t idle_now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

inline void idle_sleep_ns(uint64_t ns) {
    timespec ts{static_cast<time_t>(ns / 1000000000ull), static_cast<long>(ns % 1000000000ull)};
    nanosleep(&ts, nullptr);
}

class IdleDetector {
public:
    // Record an empty poll; true once the caller has been spinning.
    bool spinning() {
        uint64_t now = idle_now_ns();
        streak_ = now - last_ns_ < kMaxGapNs ? streak_ + 1 : 0;
        last_ns_ = now;
        return streak_ >= kMinStreak;
    }
    // Sleep, then restart the gap measurement so the sleep is not counted.
    void sleep_ns(uint64_t ns) {
        idle_sleep_ns(ns);
        last_ns_ = idle_now_ns();
    }
    // A non-empty poll ends the spin.
    void reset() { streak_ = 0; }

private:
    static constexpr uint64_t kMaxGapNs = 50000;  // 50 us between polls
    static constexpr int kMinStreak = 100;
    uint64_t last_ns_ = 0;
    int streak_ = 0;
};

}  // namespace fg
