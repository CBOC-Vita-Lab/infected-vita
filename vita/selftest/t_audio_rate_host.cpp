// Host test of the audio clock follower (platform/audio_rate.h) against a model of the pull path: a producer that writes
// 1024-frame bursts on its own clock (a fraction faster or slower than the device, with timing jitter and the odd stall) into a
// ring with the AudioSink's rules (a cursor found behind read_position + prebuffer/4 is restarted at read_position + prebuffer;
// a write past the ring's end throws the oldest frames away), and the pump taking 1024 output frames per period at 48 kHz.
// Build and run on any machine:
//   g++ -O2 -std=c++17 -I../platform t_audio_rate_host.cpp -o t_audio_rate_host && ./t_audio_rate_host
// Exit status 0 when, for clock differences up to +-0.4 % over ten minutes without stalls, the follower has no restarts and no
// overflows once settled, and the fixed step does restart or overflow from +-0.1 % on (so the test can tell the two apart). With the
// 60 ms stalls (a producer that is not scheduled for four frames) the table is informational: a stall longer than the queue is a
// gap whatever the step is, and the table shows the follower does not make it worse by more than the fixed step's own losses.
// Run with an argument ("nostall") to print the table without the stalls.
#include "audio_rate.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

struct Out { long restarts; long overflows; double min_fill; double max_fill; double max_ppm; double final_ppm; };

static Out run(double drift, bool follow, double minutes, bool stalls) {
    const double kDevRate = 48000.0, kSrcRate = 44100.0;
    const uint32_t grain = 1024;
    const uint64_t kRing = 8192, kPre = 3072;
    uint32_t step0 = (uint32_t)(((uint64_t)44100 << 16) / 48000);
    plat::RateControl rc;
    srand(7);
    double t = 0;                          // seconds, the device's clock
    const double end = minutes * 60.0;
    uint64_t read_pos = 0, cursor = 0, write_end = 0;
    uint32_t phase = 0;
    // producer: bursts of 1024 frames, one every 1024/44100 s of ITS clock (which runs at 1 + drift of the device's)
    double ideal = 0.05;
    double next_write = ideal;
    Out o{0, 0, 1e9, 0, 0, 0};
    uint32_t step = step0;
    double last_write_t = 0;
    int64_t last_lead = -1;
    while (t < end) {
        // the device's period: pull what the resampler needs
        const uint32_t need = (uint32_t)(((uint64_t)phase + (uint64_t)step * (grain - 1)) >> 16) + 1;
        (void)need;
        uint64_t adv = ((uint64_t)phase + (uint64_t)step * grain) >> 16;
        phase = (uint32_t)(((uint64_t)phase + (uint64_t)step * grain) & 0xFFFF);
        read_pos += adv;
        t += grain / kDevRate;
        // producers writing up to now: bursts on the producer's ideal grid, each late by a random delay (the guest thread's
        // scheduling), an occasional 60 ms stall after which it catches up
        while (next_write <= t) {
            if (cursor < read_pos + kPre / 4) {
                if (cursor != 0 && t > 20.0) o.restarts++;
                cursor = read_pos + kPre;
            }
            if (cursor + 1024 > read_pos + kRing) { if (t > 20.0) o.overflows++; cursor = read_pos + kRing - 1024; }
            last_lead = (int64_t)(cursor - read_pos);   // the producer's lead before its own chunk: what the sink reports
            cursor += 1024;
            if (cursor > write_end) write_end = cursor;
            last_write_t = t;
            ideal += 1024.0 / kSrcRate / (1.0 + drift);
            double late = ((double)rand() / RAND_MAX) * 0.012;                       // 0..12 ms
            if (stalls && (rand() % 3000) == 0) late += 0.06;
            next_write = ideal + late;
        }
        int64_t queued = write_end > read_pos ? (int64_t)(write_end - read_pos) : 0;
        if (follow) step = rc.update(step0, (t - last_write_t) < 0.4 ? last_lead : -1);
        if (t > 20.0) {
            if ((double)queued < o.min_fill) o.min_fill = (double)queued;
            if ((double)queued > o.max_fill) o.max_fill = (double)queued;
            if (fabs(rc.ppm) > o.max_ppm) o.max_ppm = fabs(rc.ppm);
        }
    }
    o.final_ppm = rc.ppm;
    return o;
}

int main(int argc, char** argv) {
    int bad = 0;
    const bool stalls = argc < 2;
    const double drifts[] = {-0.004, -0.001, 0.0, 0.001, 0.004};
    printf("%9s | %-52s | %-40s\n", "drift", stalls ? "following (with stalls): restarts overflows fill min..max  |ppm| max" : "following: restarts overflows fill min..max  |ppm| max", "fixed step: restarts overflows");
    for (double d : drifts) {
        Out a = run(d, true, 10.0, stalls);
        Out b = run(d, false, 10.0, stalls);
        Out c = run(d, true, 10.0, false);   // the same without the 60 ms stalls: nothing but the clocks can cost the queue
        printf("%8.2f%% | %8ld %9ld %12.0f..%-6.0f %8.0f        | %8ld %9ld\n", d * 100.0, a.restarts, a.overflows, a.min_fill, a.max_fill,
               a.max_ppm, b.restarts, b.overflows);
        if (c.restarts != 0 || c.overflows != 0) { printf("  FAIL: the follower lost the queue at %.2f %% (%ld restarts, %ld overflows, no stalls)\n", d * 100.0, c.restarts, c.overflows); bad++; }
        if (fabs(d) >= 0.001 && b.restarts == 0 && b.overflows == 0) { printf("  FAIL: the fixed step coped at %.2f %% (model too easy)\n", d * 100.0); bad++; }
        if (a.max_ppm > 6000.5) { printf("  FAIL: correction beyond the clamp\n"); bad++; }
    }
    // a silent game keeps the learned clock difference and drops the rest
    plat::RateControl rc;
    for (int i = 0; i < 3000; i++) rc.update(1000, 4600);   // a queue 1000 frames long: the integral keeps climbing
    const float learned = rc.integral;
    if (learned <= 0) { printf("FAIL: no correction learned for a long queue\n"); bad++; }
    rc.update(1000, -1);
    if (fabsf(rc.ppm - learned) > 0.01f) { printf("FAIL: silence changed the learned correction (%.2f vs %.2f ppm)\n", rc.ppm, learned); bad++; }
    printf(bad ? "FAILED (%d)\n" : "OK\n", bad);
    return bad ? 1 : 0;
}
