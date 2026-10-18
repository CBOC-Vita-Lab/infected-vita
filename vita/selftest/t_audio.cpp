// T5 - audio: 2 s of a 440 Hz sine, stereo s16 at 44100 Hz through the platform audio pump. The producer is this thread
// (like the guest audio thread would be) writing small chunks on a timer. Reports underruns, pump timing, output errors.
#include "st_common.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace st {

namespace {
struct Run {
    const char* label;
    plat::AudioParams::Port port;
};

void run_one(const Run& run, double seconds) {
    plat::AudioParams ap;
    ap.sample_rate = 44100;
    ap.ring_frames = 16384;
    ap.period_frames = 1024;
    ap.port = run.port;
    ap.core = 2;
    ap.priority = -16;
    uint64_t t_open0 = plat::now_us();
    bool ok = plat::audio_open(ap);
    plat::AudioStats s0 = plat::audio_stats();
    if (!ok) {
        // opening BGM at 44.1 kHz may legitimately fail; MAIN failing is a real problem
        if (run.port == plat::AudioParams::Bgm44k)
            limit("T5", run.label, "audio_open failed (rc 0x%08X) - the BGM port at 44100 Hz is not available here; the port will use MAIN with resampling", (unsigned)s0.open_rc);
        else
            fail("T5", run.label, "audio_open failed (rc 0x%08X)", (unsigned)s0.open_rc);
        return;
    }
    info("T5", run.label, "opened in %llu us: port type %d, hardware rate %d Hz, grain %d frames, resampling %s, open rc 0x%08X", (unsigned long long)(plat::now_us() - t_open0),
         s0.port_type, s0.port_rate, s0.port_grain, s0.resampling ? "yes (44.1k -> hw)" : "no", (unsigned)s0.open_rc);
    status("T5 audio: you should hear a 440 Hz tone (%s)", run.label);

    const double total_frames_d = seconds * 44100.0;
    const uint64_t total_frames = (uint64_t)total_frames_d;
    uint64_t produced = 0;
    double phase = 0.0;
    const double step = 2.0 * 3.14159265358979323846 * 440.0 / 44100.0;
    int16_t chunk[2 * 441];   // 10 ms
    uint64_t start = plat::now_us();
    uint64_t warm_underruns = 0;
    bool warm_taken = false;
    uint64_t max_ring = 0;
    while (produced < total_frames) {
        // keep the ring about 150 ms ahead; produce one 10 ms chunk when there is room
        size_t queued = plat::audio_queued_frames();
        if (queued > max_ring) max_ring = queued;
        if (queued < 6615 && plat::audio_free_frames() >= 441) {
            size_t n = 441;
            if (produced + n > total_frames) n = (size_t)(total_frames - produced);
            for (size_t i = 0; i < n; i++) {
                int16_t v = (int16_t)(sinf((float)phase) * 9000.0f);
                phase += step;
                if (phase > 6.283185307179586) phase -= 6.283185307179586;
                chunk[2 * i] = v;
                chunk[2 * i + 1] = v;
            }
            size_t w = plat::audio_write(chunk, n);
            produced += w;
            if (w != n) { info("T5", run.label, "short write %zu of %zu", w, n); }
        } else {
            plat::sleep_us(2000);
        }
        if (!warm_taken && plat::now_us() - start > 150000) {
            warm_underruns = plat::audio_stats().underruns;   // startup underruns (pump started before the first chunks) are not counted
            warm_taken = true;
        }
        poll_abort("T5");
        if (plat::now_us() - start > (uint64_t)(seconds * 3.0e6) + 3000000) break;
    }
    // underruns are judged when production stops: the final partial period while the ring drains is silence-padded by design
    plat::AudioStats s_prod = plat::audio_stats();
    // let the ring drain
    uint64_t drain0 = plat::now_us();
    while (plat::audio_queued_frames() > 0 && plat::now_us() - drain0 < 1500000) plat::sleep_us(5000);
    uint64_t elapsed = plat::now_us() - start;
    plat::AudioStats s = plat::audio_stats();
    plat::audio_close();

    double out_rate = elapsed ? (double)s.frames_out * 1e6 / (double)elapsed : 0;
    uint64_t steady_underruns = s_prod.underruns - warm_underruns;
    bool rate_ok = s.frames_out > 0 && fabs(out_rate - 44100.0 * seconds / (seconds + 0.05)) < 44100.0 * 0.20;   // loose: includes the drain wait
    bool good = produced >= total_frames && steady_underruns == 0 && s.output_errors == 0;
    check(good, "T5", run.label,
          "%.1f s of 440 Hz: produced %llu frames, consumed %llu, periods out %llu, underruns %llu (%llu after the 150 ms warm-up, %llu silence frames), overruns %llu, output errors %llu, longest sceAudioOutOutput %u us, peak ring fill %llu frames, elapsed %llu ms (%.0f frames/s average out)%s",
          seconds, (unsigned long long)produced, (unsigned long long)s.frames_out, (unsigned long long)s.periods_out, (unsigned long long)s.underruns, (unsigned long long)steady_underruns,
          (unsigned long long)s.underrun_frames, (unsigned long long)s.overruns, (unsigned long long)s.output_errors, (unsigned)s.max_output_us, (unsigned long long)max_ring,
          (unsigned long long)(elapsed / 1000), out_rate, rate_ok ? "" : " [consumption rate looks off]");
}
}  // namespace

void t5_audio() {
    const double seconds = cfg_int("t5.ms", 2000) / 1000.0;
    if (cfg_int("t5.main", 1)) run_one({"audio-main", plat::AudioParams::Main48k}, seconds);
    if (cfg_int("t5.bgm", 1)) run_one({"audio-bgm44k", plat::AudioParams::Bgm44k}, seconds);
    plat::sleep_us(100000);
}

}  // namespace st
