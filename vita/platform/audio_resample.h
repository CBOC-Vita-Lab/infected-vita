// audio_resample.h - the pump's sample-rate conversion core: 4-point cubic (Catmull-Rom) interpolation of interleaved stereo s16.
// Header only and free of platform calls, so the Vita pump (platform/vita/audio_vita.cpp) and the host test (selftest/t_resample_host.cpp)
// run the same code. It replaced a 2-point linear interpolator, which folds the top octave of a 44.1 kHz stream back into the audible
// band as a thin hiss (about -25 dB at 20 kHz) and rolls off 10 kHz by 1.5 dB.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace plat {

inline int16_t resample_clip16(float v) {
    v += v >= 0.0f ? 0.5f : -0.5f;
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)v;
}

// Source frames s[0 .. n): s[0] is the frame BEFORE the first one that output positions are measured from (the history frame the
// cubic needs on the left). `pos` is a 16.16 position measured from s[1]; each output frame advances it by `step` (source frames per
// output frame, 16.16). Writes up to `max_out` frames and stops early when the next one would need a frame at or past s[n]
// (it needs s[idx] .. s[idx + 3] with idx = pos >> 16). Returns the number of frames written; `pos` is left at the next one.
inline size_t resample_cubic(const int16_t* s, uint32_t n, uint32_t& pos, uint32_t step, int16_t* out, size_t max_out) {
    size_t i = 0;
    for (; i < max_out; i++) {
        const uint32_t idx = pos >> 16;
        if (idx + 4 > n) break;
        const float f = (float)(pos & 0xFFFFu) * (1.0f / 65536.0f);
        const int16_t* p = s + 2 * (size_t)idx;
        for (int c = 0; c < 2; c++) {
            const float p0 = p[c], p1 = p[2 + c], p2 = p[4 + c], p3 = p[6 + c];
            const float a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
            const float b = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
            const float d = 0.5f * (p2 - p0);
            out[2 * i + (size_t)c] = resample_clip16(((a * f + b) * f + d) * f + p1);
        }
        pos += step;
    }
    return i;
}

// Source frames one call of resample_cubic for `frames` output frames from position `pos` may touch (the staging buffer must hold
// at least this many frames, history frame included).
inline uint32_t resample_cubic_need(uint32_t pos, uint32_t step, uint32_t frames) {
    return (uint32_t)((((uint64_t)pos + (uint64_t)step * (frames ? frames - 1 : 0)) >> 16) + 4);
}

}  // namespace plat
