#include "app_settings.h"

#include <cstdint>

#include "app_config.h"
#include "platform.h"

#include "kernel/kernel.hpp"
#include "overlay/overlay_host.hpp"
#include "settings/port_settings.hpp"

namespace app::settings {
namespace {

namespace port = portablekit::port;

std::uint32_t g_applied_generation = 0;
int g_applied_cpu_mhz = 0;       // the clock the settings asked for last (0 = nothing yet)
int g_applied_vsync = -1;

void log_non_default() {
    port::Settings& s = port::settings();
    plat::logf("settings: %s (%s)", s.path().empty() ? "(no file)" : s.path().c_str(), s.dirty() ? "changed" : "as stored");
    for (std::size_t i = 0; i < port::kKeyCount; ++i) {
        const port::Key key = static_cast<port::Key>(i);
        const char* reason = s.unavailable_reason(key);
        plat::logf("settings:   %-22s = %-9s%s%s%s%s%s", port::info(key).name, s.value(key), s.is_default(key) ? "" : "  (not the default)",
                   s.overridden(key) ? "  (config.txt override)" : "", reason ? "  [greyed: " : "", reason ? reason : "", reason ? "]" : "");
    }
}

void apply() {
    port::Settings& s = port::settings();
    // perf.cpu_clock: 444 (the retail maximum) or 500 (needs a plugin that lifts the limit; the setter's answer is logged).
    // config.txt's clocks=0 keeps the application away from the clocks altogether.
    if (app::cfg_bool("clocks", true)) {
        const int want = (int)port::cpu_clock_mhz();
        if (want != g_applied_cpu_mhz) {
            g_applied_cpu_mhz = want;
            if (want != 444) {
                int applied = 0;
                const int rc = plat::clock_set_arm(want, &applied);
                // With a PSVshellPlus profile for this title (INFC00001) the clock runs at 500 while the system still reports 444 and
                // the game's own requests are ignored: the measured clock decides, not the setter's answer.
                const int measured = plat::clock_measure_mhz();
                if (measured >= want - want / 20) {
                    plat::logf("settings: the ARM clock runs at %d MHz (measured; the system reports %d)", measured, applied);
                } else if (rc < 0 || applied != want) {
                    plat::logf("settings: the ARM clock stays at %d MHz (%d MHz was refused by the system)", applied, want);
                    if (applied != 0 && applied < want) (void)plat::clock_set_arm(444, &applied);
                }
            } else if (g_applied_generation != 0) {
                int applied = 0;
                (void)plat::clock_set_arm(444, &applied);   // back from 500
            }
        }
    }
    const int vs = vsync() ? 1 : 0;
    if (vs != g_applied_vsync) {
        g_applied_vsync = vs;
        plat::gl_set_vsync(vs != 0);
        plat::logf("settings: display sync %s (perf.frame_cap=%s)", vs ? "on" : "off", s.value(port::Key::PerfFrameCap));
    }
    g_applied_generation = s.generation();
}

}  // namespace

void init() {
    // The Vita's own defaults (one list for the app and the replayer, PortableKit's apply_vita_defaults): the renderer thread split in
    // two (mission 1: 29-31 fps against 25 with one renderer thread), 30 frames a second (the game's own rate; a steady 30 looks smoother
    // than 27-34), the CPU at 500 MHz (12 % more cycles for all three threads; the app keeps 444 when the system refuses, and the menu's
    // "CPU clock" row goes back to 444 for a cooler console) and anti-aliasing Auto (FXAA while the renderer has room for it).
    // A settings.txt that names a value keeps it.
    port::apply_vita_defaults(port::settings());
    portablekit::overlay_host::init_settings();   // the data root comes from INFECTED_DATA_DIR (ux0:data/infected/)
    // The menu writes every row to settings.txt, so a console that has used the menu holds perf.cpu_clock=444, the old default,
    // and 444 would win over the new default for good. Once (the marker file records it), that 444 becomes 500; a 444 chosen
    // afterwards in the menu stays. A config.txt override is left alone.
    if (!plat::file_exists(plat::Root::Data, "cpu_clock_500.done")) {
        port::Settings& s = port::settings();
        if (!s.overridden(port::Key::PerfCpuClock) && s.index(port::Key::PerfCpuClock) == 0u) {
            s.set(port::Key::PerfCpuClock, "500");
            const bool saved = s.save();
            plat::logf("settings: perf.cpu_clock 444 in settings.txt was the old default; it is now 500 MHz (%s)",
                       saved ? "saved" : "NOT saved");
        }
        plat::file_write_all(plat::Root::Data, "cpu_clock_500.done", "1\n", 2);
    }
    // Anti-aliasing Auto is new in 1.1. The menu of 1.0 wrote every row to settings.txt, so a console that has used the menu holds
    // gfx.antialiasing=off, the old default, and Off would win over Auto for good. Once (the marker file records it): a settings.txt
    // in which every other row is at its default has no choice in it, and its Off becomes Auto; a file in which the player changed
    // anything else keeps its Off (it may be a choice), and so does a real FXAA or MSAA choice. A config.txt override is left alone.
    if (!plat::file_exists(plat::Root::Data, "aa_auto.done")) {
        port::Settings& s = port::settings();
        if (s.adopt_auto_antialiasing()) {
            const bool saved = s.save();
            plat::logf("settings: gfx.antialiasing off in settings.txt was the old default; it is now Auto (%s)", saved ? "saved" : "NOT saved");
        }
        plat::file_write_all(plat::Root::Data, "aa_auto.done", "1\n", 2);
    }
    log_non_default();
}

int msaa_samples() {
    if (app::cfg_get("gl_msaa") != nullptr) return (int)app::cfg_int("gl_msaa", 0);
    return (int)port::msaa_samples();
}

bool vsync() {
    if (app::cfg_get("gl_vsync") != nullptr) return app::cfg_bool("gl_vsync", true);
    return port::frame_cap() != port::FrameCap::Off;
}

void install_appliers() {
    {
        // The list printed by init() comes before the renderer has looked for the HD packs: say what the menu will show.
        port::Settings& s = port::settings();
        const char* reason = s.unavailable_reason(port::Key::GfxHdTextures);
        plat::logf("settings: gfx.hd_textures = %s%s%s%s", s.value(port::Key::GfxHdTextures), reason ? "  [greyed: " : "  [usable]",
                   reason ? reason : "", reason ? "]" : "");
    }
    apply();   // the first start: the clock the player chose
    portablekit::kernel().add_vblank_hook([] {
        if (port::settings().generation() != g_applied_generation) apply();
    });
}

void poll_touch() {
    if (plat::touch_corner_tap() && !portablekit::overlay_host::menu().is_open()) portablekit::overlay_host::menu().request_open();
}

}  // namespace app::settings
