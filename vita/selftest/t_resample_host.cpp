// Host test of the pump's resampler (platform/audio_resample.h): a sine at several frequencies through the same staging logic
// the Vita pump uses (1024-frame periods, a history frame, 16.16 position), compared with the ideal sine at the output instants,
// next to the old 2-point linear interpolator. Build and run on any machine:
//   g++ -O2 -std=c++17 -I../platform t_resample_host.cpp -o t_resample_host -lm && ./t_resample_host
// Exit status 0 when the cubic's error is below -80 dB at 1 kHz and below -40 dB at 5 kHz, is better than the linear's by at least
// 10 dB up to 5 kHz (and never worse), and the stream is continuous across period boundaries (no step at a boundary larger than the
// signal's own slope allows). Measured 16 Oct 2026: 440 Hz -86.8 dB (linear -68.7), 1 kHz -84.8 (-54.6), 5 kHz -45.0 (-26.8),
// 10 kHz -24.1 (-15.0).
#include "audio_resample.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

static const double kPi = 3.14159265358979323846;

struct Result { double rms_err_db; double max_boundary_jump; double max_err; };

// mode 0: cubic through the pump's staging code, 1: the old linear code
static Result run(double freq, int mode, int periods = 300) {
    const uint32_t grain = 1024;
    const uint32_t step = (uint32_t)(((uint64_t)44100 << 16) / 48000);
    const double amp = 12000.0;
    std::vector<int16_t> stg(2 * (grain * 2 + 16), 0);
    uint32_t stg_n = 1;      // frame 0 is the history frame (silent)
    uint32_t phase = 0;
    uint64_t next_src = 0;   // index of the next source sample to deliver
    auto sample = [&](uint64_t i) { return (int16_t)lrint(sin(2.0 * kPi * freq * (double)i / 44100.0) * amp); };
    uint64_t out_index = 0;
    double sum_sq = 0, max_err = 0, max_jump = 0;
    long counted = 0;
    int16_t out[2 * 1024];
    int16_t last_l = 0;
    bool have_last = false;
    for (int per = 0; per < periods; per++) {
        uint32_t need = plat::resample_cubic_need(phase, step, grain);   // the same staging for both, so only the interpolator differs
        if (stg_n < need) {
            uint32_t take = need - stg_n;
            for (uint32_t k = 0; k < take; k++) {
                int16_t v = sample(next_src++);
                stg[2 * (stg_n + k)] = v;
                stg[2 * (stg_n + k) + 1] = v;
            }
            stg_n += take;
        }
        uint32_t pos = phase;
        size_t got = 0;
        if (mode == 0) {
            got = plat::resample_cubic(stg.data(), stg_n, pos, step, out, grain);
        } else {
            for (; got < grain; got++) {   // the replaced code, with the same 'frame 0 is history' layout (so p1 = frame idx + 1)
                uint32_t idx = (pos >> 16) + 1;
                if (idx + 1 >= stg_n) break;
                int32_t f = (int32_t)(pos & 0xFFFF);
                const int16_t* a = stg.data() + 2 * idx;
                const int16_t* b = a + 2;
                out[2 * got] = (int16_t)(a[0] + (((int32_t)(b[0] - a[0]) * f) >> 16));
                out[2 * got + 1] = out[2 * got];
                pos += step;
            }
        }
        if (got != grain) { printf("  short period %zu of %u (mode %d, f=%.0f)\n", got, grain, mode, freq); exit(2); }
        uint32_t consumed = pos >> 16;
        memmove(stg.data(), stg.data() + 2 * consumed, (size_t)(stg_n - consumed) * 4);
        stg_n -= consumed;
        phase = pos & 0xFFFF;
        for (size_t i = 0; i < got; i++, out_index++) {
            // output frame k sits at source position k * step / 65536 from source sample 0
            double t = (double)out_index * (double)step / 65536.0;
            double ideal = sin(2.0 * kPi * freq * t / 44100.0) * amp;
            double e = (double)out[2 * i] - ideal;
            if (per >= 2) {
                sum_sq += e * e;
                counted++;
                if (fabs(e) > max_err) max_err = fabs(e);
            }
            if (have_last && i == 0 && per >= 2) {
                double j = fabs((double)out[0] - (double)last_l);
                if (j > max_jump) max_jump = j;
            }
            last_l = out[2 * i];
            have_last = true;
        }
    }
    Result r;
    double rms = sqrt(sum_sq / (double)counted);
    r.rms_err_db = 20.0 * log10((rms + 1e-9) / (amp / sqrt(2.0)));
    r.max_boundary_jump = max_jump;
    r.max_err = max_err;
    return r;
}

int main() {
    const double freqs[] = {440.0, 1000.0, 5000.0, 10000.0, 15000.0, 18000.0};
    printf("%8s | %-34s | %-34s\n", "freq Hz", "cubic: error vs ideal (dB, max)", "linear: error vs ideal (dB, max)");
    int bad = 0;
    for (double f : freqs) {
        Result c = run(f, 0), l = run(f, 1);
        printf("%8.0f | %7.1f dB  max %7.1f LSB       | %7.1f dB  max %7.1f LSB\n", f, c.rms_err_db, c.max_err, l.rms_err_db, l.max_err);
        if (f <= 1000.0 && c.rms_err_db > -80.0) { printf("  FAIL: cubic error too large at %.0f Hz\n", f); bad++; }
        if (f <= 5000.0 && c.rms_err_db > -40.0) { printf("  FAIL: cubic error too large at %.0f Hz\n", f); bad++; }
        if (f <= 5000.0 && c.rms_err_db > l.rms_err_db - 10.0) { printf("  FAIL: cubic is not 10 dB better than linear at %.0f Hz\n", f); bad++; }
        if (c.rms_err_db > l.rms_err_db + 0.01) { printf("  FAIL: cubic is not better than linear at %.0f Hz\n", f); bad++; }
        // a boundary step can never exceed the largest sample-to-sample change of the signal (2*pi*f/44100*amp) by much
        double slope = 2.0 * kPi * f / 48000.0 * 12000.0 * 1.2 + 4.0;
        if (c.max_boundary_jump > slope) { printf("  FAIL: step %.0f across a period boundary at %.0f Hz (limit %.0f)\n", c.max_boundary_jump, f, slope); bad++; }
    }
    printf(bad ? "FAILED (%d)\n" : "OK\n", bad);
    return bad ? 1 : 0;
}
