// retry.h - small pure helpers the Vita platform layer shares with its host tests: how often to try a probe, and when a periodic
// call is due. No platform calls.
#pragma once

#include <stdint.h>

namespace plat {

// Runs probe() up to `attempts` times, calling pause() between tries (not after the last). Stops at the first success.
// Returns the number of the successful try (1-based), or 0 when every try failed. `tried` (optional) gets the number made.
// probe: bool(int attempt_number); pause: void(int attempt_number_just_failed).
template <class Probe, class Pause>
int retry_probe(int attempts, Probe probe, Pause pause, int* tried = nullptr) {
    int n = 0;
    for (int i = 1; i <= attempts; i++) {
        n = i;
        if (probe(i)) {
            if (tried) *tried = n;
            return i;
        }
        if (i < attempts) pause(i);
    }
    if (tried) *tried = n;
    return 0;
}

// True once per `period_us` of the clock it is given (the first call is always due).
struct Throttle {
    uint64_t last_us = 0;
    bool started = false;
    bool due(uint64_t now_us, uint64_t period_us) {
        if (started && now_us - last_us < period_us) return false;
        started = true;
        last_us = now_us;
        return true;
    }
};

// A late period of a helper thread: a gap between two completed periods longer than `limit` times the period. A gap longer than
// `suspend_us` is a sleep of the console, not a late thread.
inline bool period_is_late(uint64_t gap_us, uint64_t period_us, uint32_t limit, uint64_t suspend_us) {
    return gap_us > period_us * limit && gap_us < suspend_us;
}

// Counts late periods in a row; `demote` is true when `limit` of them came one after another. An on-time period starts the count over.
struct LateCounter {
    int in_a_row = 0;
    uint64_t total = 0;
    bool note(bool late, int limit) {
        if (!late) { in_a_row = 0; return false; }
        total++;
        in_a_row++;
        return in_a_row >= limit;
    }
};

}  // namespace plat
