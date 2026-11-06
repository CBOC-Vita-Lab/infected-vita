#include "app_settings.h"

#include <cstdint>

#include "app_config.h"
#include "platform.h"

#include "gpu/hd_texture_pack.hpp"
#include "kernel/kernel.hpp"
#include "overlay/overlay_host.hpp"
#include "settings/port_settings.hpp"

namespace app::settings {
namespace {

namespace port = portablekit::port;

std::uint32_t g_applied_generation = 0;
int g_applied_cpu_mhz = 0;       // the clock the settings asked for last (0 = nothing yet)
int g_applied_vsync = -1;

// The constructor greys the HD row ("No texture pack installed.") and the renderer lifts it when it first looks for the pack, which is
// after the settings list is logged and after the first menu frame: look now, so the log and the menu are right from the start.
void lift_hd_reason() {
    port::Settings& s = port::settings();
    if (s.unavailable_reason(port::Key::GfxHdTextures) != nullptr && portablekit::gpu::hd_pack_present(portablekit::gpu::hd_data_root()))
        s.set_unavailable(port::Key::GfxHdTextures, nullptr);
}

void log_non_default() {
    lift_hd_reason();
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
    // perf.cpu_clock: 444 (the retail maximum) or 500. 500 MHz comes from a PSVshellPlus profile for this title (ur0:data/PSVshell/profiles/INFC00001)
    // that the game keeps in place: PSVshellPlus applies a profile when the app becomes active, so a profile written now acts from the next start.
    // The system's user-mode setter stays at 444 either way, so the MEASURED clock decides, and the menu's row shows it (port::set_measured_cpu_clock).
    // config.txt's clocks=0 keeps the application away from the clocks (and the profile) altogether.
    if (app::cfg_bool("clocks", true)) {
        const int want = (int)port::cpu_clock_mhz();
        if (want != g_applied_cpu_mhz) {
            g_applied_cpu_mhz = want;
            const bool installed = plat::psvshell_installed();
            int written = 0;
            if (installed) written = plat::psvshell_profile_set(want);
            const int measured = plat::clock_measure_mhz();
            port::set_measured_cpu_clock(measured);
            port::set_cpu_clock_reason(nullptr, nullptr);
            if (want != 444 && measured >= want - want / 20) {
                plat::logf("settings: the ARM clock runs at %d MHz (measured; PSVshellPlus profile)", measured);
            } else if (want != 444 && !installed) {
                port::set_cpu_clock_reason("500 needs PSVshellPlus", "500 MHz needs PSVshellPlus (see INSTALL.md). The game stays at 444 MHz.");
                plat::logf("settings: the ARM clock stays at %d MHz measured: 500 MHz needs PSVshellPlus, which is not installed", measured);
            } else if (want != 444 && written == 1) {
                port::set_cpu_clock_reason("500 from the next start", "500 MHz from the next start (PSVshellPlus applies its profile when the game starts).");
                plat::logf("settings: 500 MHz from the next start (the PSVshellPlus profile was written; the clock measures %d MHz now)", measured);
            } else if (want != 444 && written == -1) {
                port::set_cpu_clock_reason("your PSVshell profile", "The game found a PSVshellPlus profile of your own for it and left it alone.");
                plat::logf("settings: the ARM clock stays at %d MHz measured: the PSVshellPlus profile for this game is the player's own", measured);
            } else if (want != 444) {
                plat::logf("settings: the ARM clock stays at %d MHz measured (PSVshellPlus profile: %s)", measured, written < 0 ? "could not be written" : "in place, applied at the next start");
            } else if (written == 1) {
                plat::logf("settings: 444 MHz from the next start (the PSVshellPlus profile was changed to 444; the clock measures %d MHz now)", measured);
            } else {
                plat::logf("settings: the ARM clock is %d MHz (measured)", measured);
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
        lift_hd_reason();
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
