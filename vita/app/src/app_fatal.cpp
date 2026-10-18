#include "app_fatal.h"

#include <exception>
#include <stdio.h>

#include "app_config.h"
#include "app_ui.h"
#include "platform.h"

namespace app {
namespace {
bool g_in_fatal = false;
}

void fatal(const char* title, const std::string& message, bool has_guest_pc, uint32_t guest_pc) {
    if (g_in_fatal) plat::exit_process(3);   // a fault while reporting a fault: leave
    g_in_fatal = true;
    std::string body = message;
    if (has_guest_pc) {
        char pc[64];
        snprintf(pc, sizeof(pc), "\n\nGuest PC: 0x%08X", (unsigned)guest_pc);
        body += pc;
    }
    plat::logf("FATAL: %s: %s", title, body.c_str());
    plat::log_set_sync(true);
    plat::log_flush();

    const bool wait = cfg_bool("fatal_wait", true);
    if (!plat::gl_is_ready()) {
        // Failure before (or in) the GL start-up: try once to get a screen.
        plat::GlParams p;
        if (plat::gl_init(p)) ui::init();
    }
    if (plat::gl_is_ready()) {
        ui::message_show(title, body.c_str(), wait ? "Press START to exit.  The details are in ux0:data/infected/log.txt" : "Exiting.");
        const uint64_t t0 = plat::now_us();
        for (;;) {
            ui::present();
            plat::PadState pad;
            if (plat::pad_poll(&pad) && (pad.buttons & plat::PAD_START) && wait) break;
            if (!wait && plat::now_us() - t0 > 3000000ull) break;
        }
    } else {
        plat::logf("FATAL: no screen available; exiting");
    }
    plat::shutdown();
    plat::exit_process(2);
}

void install_terminate_handler() {
    std::set_terminate([] {
        std::string what = "uncaught C++ exception or std::terminate";
        if (std::exception_ptr ep = std::current_exception()) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                what = std::string("uncaught exception: ") + e.what();
            } catch (...) {
                what = "uncaught exception of unknown type";
            }
        }
        fatal("Infected stopped", what);
    });
}

}  // namespace app
