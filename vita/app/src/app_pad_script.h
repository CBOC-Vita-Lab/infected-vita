// app_pad_script.h - scripted input for unattended runs (Vita3K, automated tests).
//
// The runtime already has a scripted pad: INFECTED_PAD_SCRIPT=<at>:<buttons>,... where <at> is a flip number (or Nms / Ns of emulated
// time) and <buttons> names joined by + (CROSS, START, ...) or '-' for none; a step holds until the next one. See
// PortableKit/host/perf/bringup_stats.hpp. That variable is a string, and the example scripts are 8 KB long, so on the Vita the
// script comes from a file:
//     pad_script_file=pad.script        (relative to ux0:data/infected/, same flip-keyed format, newlines and '#' lines allowed)
// or inline as pad_script=150:CROSS,156:-.
#pragma once

namespace app {
// Reads pad_script_file and exports the result as INFECTED_PAD_SCRIPT. Call after config_load(), before install_system().
void pad_script_setup();
}  // namespace app
