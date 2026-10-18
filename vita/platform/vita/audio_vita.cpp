// platform/vita/audio_vita.cpp - audio out: lock-free SPSC ring -> pump thread (core 2) -> sceAudioOutOutput.
// Producer: plat::audio_write (any one thread, wait-free). Consumer: the pump thread created by audio_open.
// SDK facts (psp2/audioout.h, FW 3.60 documentation): MAIN requires 48000 Hz; BGM/VOICE accept every SceAudioOutSampleRate
// (incl. 44100) except 44184; len must be a multiple of SCE_AUDIO_MIN_LEN (64) and <= SCE_AUDIO_MAX_LEN (65472).
#include "../platform.h"
#include "../audio_rate.h"
#include "../audio_resample.h"

#include <atomic>
#include <malloc.h>
#include <string.h>

#include <psp2/audioout.h>

namespace plat {
namespace {

struct Audio {
    bool open = false;
    int16_t* ring = nullptr;         // ring_frames * 2 samples
    uint32_t mask = 0;               // ring_frames - 1
    alignas(64) std::atomic<uint32_t> head{0};   // producer index in frames (free running)
    alignas(64) std::atomic<uint32_t> tail{0};   // consumer index in frames (free running)
    std::atomic<bool> run{false};
    std::atomic<bool> armed{false};  // set at the first audio_write; before that silence is not an underrun
    std::atomic<uint32_t> volume_q16{65536};
    int port = -1;
    int src_rate = 44100;
    int dst_rate = 48000;
    int grain = 1024;
    bool resample = false;
    Thread* th = nullptr;
    AudioFill fill = nullptr;        // pull mode: how far ahead of the device the producers are (for the rate follower)
    RateControl rate;                // follows it
    AudioSource source = nullptr;    // pull mode: the pump takes its source frames from here instead of the ring (set before audio_open)
    int16_t* out = nullptr;          // grain * 2
    int16_t* stg = nullptr;          // staging for the resampler (source frames; frame 0 is the history frame), capacity stg_cap
    uint32_t stg_cap = 0, stg_n = 0;
    uint32_t phase = 0;              // 16.16 position measured from stg frame 1
    // stats (single writer each; relaxed)
    std::atomic<uint64_t> periods{0}, frames_in{0}, frames_out{0}, underruns{0}, underrun_frames{0}, overruns{0}, out_errors{0};
    std::atomic<uint32_t> max_out_us{0};
    AudioStats st_static;            // port_rate/type/grain/open_rc/resampling filled at open
};
Audio g;

uint32_t ring_count() { return g.head.load(std::memory_order_acquire) - g.tail.load(std::memory_order_relaxed); }

// copy `n` frames from the ring (consumer side) without advancing tail
void ring_peek(int16_t* dst, uint32_t offset_frames, uint32_t n) {
    uint32_t t = g.tail.load(std::memory_order_relaxed) + offset_frames;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (t + i) & g.mask;
        dst[2 * i] = g.ring[2 * idx];
        dst[2 * i + 1] = g.ring[2 * idx + 1];
    }
}

void pump_main(void*) {
    while (g.run.load(std::memory_order_acquire)) {
        const uint32_t grain = (uint32_t)g.grain;
        uint32_t produced = 0;      // frames of valid audio in g.out
        uint32_t missing = 0;
        if (!g.resample) {
            if (g.source) {
                g.source(g.out, grain);
                produced = grain;
                g.frames_in.fetch_add(grain, std::memory_order_relaxed);
            } else {
                uint32_t avail = ring_count();
                produced = avail < grain ? avail : grain;
                ring_peek(g.out, 0, produced);
                g.tail.fetch_add(produced, std::memory_order_release);
                missing = grain - produced;
            }
        } else {
            uint32_t step = (uint32_t)(((uint64_t)g.src_rate << 16) / (uint32_t)g.dst_rate);   // source frames per output frame, 16.16
            if (g.fill) step = g.rate.update(step, g.fill());   // the game's clock and the device's differ by a few hundred ppm: follow
            const uint32_t need = resample_cubic_need(g.phase, step, grain);                           // source frames touched this period
            // top up the staging buffer: from the source callback (pull mode: it always delivers, silence where nothing was written)
            // or from the ring
            if (g.stg_n < need) {
                uint32_t take = need - g.stg_n;
                if (g.stg_n + take > g.stg_cap) take = g.stg_cap - g.stg_n;
                if (g.source) {
                    g.source(g.stg + 2 * g.stg_n, take);
                    g.frames_in.fetch_add(take, std::memory_order_relaxed);
                } else {
                    uint32_t avail = ring_count();
                    if (take > avail) take = avail;
                    ring_peek(g.stg + 2 * g.stg_n, 0, take);
                    g.tail.fetch_add(take, std::memory_order_release);
                }
                g.stg_n += take;
            }
            uint32_t pos = g.phase;
            const size_t i = resample_cubic(g.stg, g.stg_n, pos, step, g.out, grain);
            produced = (uint32_t)i;
            missing = grain - (uint32_t)i;
            uint32_t consumed = pos >> 16;   // the frame before the next position stays as the new history frame
            if (consumed > g.stg_n) consumed = g.stg_n;
            if (consumed) {
                memmove(g.stg, g.stg + 2 * consumed, (size_t)(g.stg_n - consumed) * 4);
                g.stg_n -= consumed;
            }
            g.phase = pos & 0xFFFF;
            // "frames_out" counts source frames
            g.frames_out.fetch_add(consumed, std::memory_order_relaxed);
        }
        if (!g.resample) g.frames_out.fetch_add(produced, std::memory_order_relaxed);
        if (missing) {
            memset(g.out + 2 * produced, 0, (size_t)missing * 4);
            if (g.armed.load(std::memory_order_relaxed)) {
                g.underruns.fetch_add(1, std::memory_order_relaxed);
                g.underrun_frames.fetch_add(missing, std::memory_order_relaxed);
            }
        }
        const uint32_t vol = g.volume_q16.load(std::memory_order_relaxed);
        if (vol != 65536) {
            for (uint32_t k = 0; k < grain * 2; k++) g.out[k] = (int16_t)(((int32_t)g.out[k] * (int32_t)vol) >> 16);
        }
        uint64_t t0 = now_us();
        int rc = sceAudioOutOutput(g.port, g.out);
        uint32_t dt = (uint32_t)(now_us() - t0);
        if (rc < 0) {
            g.out_errors.fetch_add(1, std::memory_order_relaxed);
            sleep_us(5000);   // do not spin if the port went away
        }
        if (dt > g.max_out_us.load(std::memory_order_relaxed)) g.max_out_us.store(dt, std::memory_order_relaxed);
        g.periods.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace

bool audio_open(const AudioParams& p) {
    if (g.open) return true;
    int ring_frames = p.ring_frames;
    if (ring_frames < 1024 || (ring_frames & (ring_frames - 1))) {
        logf("audio_open: ring_frames %d must be a power of two >= 1024", ring_frames);
        return false;
    }
    int grain = (p.period_frames + 63) & ~63;
    if (grain < 64) grain = 64;
    if (grain > 65472) grain = 65472;

    SceAudioOutPortType type = SCE_AUDIO_OUT_PORT_TYPE_MAIN;
    int rate = 48000;
    bool use_bgm = p.port == AudioParams::Bgm44k;
    if (use_bgm) { type = SCE_AUDIO_OUT_PORT_TYPE_BGM; rate = p.sample_rate; }
    else if (p.port == AudioParams::Auto && p.sample_rate == 48000) rate = 48000;

    int port = sceAudioOutOpenPort(type, grain, rate, SCE_AUDIO_OUT_MODE_STEREO);
    int open_rc = port;
    logf("audio_open: type=%s grain=%d rate=%d -> %d (0x%08X)", type == SCE_AUDIO_OUT_PORT_TYPE_MAIN ? "MAIN" : "BGM", grain, rate, port,
         (unsigned)port);
    if (port < 0 && use_bgm) {   // graceful: fall back to MAIN with resampling
        type = SCE_AUDIO_OUT_PORT_TYPE_MAIN; rate = 48000;
        port = sceAudioOutOpenPort(type, grain, rate, SCE_AUDIO_OUT_MODE_STEREO);
        logf("audio_open: BGM failed, fallback MAIN grain=%d rate=%d -> %d (0x%08X)", grain, rate, port, (unsigned)port);
    }
    if (port < 0) return false;

    g.src_rate = p.sample_rate;
    g.dst_rate = rate;
    g.resample = (p.sample_rate != rate);
    g.grain = grain;
    g.port = port;
    g.mask = (uint32_t)ring_frames - 1;
    g.ring = (int16_t*)memalign(64, (size_t)ring_frames * 4);
    g.out = (int16_t*)memalign(64, (size_t)grain * 4);
    g.stg_cap = (uint32_t)grain * 2 + 16;
    g.stg = (int16_t*)memalign(64, (size_t)g.stg_cap * 4);
    if (!g.ring || !g.out || !g.stg) { logf("audio_open: out of memory"); sceAudioOutReleasePort(port); return false; }
    memset(g.ring, 0, (size_t)ring_frames * 4);
    memset(g.out, 0, (size_t)grain * 4);
    memset(g.stg, 0, (size_t)g.stg_cap * 4);
    g.head = 0; g.tail = 0; g.stg_n = 1; g.phase = 0;   // stg frame 0 is the (silent) history frame of the cubic
    g.periods = 0; g.frames_in = 0; g.frames_out = 0; g.underruns = 0; g.underrun_frames = 0; g.overruns = 0; g.out_errors = 0; g.max_out_us = 0;
    g.armed = false;
    g.run = true;
    g.st_static = AudioStats();
    g.st_static.port_rate = rate;
    g.st_static.port_type = (int)type;
    g.st_static.port_grain = grain;
    g.st_static.open_rc = open_rc;
    g.st_static.resampling = g.resample;
    ThreadDesc d;
    d.name = "plat-audio";
    d.fn = pump_main;
    d.core = p.core < 0 ? helper_core() : p.core;
    d.priority = p.priority;
    d.stack_bytes = 64 * 1024;
    g.th = thread_start(d);
    if (!g.th) { g.run = false; sceAudioOutReleasePort(port); return false; }
    g.open = true;
    logf("audio_open: ok port=%d src=%d dst=%d resample=%d ring=%d frames core=%d prio=%d", port, g.src_rate, g.dst_rate, (int)g.resample,
         ring_frames, d.core, p.priority);
    return true;
}

void audio_close() {
    if (!g.open) return;
    g.run = false;
    thread_join(g.th);
    g.th = nullptr;
    sceAudioOutReleasePort(g.port);
    g.port = -1;
    free(g.ring); free(g.out); free(g.stg);
    g.ring = g.out = g.stg = nullptr;
    g.open = false;
    logf("audio_close: done");
}

bool audio_is_open() { return g.open; }

bool audio_set_source(AudioSource fn, AudioFill fill) {
    g.source = fn;
    g.fill = fill;
    return true;
}

size_t audio_write(const int16_t* in, size_t frames) {
    if (!g.open) return 0;
    uint32_t h = g.head.load(std::memory_order_relaxed);
    uint32_t t = g.tail.load(std::memory_order_acquire);
    uint32_t cap = g.mask + 1;
    uint32_t freef = cap - (h - t);
    size_t n = frames < freef ? frames : freef;
    for (size_t i = 0; i < n; i++) {
        uint32_t idx = (h + (uint32_t)i) & g.mask;
        g.ring[2 * idx] = in[2 * i];
        g.ring[2 * idx + 1] = in[2 * i + 1];
    }
    g.head.store(h + (uint32_t)n, std::memory_order_release);
    g.frames_in.fetch_add(n, std::memory_order_relaxed);
    if (n) g.armed.store(true, std::memory_order_relaxed);
    if (n < frames) g.overruns.fetch_add(1, std::memory_order_relaxed);
    return n;
}

size_t audio_free_frames() {
    if (!g.open) return 0;
    return (g.mask + 1) - (g.head.load(std::memory_order_relaxed) - g.tail.load(std::memory_order_acquire));
}
size_t audio_queued_frames() {
    if (!g.open) return 0;
    return g.head.load(std::memory_order_acquire) - g.tail.load(std::memory_order_acquire);
}
void audio_set_volume(float v) {
    if (v < 0.f) v = 0.f;
    if (v > 1.f) v = 1.f;
    g.volume_q16.store((uint32_t)(v * 65536.f), std::memory_order_relaxed);
}

AudioStats audio_stats() {
    AudioStats s = g.st_static;
    s.periods_out = g.periods.load(std::memory_order_relaxed);
    s.frames_in = g.frames_in.load(std::memory_order_relaxed);
    s.frames_out = g.frames_out.load(std::memory_order_relaxed);
    s.underruns = g.underruns.load(std::memory_order_relaxed);
    s.underrun_frames = g.underrun_frames.load(std::memory_order_relaxed);
    s.overruns = g.overruns.load(std::memory_order_relaxed);
    s.output_errors = g.out_errors.load(std::memory_order_relaxed);
    s.max_output_us = g.max_out_us.load(std::memory_order_relaxed);
    s.rate_ppm = (int32_t)g.rate.ppm;   // written by the pump thread, read here for the log: a torn read is a wrong digit at worst
    return s;
}

}  // namespace plat
