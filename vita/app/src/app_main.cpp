// app_main.cpp - Infected for the PS Vita: start-up, boot flow and the main loop.
//
//   1. platform layer, log, config.txt (exported to the environment), stdout/stderr -> log
//   2. clocks, vitaGL and the boot screen
//   3. game data check (the .iso in ux0:data/infected/, Infected.iso or any name that looks like it)
//   4. guest arena, runtime, executable from the disc, system (HLE, kernel, renderer), pad script
//   5. the scheduler (portablekit::kernel().run_main()) until the runtime stops
//   6. clean exit, or the fatal-error screen with the message and the guest PC
//
// The same PortableKit sources as the desktop build run underneath; everything Vita specific is here, in the platform layer
// (vita/platform/vita) and behind the seams listed in app_platform_hooks.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>

#include "app_bench.h"
#include "app_config.h"
#include "app_fatal.h"
#include "app_install.h"
#include "app_iso.h"
#include "app_log.h"
#include "app_memory.h"
#include "app_pad_script.h"
#include "app_platform_hooks.h"
#include "app_settings.h"
#include "app_status.h"
#include "app_ui.h"
#include "platform.h"

#include "kernel/kernel.hpp"
#include "perf/bringup_stats.hpp"
#include "profile.hpp"
#include "system.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/runtime.hpp"
#include "gpu/ge_worker.hpp"
#include "gpu/split_renderer.hpp"
#include "hle/hle_common.hpp"

extern psprecomp::AllegrexContext g_cpu;   // exec.cpp: the one live register file

// Provided by the lean library's table (reads LEAN_NO_HOOKS: "memcpy,memset,strcmp" or "all" switches its native replacements off).
extern "C" void lean_init_hooks_from_env(void) __attribute__((weak));

namespace {

struct EmbeddedNid {
    const char* library;
    std::uint32_t nid;
    const char* name;
};
constexpr EmbeddedNid kEmbeddedNids[] = {
#include "nid_table.inc"
};

void apply_config_defaults() {
    // Runtime defaults for the handheld; config.txt overrides any of them (keys are INFECTED_<NAME>, case-insensitive).
    app::config_default("CLOCK", "realtime");           // the kernel's clock follows the device (the virtual clock is for desktop runs)
    app::config_default("GL_USE_GLCLEAR", "0");          // vitaGL's glClear is a full-screen draw that ignores glColorMask: the renderer's own masked quad is right (and Vita3K does not run vitaGL's clear at all)
    app::config_default("DATA_DIR", "ux0:data/infected/");   // settings.ini / settings.txt and the framework's user data directory
    app::config_default("STALL_WATCHDOG", "0");          // the watchdog needs a host thread; the status line shows a stall as flips=+0
    // vitaGL patch 0003: GXM program, texture and stream calls that set what the draw before set are left out. On the console
    // vitaGL's share of the GL thread went from 4.6-7.4 to 4.2 ms a frame with the same pictures (README-VITA.md).
    app::config_default("GL_VGL_SKIP_REDUNDANT", "1");
    // Transformed draws are written without the vertex fields their program never reads (36 bytes lit, 24 unlit, instead of
    // 40): 15 % fewer vertex and index bytes a frame in a mission, identical pictures on the PC replay. Not yet timed on the
    // console; gl_compact_vertices=0 in config.txt turns it off. The renderer's own default stays off (desktop builds).
    app::config_default("GL_COMPACT_VERTICES", "1");
}

plat::GlParams gl_params_from_config() {
    plat::GlParams p;
    p.width = 960;
    p.height = 544;
    p.msaa = app::settings::msaa_samples();     // gl_msaa= in config.txt, else gfx.antialiasing (restart needed)
    p.vsync = app::settings::vsync();           // gl_vsync= in config.txt, else perf.frame_cap (off = not synchronised)
    p.display_buffers = (int)app::cfg_int("gl_buffers", 3);
    p.ram_pool_mib = (int)app::cfg_int("gl_ram_pool_mib", 24);
    p.cdram_pool_mib = (int)app::cfg_int("gl_cdram_pool_mib", -1);
    p.circular_pool_mib = (int)app::cfg_int("gl_circular_pool_mib", 16);
    p.vdm_ring_kib = (int)app::cfg_int("gl_vdm_kib", 0);
    p.vertex_ring_kib = (int)app::cfg_int("gl_vertex_kib", 0);
    p.fragment_ring_kib = (int)app::cfg_int("gl_fragment_kib", 0);
    p.usse_ring_kib = (int)app::cfg_int("gl_usse_kib", 0);
    p.shark_opt_level = (int)app::cfg_int("gl_shark_opt", 2);
    p.rt_scenes_display = (int)app::cfg_int("gl_rt_scenes", 4);                  // 0 = vitaGL's default (1)
    p.rt_scenes_fbo = (int)app::cfg_int("gl_rt_scenes_fbo", p.rt_scenes_display);
    p.shader_cache_dir = "shader_cache";
    return p;
}

bool is_normal_stop(const std::string& reason) {
    if (reason.empty()) return true;
    return reason.find("limit") != std::string::npos || reason.find("quit") != std::string::npos ||
           reason.find("MAX_FRAMES") != std::string::npos || reason.find("bench complete") != std::string::npos || reason.find("window closed") != std::string::npos;
}

}  // namespace

namespace {
struct GuestRun {
    std::string error;
};
// The guest executor: the kernel's scheduler on its own thread (core 0). An exception ends the run; main() reports it.
void guest_thread_main(void* arg) {
    GuestRun* run = static_cast<GuestRun*>(arg);
    try {
        portablekit::kernel().run_main();
    } catch (const std::exception& e) {
        run->error = std::string("exception: ") + e.what();
        portablekit::kernel().runtime().stop(run->error);
    }
}
// What the primitives the threads lean on cost on this console, measured once at start (config cost_probe=0: off). Each
// one is a system call or a lock on a hot path (a clock read per timed scope, a signal per queued job), so the numbers
// say what a per-draw use of them costs. A few milliseconds in all.
thread_local int t_probe = 0;
void log_platform_costs() {
    constexpr int kN = 4000;
    const auto per_call_us = [](uint64_t start, int n) { return (double)(plat::now_us() - start) / n; };
    uint64_t t = plat::now_us();
    volatile uint64_t sink = 0;
    for (int i = 0; i < kN; i++) sink = sink + plat::now_us();
    const double clock_us = per_call_us(t, kN);
    plat::Event* e = plat::event_create(/*auto_reset=*/true, /*initially_signalled=*/false);
    t = plat::now_us();
    for (int i = 0; i < kN; i++) plat::event_signal(e);
    const double signal_us = per_call_us(t, kN);
    t = plat::now_us();
    for (int i = 0; i < kN; i++) (void)plat::event_wait(e, 0);
    const double poll_us = per_call_us(t, kN);
    plat::event_destroy(e);
    std::mutex m;
    t = plat::now_us();
    for (int i = 0; i < kN; i++) {
        m.lock();
        m.unlock();
    }
    const double mutex_us = per_call_us(t, kN);
    t = plat::now_us();
    for (int i = 0; i < kN * 10; i++) t_probe = t_probe + 1;
    const double tls_ns = per_call_us(t, kN * 10) * 1000.0;
    (void)sink;
    plat::logf("costs: clock read %.2f us | event signal %.2f us, poll %.2f us | std::mutex lock+unlock %.2f us | thread_local "
               "access %.0f ns",
               clock_us, signal_us, poll_us, mutex_us, tls_ns);
}
}  // namespace

int main(int, char**) {
    plat::InitParams ip;
    ip.set_max_clocks = false;      // after config.txt was read
    plat::init(ip);
    plat::logf("Infected for PS Vita | built " __DATE__ " " __TIME__ " | title %s | newlib heap %u MiB", INFECTED_VITA_TITLEID,
               (unsigned)NEWLIB_HEAP_MIB);

    app::config_load("config.txt");
    apply_config_defaults();
    if (app::cfg_bool("log_sync", false)) plat::log_set_sync(true);
    app::log_redirect_streams();
    app::install_terminate_handler();
    plat::logf("config:\n%s", app::config_dump().c_str());
    app::settings::init();   // settings.txt (the in-game menu's file) before vitaGL: the multisampling is read from it
    // The fourth core, when a plugin lets the application have it: the helper threads (audio pump, movie decoder, GC thread of vitaGL,
    // watchdog) go there and leave core 2 to the display-list walker. config.txt core3=0 keeps them where they were.
    plat::core3_allow(app::cfg_bool("core3", true));
    plat::logf("cores: helper threads on core %d (core 3 %s)", plat::helper_core(), plat::core3_usable() ? "usable" : "not available");

    if (app::cfg_bool("clocks", true)) plat::clocks_set_max();
    if (app::cfg_bool("cost_probe", true)) log_platform_costs();
    {
        // Compiled shaders shipped in the VPK (app0:shader_cache) are copied beside the ones this device compiled; none is overwritten.
        const int shipped = plat::dir_copy_tree("shader_cache", "shader_cache", false);
        plat::logf("shader cache: %d shipped file(s) copied to the data folder", shipped < 0 ? 0 : shipped);
    }
    plat::logf("%s", app::memory_line().c_str());

    // ---- the screen first, so every later failure can be shown
    const bool gl_ok = plat::gl_init(gl_params_from_config());
    if (!gl_ok) plat::logf("vitaGL did not start; nothing can be shown on screen");
    app::ui::init();
    app::ui::boot_title("Infected  (PS Vita port)");
    app::ui::boot_line("Starting...");
    app::install_retroflow_art();
    if (!plat::gl_shader_compiler_available())
        app::ui::boot_line("libshacccg.suprx not found in ur0:data/ - shaders can only come from the shader cache");
    app::ui::boot_line("%s", app::memory_line().c_str() + 5);

    const std::string mode = app::bench_mode();
    if (mode == "ge_replay") {
        const int rc = app::bench_ge_replay();
        plat::shutdown();
        plat::exit_process(rc);
    }
    if (mode == "cpu") app::bench_cpu_setup();
    if (mode == "play") app::bench_play_setup();

    // ---- the game data
    const std::string disc_name = app::find_disc_name();   // Infected.iso, or the one .iso that looks like it
    const app::IsoCheck iso = app::iso_check(disc_name.c_str());
    if (iso.status != app::IsoStatus::Ok) {
        app::ui::boot_line("Game data problem: %s", iso.message.c_str());
        app::fatal("Game data not usable", iso.message);
    }

    try {
        // ---- guest memory, runtime, executable
        app::ui::boot_line("Allocating the 66 MiB guest arena");
        psprecomp::ArenaProvider provider;
        provider.allocate = &app::arena_allocate;
        provider.release = &app::arena_release;
        psprecomp::set_arena_provider(provider);

        psprecomp::Elf32Image elf = psprecomp::Elf32Image::from_bytes(iso.boot_bin, "disc0:/PSP_GAME/SYSDIR/BOOT.BIN");
        const std::uint32_t load_base = portablekit::game().load_base;
        const std::uint32_t ram_bytes = portablekit::game().guest_ram_bytes;
        if (elf.required_ram_size(load_base) > ram_bytes) throw psprecomp::Error("the executable needs more guest memory than the profile declares");

        psprecomp::Runtime runtime(ram_bytes);
        for (const EmbeddedNid& entry : kEmbeddedNids) runtime.nids().add(entry.library, entry.nid, entry.name);
        (void)elf.load_and_relocate(runtime.memory(), load_base);
        psprecomp::register_generated_functions(runtime);   // no AOT corpus on the Vita: the lean library registers through g_lean_entry
        app::ui::boot_line("Executable loaded: entry 0x%08X, %lu recompiled functions", elf.runtime_entry(load_base), (unsigned long)runtime.function_count());

        // ---- platform seams, scripted input, then the system (HLE, kernel, renderer)
        app::install_platform_hooks();
        app::pad_script_setup();
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress"
        if (lean_init_hooks_from_env != nullptr) lean_init_hooks_from_env();
#pragma GCC diagnostic pop
        plat::dir_make(plat::Root::Data, "ms0");
        portablekit::ProfilePaths paths;
        char disc[256];
        plat::path_resolve(plat::Root::Data, disc_name.c_str(), disc, sizeof(disc));
        paths.disc_image = disc;
        char ms0[256];
        plat::path_resolve(plat::Root::Data, "ms0", ms0, sizeof(ms0));
        paths.memory_stick = ms0;

        app::ui::boot_line("Installing the system (kernel, HLE, renderer)");
        portablekit::install_system(runtime, elf, paths);
        app::status::set_runtime(&runtime);
        app::status::install();
        if (mode == "play") app::bench_play_start();
        {
            static bool first_flip_seen = false;
            portablekit::kernel().add_vblank_hook([] {
                if (!first_flip_seen && portablekit::bringup::flip_count() > 0) {
                    first_flip_seen = true;
                    app::ui::boot_hide();
                    plat::logf("first guest flip at %llu ms", (unsigned long long)(plat::now_us() / 1000));
                    plat::logf("%s", app::memory_line().c_str());
                }
            });
        }
        app::settings::install_appliers();   // perf.cpu_clock, display sync; the touch corner opens the menu
        app::ui::boot_line("Starting the game (%s)", portablekit::kernel().realtime() ? "realtime clock" : "virtual clock");
        plat::logf("%s", app::memory_line().c_str());
        plat::log_flush();

        // ---- run: the guest on core 0, the GE list walk + GL submission on core 1 (this thread owns the GL context);
        // perf.threaded_renderer=off (settings.txt) or threaded_renderer=0 (config.txt) runs everything on this thread as before.
        const bool want_threaded = portablekit::gpu::ge_worker_setting_enabled() && mode != "ge_replay";
        bool ran_threaded = false;
        if (want_threaded) {
            GuestRun guest{};
            if (portablekit::gpu::SplitRenderer* split = portablekit::gpu::split_of(portablekit::media_renderer())) {
                // config.txt ge_split=1: the list walk and vertex decode on core 2, GL submission on core 1 (this thread).
                plat::logf("threaded renderer: guest executor on core 0, GE list walker on core 2, GL thread on core 1");
                ran_threaded = split->run(portablekit::gpu::ge_worker(), &guest_thread_main, &guest, 0, 1024 * 1024, 2, 1024 * 1024, 1);
            } else {
                plat::logf("threaded renderer: guest executor on core 0, GE thread on core 1");
                ran_threaded = portablekit::gpu::ge_worker().run_with_guest_thread(&guest_thread_main, &guest, 0, 1024 * 1024, 1);
            }
            if (ran_threaded && !guest.error.empty()) throw std::runtime_error(guest.error);
            if (!ran_threaded) plat::logf("threaded renderer: the guest thread could not be started; running on this thread");
        } else {
            plat::logf("threaded renderer: off");
        }
        if (!ran_threaded) portablekit::kernel().run_main();
        const std::string reason = runtime.stop_reason();
        plat::logf("runtime stopped: %s", reason.empty() ? "(no reason)" : reason.c_str());
        app::status::report("final");
        plat::logf("%s", app::memory_line().c_str());
        if (mode == "cpu") app::bench_cpu_report(reason.c_str());
        if (!is_normal_stop(reason)) app::fatal("The game stopped", reason, true, g_cpu.pc);
    } catch (const std::exception& e) {
        app::fatal("Infected stopped", std::string("exception: ") + e.what(), true, g_cpu.pc);
    }

    app::status::shutdown();
    app::shutdown_platform_hooks();
    plat::logf("clean exit");
    plat::shutdown();
    plat::exit_process(0);
}
