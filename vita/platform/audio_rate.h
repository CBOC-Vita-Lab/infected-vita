// audio_rate.h - keeps the audio device and the game's audio producers on the same clock.
//
// In pull mode the 48 kHz hardware asks for frames at its own rate while the game's audio threads write them at the rate of the
// kernel clock they sleep on. The two crystals differ, so the queue between them (the AudioSink ring) either creeps up until
// the ring overflows and a stretch is thrown away, or creeps down until a producer is found behind the device and restarted,
// which is a click and a gap. A slow change of the resampler's step closes the loop: when the queue is longer than its target the
// device is made to consume a little faster, when shorter a little slower. The change is a few parts per thousand at most
// (a few cents of pitch), far below what anyone can hear, and it follows the drift, which moves over minutes.
// Header only and free of platform calls: the Vita pump (platform/vita/audio_vita.cpp) and the host test
// (selftest/t_audio_rate_host.cpp) run the same code.
#pragma once

#include <stdint.h>

namespace plat {

// A PI loop. The proportional part reacts to the queue now; the integral part learns the difference between the two clocks
// (constant for the whole session, so it is kept through silences and pauses).
struct RateControl {
    static constexpr float kTargetFrames = 3072.0f;   // the lead the loop settles at: the sink's prebuffer, where a restarted producer starts
    static constexpr float kAlpha = 1.0f / 48.0f;     // averaging of the queue length per period (21 ms): about 1 s
    static constexpr float kKp = 4.5f;                // ppm per frame of error, at once
    static constexpr float kKi = 0.015f;              // ppm per frame of error, added every period (0.7 ppm/frame/s): settles in ~10 s
    static constexpr float kMaxError = 1000.0f;       // frames: a stall or a restart is a jolt, not a measurement of the clocks
    static constexpr float kMaxPpm = 6000.0f;         // 0.6 %: 10 cents of pitch, far more than two crystals differ by

    float average = kTargetFrames;
    float integral = 0.0f;   // the learned clock difference, ppm
    float ppm = 0.0f;        // the correction in use: positive = consume faster
    bool primed = false;

    // Once per output period. `queued`: the least lead (frames ahead of the device, before its own chunk) of the producers that wrote
    // lately, or -1 when none did (the game is silent or paused: nothing to measure, so only the learned clock difference stays in
    // force).
    // Returns the step to use.
    uint32_t update(uint32_t nominal_step, int64_t queued) {
        if (queued < 0) {
            primed = false;
            ppm = integral;
        } else {
            if (!primed) {
                average = (float)queued;
                primed = true;
            } else {
                average += ((float)queued - average) * kAlpha;
            }
            float error = average - kTargetFrames;
            if (error > kMaxError) error = kMaxError;
            if (error < -kMaxError) error = -kMaxError;
            integral += error * kKi;
            if (integral > kMaxPpm) integral = kMaxPpm;
            if (integral < -kMaxPpm) integral = -kMaxPpm;
            ppm = error * kKp + integral;
            if (ppm > kMaxPpm) ppm = kMaxPpm;
            if (ppm < -kMaxPpm) ppm = -kMaxPpm;
        }
        return (uint32_t)((double)nominal_step * (1.0 + (double)ppm * 1e-6));
    }
};

}  // namespace plat
