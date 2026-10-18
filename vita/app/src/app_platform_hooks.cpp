#include "app_platform_hooks.h"

#include "platform.h"
#include "app_config.h"
#include "app_hd_movie.h"
#include "app_settings.h"
#include "pad_layout.h"

#include "audio/audio_sink.hpp"
#include "gpu/ge_worker.hpp"
#include "input/pad_provider.hpp"
#include "kernel/kernel.hpp"
#include "kernel/kernel_platform.hpp"
#include "movie/avc_decoder.hpp"
#include "overlay/overlay_host.hpp"
#include "settings/port_settings.hpp"

namespace app {
namespace {

plat::Event* g_kernel_event = nullptr;

uint64_t kp_monotonic_us() { return plat::now_us(); }
void kp_wait(uint64_t timeout_us) {
    // Event semantics of KernelPlatform: a signal that arrived before the wait is not lost; auto-reset event.
    if (timeout_us == 0) return;
    plat::event_wait(g_kernel_event, (int64_t)timeout_us);
}
void kp_signal() { plat::event_signal(g_kernel_event); }

// The GE thread's seam (gpu::GeWorkerPlatform): events and threads of the platform layer.
void* gw_event_create() { return plat::event_create(/*auto_reset=*/true, /*initially_signalled=*/false); }
void gw_event_destroy(void* e) { plat::event_destroy(static_cast<plat::Event*>(e)); }
void gw_event_wait(void* e, uint64_t timeout_us) {
    if (timeout_us == 0) return;
    plat::event_wait(static_cast<plat::Event*>(e), (int64_t)timeout_us);
}
void gw_event_signal(void* e) { plat::event_signal(static_cast<plat::Event*>(e)); }
void* gw_thread_start(void (*fn)(void*), void* arg, const char* name, int core, size_t stack_bytes) {
    plat::ThreadDesc d;
    d.name = name;
    d.fn = fn;
    d.arg = arg;
    d.stack_bytes = stack_bytes;
    d.core = core;
    return plat::thread_start(d);
}
void gw_thread_join(void* t) { plat::thread_join(static_cast<plat::Thread*>(t)); }
void gw_pin_current(int core) { (void)plat::thread_set_core(nullptr, core); }

// The way out of the game: START+SELECT+L+R held for 0.7 s, or the PS button when the system lets the application see it. The pad
// read of the guest (executor thread) turns it into a normal stop of the scheduler; the watchdog thread (app_status.cpp) is the
// way out when the guest does not read the pad any more.
constexpr uint32_t kQuitCombo = plat::PAD_START | plat::PAD_SELECT | plat::PAD_LTRIGGER | plat::PAD_RTRIGGER;
uint64_t g_combo_since_us = 0;

// The controls layout (settings controls.layout; the mapping itself is in pad_layout.h).
padlayout::State g_layout;

uint32_t apply_layout(uint32_t raw, uint8_t right_y, uint64_t now_us) {
    namespace port = portablekit::port;
    // The settings menu has its own buttons (L and R turn its pages, Cross changes a value): it reads the pad through this reader,
    // so while it is open the Vita's buttons pass through unchanged.
    const bool modern = port::control_layout() == port::ControlLayout::Modern && !portablekit::overlay_host::menu().is_open();
    return padlayout::apply(g_layout, modern, port::auto_fire(), raw, right_y, now_us);
}

bool pad_read(portablekit::PadSample& out) {
    plat::PadState s;
    if (!plat::pad_poll(&s)) return false;
    app::settings::poll_touch();   // the top-right corner of the touch screen opens the settings menu
    if ((s.buttons & kQuitCombo) == kQuitCombo || (s.buttons & plat::PAD_HOME) != 0) {
        const uint64_t now = plat::now_us();
        if (g_combo_since_us == 0) g_combo_since_us = now;
        if ((s.buttons & plat::PAD_HOME) != 0 || now - g_combo_since_us >= 700000ull) {
            portablekit::Kernel& k = portablekit::kernel();
            if (!k.runtime().stopped()) {
                plat::logf("quit requested from the pad (%s)", (s.buttons & plat::PAD_HOME) ? "PS button" : "START+SELECT+L+R");
                k.runtime().stop("quit: requested from the pad");
            }
        }
    } else {
        g_combo_since_us = 0;
    }
    out.buttons = apply_layout(s.buttons, s.ry, plat::now_us());
    out.analog_x = s.lx;
    out.analog_y = s.ly;
    out.right_x = s.rx;
    out.right_y = s.ry;
    return true;
}

bool audio_open_44k() {
    plat::AudioParams p;
    p.sample_rate = 44100;
    return plat::audio_open(p);
}
size_t audio_push(const int16_t* frames, size_t count) { return plat::audio_write(frames, count); }
// Pull mode (the Vita): the pump thread asks for exactly what the hardware needs, so the guest's clock never has to match the device's.
size_t audio_pull(int16_t* out, size_t frames) {
    portablekit::audio::AudioSink::instance().pull(out, frames);
    return frames;
}
// The least lead over the device any of the game's audio producers had when it last wrote (frames; -1 when none wrote for a few
// tenths of a second: a pause, a silent screen), for the pump's clock follower. It is the margin of the producer nearest to being
// found behind the device and restarted.
int64_t audio_fill() { return portablekit::audio::AudioSink::instance().telemetry().lead; }
void audio_close_dev() { plat::audio_close(); }
void audio_gain(float g) { plat::audio_set_volume(g); }

}  // namespace

void install_platform_hooks() {
    if (!g_kernel_event) g_kernel_event = plat::event_create(/*auto_reset=*/true, /*initially_signalled=*/false);
    portablekit::KernelPlatform kp;
    kp.monotonic_us = &kp_monotonic_us;
    kp.wait_event = &kp_wait;
    kp.signal_event = &kp_signal;
    portablekit::set_kernel_platform(kp);

    portablekit::gpu::GeWorkerPlatform gw;
    gw.event_create = &gw_event_create;
    gw.event_destroy = &gw_event_destroy;
    gw.event_wait = &gw_event_wait;
    gw.event_signal = &gw_event_signal;
    gw.thread_start = &gw_thread_start;
    gw.thread_join = &gw_thread_join;
    gw.pin_current_thread = &gw_pin_current;
    gw.monotonic_us = &kp_monotonic_us;
    portablekit::gpu::set_ge_worker_platform(gw);

    // The movie player decodes one picture ahead on core 2 (idle while a movie plays: the geometry thread has nothing to walk).
    portablekit::movie::DecodeThreadPlatform dp;
    dp.event_create = &gw_event_create;
    dp.event_destroy = &gw_event_destroy;
    dp.event_wait = &gw_event_wait;
    dp.event_signal = &gw_event_signal;
    dp.thread_start = &gw_thread_start;
    dp.thread_join = &gw_thread_join;
    dp.core = plat::helper_core();   // core 3 when the system lets the application use it, else core 2 beside the walker
    portablekit::movie::set_decode_thread_platform(dp);

    portablekit::set_pad_provider(&pad_read);

    portablekit::audio::AudioOutput out;
    // config.txt audio_follow=0: the pump keeps the device's own rate (no clock follower), to compare by ear and by the stat line.
    out.pulls = plat::audio_set_source(&audio_pull, app::cfg_bool("audio_follow", true) ? &audio_fill : nullptr);   // false on the desktop null sink, which keeps the push model
    out.open = &audio_open_44k;
    out.write = &audio_push;
    out.close = &audio_close_dev;
    out.set_gain = &audio_gain;
    portablekit::audio::set_audio_output(out);
    app::hd_movie_install();   // the hardware source of the HD replacement movies (nothing is loaded until one is opened)
    plat::logf("hooks: kernel clock/idle, pad provider and audio output (%s) bound to the platform layer", out.pulls ? "pulled by the device" : "pushed");
}

void shutdown_platform_hooks() {
    if (plat::audio_is_open()) plat::audio_close();
}

}  // namespace app
