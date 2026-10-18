#include "app_status.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <string_view>
#include <string>
#include <vector>

#include "app_config.h"
#include "app_memory.h"
#include "app_ui.h"
#include "platform.h"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/exec.hpp"
#include "psprecomp/runtime.hpp"
#include "audio/audio_sink.hpp"
#include "gpu/ge_worker.hpp"
#include "gpu/gl/gl_renderer.hpp"
#include "gpu/split_renderer.hpp"
#include "settings/port_settings.hpp"
#include "infected_bot.hpp"
#include "infected_tuning.hpp"
#include "hle/hle_common.hpp"
#include "kernel/kernel.hpp"
#include "perf/bringup_stats.hpp"
#include "perf/frame_stats.hpp"
#include "perf/phase_timers.hpp"

extern psprecomp::AllegrexContext g_cpu;   // exec.cpp: the one live register file (read racily for the watchdog's report)

namespace app::status {
namespace {

using namespace portablekit;

uint32_t g_cpu_pc() { return g_cpu.pc; }

struct Snap {
    uint64_t t_us = 0;
    uint64_t flips = 0;
    uint32_t us[phase::kCount] = {};
    uint32_t calls[phase::kCount] = {};
    uint64_t switches = 0;
    uint64_t underruns = 0;
    uint64_t gl_draws = 0;
    uint64_t submits = 0;
    uint64_t presented = 0;
    uint64_t lean_calls = 0, interp_calls = 0, hle_calls = 0;
};

Snap g_last;
psprecomp::Runtime* g_rt = nullptr;
struct CallSite {
    std::string library;
    uint32_t nid = 0;
    uint32_t ra = 0;
    uint64_t count = 0;
};
std::map<std::pair<uint32_t, uint32_t>, CallSite> g_sites;   // (nid, ra) -> count; library kept for the name lookup

uint32_t g_trace_left = 0;       // trace_hle=N: the first N HLE calls, one line each, in call order (thread, import, a0..a3)

// The watchdog's view of the guest: the last HLE import it called (the stall is usually inside or right after one).
std::atomic<uint32_t> g_last_nid{0};
std::atomic<const char*> g_last_library{nullptr};
std::atomic<uint64_t> g_import_calls{0};
std::atomic<bool> g_wd_stop{false};
plat::Thread* g_wd_thread = nullptr;

void on_import(psprecomp::Runtime& rt, std::string_view library, uint32_t nid, psprecomp::AllegrexContext& ctx) {
    g_last_nid.store(nid, std::memory_order_relaxed);
    g_last_library.store(library.data(), std::memory_order_relaxed);   // names live in the registry for the whole run
    g_import_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_trace_left) {
        g_trace_left--;
        const auto name = rt.nids().resolve(std::string(library), nid);
        portablekit::Thread* t = portablekit::kernel().current_thread();
        uint32_t lo = 0, hi = 0;
        if (name && *name == "sceKernelSysClock2USec") {   // the 64-bit clock value the game is about to convert
            lo = rt.memory().load32(ctx.gpr[4]);
            hi = rt.memory().load32(ctx.gpr[4] + 4);
        }
        plat::logf("[call] %-18s %s %08X %08X %08X %08X%s", t ? t->name.c_str() : "?", name ? name->c_str() : "?", ctx.gpr[4], ctx.gpr[5], ctx.gpr[6], ctx.gpr[7],
                   (name && *name == "sceKernelSysClock2USec") ? (std::string(" clock=") + std::to_string(((uint64_t)hi << 32) | lo)).c_str() : "");
    }
    if (cfg_int("hle_top", 0) <= 0) return;
    const uint32_t ra = ctx.gpr[31];
    CallSite& c = g_sites[{nid, ra}];
    if (c.count++ == 0) {
        c.library = std::string(library);
        c.nid = nid;
        c.ra = ra;
    }
}
bool g_installed = false;
uint64_t g_minute_mark = 0;

Snap take() {
    Snap s;
    s.t_us = plat::now_us();
    s.flips = bringup::flip_count();
    for (unsigned i = 0; i < phase::kCount; i++) {
        s.us[i] = phase::total_us((phase::Phase)i);
        s.calls[i] = phase::total_calls((phase::Phase)i);
    }
    const psprecomp::ExecStats e = psprecomp::exec_stats();
    s.lean_calls = e.lean_calls;
    s.interp_calls = e.interpreted_calls;
    s.hle_calls = e.hle_calls;
    s.switches = kernel().switch_count();
    s.underruns = plat::audio_is_open() ? plat::audio_stats().underruns : 0;
    if (gpu::Renderer* r = media_renderer()) {
        s.presented = r->frames_presented();
        s.submits = r->draws_submitted();
#if !defined(INFECTED_NO_GL)
        // total_stats() only takes in a frame's counters when reset_frame_stats() is called (the replay tools do; the game does not): add the open frame.
        const gpu::GlRenderer* gl = static_cast<gpu::GlRenderer*>(gpu::innermost(r));
        s.gl_draws = gl->total_stats().gl_draws + gl->frame_stats().gl_draws;
#endif
    }
    return s;
}

// ------------------------------------------------------------------------------------------------------------ bench=play
// The report of bench=play (app_bench.h): the bot plays a mission with the normal threaded renderer and every `every_s` seconds, and once more
// at the end, one [play] line says how the device did. Frame times come from the flips themselves (tuning's flip observer, on the guest thread).
struct Metrics {
    double frame_ms = 0, guest_ms = 0, ge_pct = 0, present_ms = 0;
    unsigned draws_pf = 0, gl_pf = 0;
};

Metrics metrics_between(const Snap& a, const Snap& b, bool threaded) {
    Metrics m;
    const uint64_t df = b.flips - a.flips;
    const double dt = (b.t_us - a.t_us) / 1e6;
    if (df == 0 || dt <= 0.0) return m;
    const double frames = (double)df;
    double ms[phase::kCount];
    for (unsigned i = 0; i < phase::kCount; i++) ms[i] = (uint32_t)(b.us[i] - a.us[i]) / 1000.0 / frames;
    portablekit::gpu::SplitRenderer* split = portablekit::gpu::split_of(media_renderer());
    const bool split_on = split != nullptr && split->forwarding();
    const double gewalk = split_on ? ms[phase::GeWalk] : ms[phase::GeWalk] - ms[phase::GlSubmit];
    const double hle_excl = ms[phase::Hle] - ms[phase::GeWalk] - ms[phase::Present] - ms[phase::Decode] - ms[phase::Audio] - ms[phase::IsoRead];
    const double accounted = (threaded ? 0.0 : (gewalk > 0 ? gewalk : 0) + ms[phase::GlSubmit] + ms[phase::Present]) + (hle_excl > 0 ? hle_excl : 0) + ms[phase::Decode] +
                             ms[phase::Audio] + ms[phase::IsoRead] + ms[phase::Idle];
    m.frame_ms = dt * 1000.0 / frames;
    m.guest_ms = std::max(0.0, m.frame_ms - accounted);
    m.present_ms = ms[phase::Present];
    // The busier of the GE side's threads: one GE thread (GeWalk contains GlSubmit), or with the split the walker or the GL thread.
    const double walker_busy = std::max(0.0, m.frame_ms - ms[phase::WalkIdle] - (split_on ? ms[phase::SplitWait] : 0.0));
    const double ge_side = split_on ? std::max(walker_busy, m.frame_ms - ms[phase::GlIdle]) : (threaded ? walker_busy : ms[phase::GeWalk] + ms[phase::Present]);
    m.ge_pct = m.frame_ms > 0 ? 100.0 * ge_side / m.frame_ms : 0.0;
    m.draws_pf = (unsigned)((b.submits - a.submits) / df);
    m.gl_pf = (unsigned)((b.gl_draws - a.gl_draws) / df);
    return m;
}

struct Play {
    bool on = false, began = false, finished = false;
    long max_s = 300, every_s = 10;
    uint64_t begin_us = 0, win_us = 0, last_flip_us = 0;
    uint64_t begin_flip = 0;
    Snap win, all;
    std::vector<uint32_t> dt_win, dt_all;           // microseconds between two flips
    struct Alive {
        uint64_t sum = 0, n = 0;
        unsigned max = 0;
    } aw, aa;                                       // enemies alive: this window, the whole run
    unsigned lines = 0;
};
Play g_play;

// fps over a set of frame intervals: average, and the average rate of the slowest 1 % of the frames ("1 % low").
void fps_of(const std::vector<uint32_t>& dts, double* avg, double* low1) {
    *avg = *low1 = 0.0;
    if (dts.empty()) return;
    uint64_t sum = 0;
    for (uint32_t d : dts) sum += d;
    if (sum == 0) return;
    *avg = 1e6 * (double)dts.size() / (double)sum;
    std::vector<uint32_t> sorted(dts);
    const size_t worst = std::max<size_t>(1, sorted.size() / 100);
    std::nth_element(sorted.begin(), sorted.begin() + (sorted.size() - worst), sorted.end());
    uint64_t wsum = 0;
    for (size_t i = sorted.size() - worst; i < sorted.size(); i++) wsum += sorted[i];
    *low1 = wsum ? 1e6 * (double)worst / (double)wsum : 0.0;
}

// "defaults", or the settings that differ from the defaults (a developer override in config.txt counts), plus the pack the renderer really uses.
std::string settings_text() {
    namespace port = portablekit::port;
    std::string text;
    for (size_t i = 0; i < port::kKeyCount; i++) {
        const port::Key key = (port::Key)i;
        if (port::settings().is_default(key)) continue;
        text += std::string(text.empty() ? "" : " ") + port::info(key).name + "=" + port::settings().value(key);
    }
    if (text.empty()) text = "all defaults";
    const char* pack = "off";
#if !defined(INFECTED_NO_GL)
    if (gpu::Renderer* r = media_renderer()) {
        const gpu::HdMode mode = static_cast<gpu::GlRenderer*>(gpu::innermost(r))->hd_mode();
        pack = mode == gpu::HdMode::X2 ? "2x" : (mode == gpu::HdMode::X4 ? "4x" : "off");
    }
#endif
    return std::string("HD pack in use=") + pack + "; settings: " + text;
}

void play_line(const char* tag, const Snap& from, const Snap& to, const std::vector<uint32_t>& dts, uint64_t shown_us, const Play::Alive& al) {
    const bool threaded = portablekit::gpu::ge_worker().active();
    const Metrics m = metrics_between(from, to, threaded);
    double avg = 0, low1 = 0;
    fps_of(dts, &avg, &low1);
    const infected::bot::Status st = infected::bot::status();
    const double alive_avg = al.n ? (double)al.sum / (double)al.n : 0.0;
    plat::logf("[play] %s%4.0fs flips=%u fps=%.1f low1%%=%.1f | guest=%.1fms ge=%.0f%% swap=%.1fms | draws/f=%u | infected=%u(avg %.1f max %u) deaths=%u | %s", tag,
               shown_us / 1e6, (unsigned)dts.size(), avg, low1, m.guest_ms, m.ge_pct, m.present_ms, m.draws_pf, st.infected_alive, alive_avg, al.max, st.deaths,
               settings_text().c_str());
    plat::log_flush();
}

void on_flip(uint64_t flip) {
    Play& p = g_play;
    if (!p.on || p.finished) return;
    const uint64_t now = plat::now_us();
    const infected::bot::Status st = infected::bot::status();
    if (!p.began) {
        if (!st.playing) return;
        p.began = true;
        p.begin_us = p.win_us = p.last_flip_us = now;
        p.begin_flip = flip;
        p.win = p.all = take();
        plat::logf("[play] begin at flip %llu: a mission is running, the bot plays; a line every %lds, the run ends %lds after this point or when the mission is over",
                   (unsigned long long)flip, p.every_s, p.max_s);
        plat::logf("[play] %s", settings_text().c_str());
        return;
    }
    const uint32_t dt = (uint32_t)std::min<uint64_t>(now - p.last_flip_us, 0xFFFFFFFFu);
    p.last_flip_us = now;
    p.dt_win.push_back(dt);
    if (p.dt_all.size() < 2000000) p.dt_all.push_back(dt);
    for (Play::Alive* a : {&p.aw, &p.aa}) {
        a->sum += st.infected_alive;
        a->n++;
        a->max = std::max(a->max, st.infected_alive);
    }
    const bool over = st.results || now - p.begin_us >= (uint64_t)p.max_s * 1000000ull;
    if (now - p.win_us >= (uint64_t)p.every_s * 1000000ull && !over) {
        const Snap t = take();
        play_line("t=", p.win, t, p.dt_win, now - p.begin_us, p.aw);  // the window is every_s long, the label is the time since the mission began
        p.lines++;
        p.win = t;
        p.win_us = now;
        p.dt_win.clear();
        p.aw = Play::Alive();
    }
    if (over) {
        p.finished = true;
        const Snap t = take();
        if (!p.dt_win.empty()) play_line("last t=", p.win, t, p.dt_win, now - p.begin_us, p.aw);
        play_line("FINAL ", p.all, t, p.dt_all, now - p.begin_us, p.aa);
        plat::logf("[play] end: %s after %.0f s of play (%llu flips), mission %s, bot deaths %u. %s", st.results ? "the mission is over" : "time limit",
                   (now - p.begin_us) / 1e6, (unsigned long long)(flip - p.begin_flip), st.missions_completed ? "COMPLETED" : "not completed", st.deaths,
                   app::memory_line().c_str());
        plat::log_flush();
        if (g_rt != nullptr) g_rt->stop("bench complete (play)");
    }
}

}  // namespace

void play_enable(long max_seconds, long every_seconds) {
    g_play.on = true;
    g_play.max_s = max_seconds > 0 ? max_seconds : 300;
    g_play.every_s = every_seconds > 0 ? every_seconds : 10;
    infected::set_flip_observer(&on_flip);
}

void report(const char* why) {
    const Snap now = take();
    const double dt = (now.t_us - g_last.t_us) / 1e6;
    if (dt <= 0.0) return;
    const uint64_t df = now.flips - g_last.flips;
    const double frames = df ? (double)df : 1.0;
    double ms[phase::kCount];
    double accounted = 0.0;
    for (unsigned i = 0; i < phase::kCount; i++) {
        ms[i] = (uint32_t)(now.us[i] - g_last.us[i]) / 1000.0 / frames;
    }
    // GeWalk contains GlSubmit. The leaves (decode, audio mix, disc reads, the display lists, the flip) run inside HLE calls, so the
    // timed HLE total (stats_hle=1) has them in it; hle= below is what is left of it.
    // With the split GE thread (config.txt ge_split=1) GlSubmit is the GL thread's, not part of the walker's GeWalk.
    portablekit::gpu::SplitRenderer* split = portablekit::gpu::split_of(media_renderer());
    const bool split_on = split != nullptr && split->forwarding();
    const double gewalk = split_on ? ms[phase::GeWalk] : ms[phase::GeWalk] - ms[phase::GlSubmit];
    const double hle_excl = ms[phase::Hle] - ms[phase::GeWalk] - ms[phase::Present] - ms[phase::Decode] - ms[phase::Audio] - ms[phase::IsoRead];
    // With the GE thread (core 1) the list walk, the GL submission and the present run beside the guest, not inside its frame: they are
    // not subtracted from it, and "ge=" is the share of the frame the GE thread was busy (above 100 % the GE thread is the bottleneck).
    static bool threaded_seen = false;   // the last report comes after the worker has stopped
    if (portablekit::gpu::ge_worker().active()) threaded_seen = true;
    const bool threaded = threaded_seen;
    accounted = (threaded ? 0.0 : (gewalk > 0 ? gewalk : 0) + ms[phase::GlSubmit] + ms[phase::Present]) + (hle_excl > 0 ? hle_excl : 0) +
                ms[phase::Decode] + ms[phase::Audio] + ms[phase::IsoRead] + ms[phase::Idle];
    const double frame_ms = df ? dt * 1000.0 / df : 0.0;
    const double guest = frame_ms - accounted;
    const unsigned draws_pf = df ? (unsigned)((now.submits - g_last.submits) / df) : 0;
    const unsigned gl_pf = df ? (unsigned)((now.gl_draws - g_last.gl_draws) / df) : 0;
    // The GE side's threads are busy whenever they are not asleep waiting for a job (WalkIdle, GlIdle) or, the walker, for the GL
    // thread (SplitWait): exact, and two clock reads a sleep instead of two a draw.
    const double walker_busy_ms = std::max(0.0, frame_ms - ms[phase::WalkIdle] - (split_on ? ms[phase::SplitWait] : 0.0));
    const double gl_busy_ms = std::max(0.0, frame_ms - ms[phase::GlIdle]);
    const double ge_busy_ms = walker_busy_ms;
    char ge_label[48];
    if (split_on) snprintf(ge_label, sizeof(ge_label), "split:walk %.0f%% gl ", frame_ms > 0 ? 100.0 * walker_busy_ms / frame_ms : 0.0);
    else snprintf(ge_label, sizeof(ge_label), "%s", threaded ? "thr:" : "inline:");
    const auto snd = portablekit::audio::AudioSink::instance().telemetry();
    // Frame skipping: frames left undrawn so far, how far behind the walker the GL thread runs, and whether the guest runs ahead of it.
    const portablekit::gpu::FrameSkipStats fs = split_on ? split->frame_skip_stats() : portablekit::gpu::FrameSkipStats{};
    char line[960];
    snprintf(line, sizeof(line),
             "[stat] %s t=%.0fs flips=%llu(+%llu) presented=%llu(+%llu) frame=%.1fms ge=%s%.0f%% | ms/frame guest=%.1f hle=%s gewalk=%.1f (vertex=%.1f) glsubmit=%.1f present=%.1f "
             "decode=%.1f audio=%.1f io=%.1f idle=%.1f (gesync=%.1f flipwait=%.1f) | splitwait=%.1f walkidle=%.1f glidle=%.1f | draws/f=%u gl_draws/f=%u | switches/s=%.0f calls/f lean=%.0f interp=%.0f hle=%.0f | audio q=%u underruns=%llu(+%llu) snd fill=%llu silent=%llu lost=%llu rst=%llu over=%llu lead=%lld ppm=%d | skip=%llu catchup=%d lag=%.1fms",
             why, now.t_us / 1e6, (unsigned long long)now.flips, (unsigned long long)df, (unsigned long long)now.presented,
             (unsigned long long)(now.presented - g_last.presented), frame_ms, ge_label,
             frame_ms > 0 ? 100.0 * (split_on ? gl_busy_ms : ge_busy_ms) / frame_ms : 0.0, guest < 0 ? 0.0 : guest,
             cfg_bool("stats_hle", false) ? "on" : "off", gewalk < 0 ? 0.0 : gewalk, ms[phase::GeVertex], ms[phase::GlSubmit], ms[phase::Present], ms[phase::Decode],
             ms[phase::Audio], ms[phase::IsoRead], ms[phase::Idle], ms[phase::GeSync], ms[phase::FlipWait], ms[phase::SplitWait], ms[phase::WalkIdle], ms[phase::GlIdle], draws_pf, gl_pf, (now.switches - g_last.switches) / dt,
             (now.lean_calls - g_last.lean_calls) / frames, (now.interp_calls - g_last.interp_calls) / frames, (now.hle_calls - g_last.hle_calls) / frames,
             plat::audio_is_open() ? (unsigned)plat::audio_queued_frames() : 0u, (unsigned long long)now.underruns,
             (unsigned long long)(now.underruns - g_last.underruns),
             (unsigned long long)snd.fill, (unsigned long long)snd.silent, (unsigned long long)snd.dropped, (unsigned long long)snd.restarts,
             (unsigned long long)plat::audio_stats().overruns, (long long)snd.lead, (int)plat::audio_stats().rate_ppm,
             (unsigned long long)fs.skipped_frames, fs.catchup ? 1 : 0, fs.lag_us / 1000.0);
    // with the HLE timer on, replace the on/off marker by the number
    if (cfg_bool("stats_hle", false)) {
        char* p = strstr(line, "hle=on");
        if (p) {
            char num[16];
            snprintf(num, sizeof(num), "%.1f", hle_excl > 0 ? hle_excl : 0.0);
            std::string rest = p + 6;
            std::string head(line, p - line);
            snprintf(line, sizeof(line), "%shle=%s%s", head.c_str(), num, rest.c_str());
        }
    }
    plat::logf("%s", line);
    plat::log_flush();   // one flush per status line: a crash or a power-off loses at most the last second

    // hle_top=N: the N most called HLE imports of the last second, each with the return address of its most frequent call site.
    if (g_rt != nullptr && cfg_int("hle_top", 0) > 0) {
        const size_t top = (size_t)cfg_int("hle_top", 0);
        std::vector<std::pair<uint64_t, const CallSite*>> order;
        for (const auto& e : g_sites) order.emplace_back(e.second.count, &e.second);
        std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::string out;
        for (size_t i = 0; i < order.size() && i < top; i++) {
            const CallSite& c = *order[i].second;
            const auto name = g_rt->nids().resolve(c.library, c.nid);
            char one[200];
            snprintf(one, sizeof(one), " %s@%08X=%llu", name ? name->c_str() : "?", c.ra, (unsigned long long)c.count);
            out += one;
        }
        g_sites.clear();
        plat::logf("[hle]%s", out.empty() ? " (no imports this second)" : out.c_str());
    }

    // dump_threads=N: the kernel's thread table (state, what each waits for, pc) every N seconds.
    {
        static uint64_t next_dump_us = 0;
        const int every = (int)cfg_int("dump_threads", 0);
        if (every > 0 && now.t_us >= next_dump_us) {
            next_dump_us = now.t_us + (uint64_t)every * 1000000ull;
            const std::string text = kernel().describe_threads();
            size_t pos = 0;
            while (pos <= text.size()) {
                size_t eol = text.find('\n', pos);
                if (eol == std::string::npos) eol = text.size();
                plat::logf("[threads] %.*s", (int)(eol - pos), text.c_str() + pos);
                pos = eol + 1;
            }
        }
    }

    ui::hud_enable(cfg_bool("hud", false));
    char h1[160], h2[160];
    snprintf(h1, sizeof(h1), "%.1ffps %lluf %.1fms g%.1f ge%.1f gl%.1f p%.1f", df / dt, (unsigned long long)now.flips, frame_ms,
             guest < 0 ? 0.0 : guest, gewalk < 0 ? 0.0 : gewalk, ms[phase::GlSubmit], ms[phase::Present]);
    plat::MemInfo m;
    plat::mem_info(&m);
    snprintf(h2, sizeof(h2), "d%u/%u sw%.0f user%dM cdram%dM heap%uM", draws_pf, gl_pf, (now.switches - g_last.switches) / dt,
             (int)(m.user_free >> 20), (int)(m.cdram_free >> 20), (unsigned)(m.malloc_arena >> 20));
    ui::hud_set(h1, h2);

    app::memory_sample();
    if (now.t_us - g_minute_mark >= 60ull * 1000000ull) {
        g_minute_mark = now.t_us;
        plat::logf("[stat] %s", app::memory_line().c_str());
    }
    g_last = now;
}

// A thread on core 2 that logs when the game has not flipped for `watchdog` seconds (config.txt, default 8, 0 = off): the last HLE
// import called, the guest pc and the thread table. It only reads; the stall itself is not touched.
void watchdog_main(void*) {
    const uint64_t limit_us = (uint64_t)cfg_int("watchdog", 8) * 1000000ull;
    uint64_t last_flips = bringup::flip_count();
    uint64_t last_change = plat::now_us();
    uint64_t last_report = 0;
    uint64_t combo_since = 0;
    while (!g_wd_stop.load(std::memory_order_relaxed)) {
        plat::sleep_us(500000);
        const uint64_t now = plat::now_us();
        // Last resort out of a game that no longer reads the pad: START+SELECT+L+R held for 3 s ends the process.
        {
            plat::PadState pad;
            const uint32_t combo = plat::PAD_START | plat::PAD_SELECT | plat::PAD_LTRIGGER | plat::PAD_RTRIGGER;
            if (plat::pad_poll(&pad) && (pad.buttons & combo) == combo) {
                if (combo_since == 0) combo_since = now;
                if (now - combo_since >= 3000000ull) {
                    plat::logf("quit: START+SELECT+L+R held for 3 s while the game does not respond; exiting now");
                    plat::log_flush();
                    plat::exit_process(0);
                }
            } else {
                combo_since = 0;
            }
        }
        const uint64_t flips = bringup::flip_count();
        if (flips != last_flips) {
            if (last_report) plat::logf("[watchdog] flips resumed after %.1f s", (now - last_change) / 1e6);
            last_flips = flips;
            last_change = now;
            last_report = 0;
            continue;
        }
        if (now - last_change < limit_us || (last_report && now - last_report < 10000000ull)) continue;
        last_report = now;
        const char* lib = g_last_library.load(std::memory_order_relaxed);
        std::string name = "?";
        if (g_rt && lib) {
            const auto n = g_rt->nids().resolve(std::string(lib), g_last_nid.load(std::memory_order_relaxed));
            if (n) name = *n;
        }
        plat::logf("[watchdog] no flip for %.1f s (flips=%llu) | last HLE import %s | %llu imports so far | guest pc 0x%08X | gl %s", (now - last_change) / 1e6,
                   (unsigned long long)flips, name.c_str(), (unsigned long long)g_import_calls.load(std::memory_order_relaxed), g_cpu_pc(),
                   plat::gl_is_ready() ? "up" : "down");
        plat::log_flush();
    }
}

void set_runtime(void* runtime) {
    g_rt = static_cast<psprecomp::Runtime*>(runtime);
    g_trace_left = (uint32_t)cfg_int("trace_hle", 0);
    // The pre-import hook is the watchdog's source of "last call" too, so it is always installed (two relaxed stores per import).
    if (g_rt != nullptr) psprecomp::set_runtime_pre_import_hook(on_import);
}

void install() {
    if (g_installed) return;
    g_installed = true;
    phase::set_clock(&plat::now_us);
    g_last = take();
    g_minute_mark = g_last.t_us;
    kernel().add_vblank_hook([] {
        if (plat::now_us() - g_last.t_us >= 1000000ull) report("");
    });
    plat::logf("status: per-second [stat] lines enabled");
    if (cfg_int("watchdog", 8) > 0 && !g_wd_thread) {
        plat::ThreadDesc d;
        d.name = "watchdog";
        d.fn = &watchdog_main;
        d.stack_bytes = 64 * 1024;
        d.core = plat::helper_core();
        d.priority = 8;   // below everything that matters
        g_wd_thread = plat::thread_start(d);
    }
}

void shutdown() {
    g_wd_stop.store(true);
    if (g_wd_thread) {
        plat::thread_join(g_wd_thread);
        g_wd_thread = nullptr;
    }
}

}  // namespace app::status
