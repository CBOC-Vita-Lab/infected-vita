// app_settings.h - the port's user settings (settings.txt, the in-game menu) on the Vita.
//
//   init()           before vitaGL starts: settings.txt is read from the data root, the developer overrides are applied
//                    (config.txt keys gfx.* / perf.*), the restart-needed values are marked. vitaGL reads the multisampling from it.
//   msaa_samples()   0, 2 or 4: gl_msaa= in config.txt if present, else gfx.antialiasing as the program started with it
//   vsync()          vsync of the display, from perf.frame_cap (off = not synchronised) unless gl_vsync= says otherwise
//   install_appliers()  kernel vblank hook: perf.cpu_clock and the vsync follow the settings when they change; the touch
//                    screen's top-right corner opens the menu
//   poll_touch()     called with every pad read of the guest
#pragma once

namespace app::settings {

void init();
int msaa_samples();
bool vsync();
void install_appliers();
void poll_touch();

}  // namespace app::settings
