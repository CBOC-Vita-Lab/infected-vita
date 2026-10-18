// Gameplay tuning for the Infected port: more enemies on screen (perf.enemy_density) and a longer view distance
// (perf.view_distance). Findings and addresses: docs/GAMEPLAY_TUNING.md. Addresses are for the relocated US 1.02 image.
#pragma once

#include "settings/port_settings.hpp"

#include <cstdint>

namespace psprecomp {
class Runtime;
}

namespace infected {

// The ONE place the two settings are read: perf.enemy_density and perf.view_distance from the port settings (host/settings/port_settings.hpp:
// the developer override, settings.txt written by the overlay, the default), as percentages. `fallback` is returned for any other key.
int tuning_setting(portablekit::port::Key key, int fallback);

// Called from GameProfile::patch_loaded_image (once, executable in memory, before it runs).
void tuning_install(psprecomp::Runtime &runtime);
// Called from GameProfile::frame_hook (once per guest flip).
void tuning_frame(psprecomp::Runtime &runtime, std::uint64_t flip);
// A platform application may ask to be told of every flip (on the guest thread, at the start of tuning_frame, before the flip's pacing hold):
// the Vita's bench=play report times the flips with it. One observer; nullptr removes it.
void set_flip_observer(void (*observer)(std::uint64_t flip));

} // namespace infected
