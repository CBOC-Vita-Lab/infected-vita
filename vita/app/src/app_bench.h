// app_bench.h - benchmark modes, selected by bench=<mode> in config.txt (see README-VITA.md):
//
//   bench=ge_replay   Replays the GE dumps in ux0:data/infected/dumps/ (recorded on the desktop with INFECTED_GE_DUMP=<dir>) in a loop
//                     through GeState and the GLES2 renderer on this device, with no guest code. Logs ms/frame for the display-list walk
//                     plus vertex decode, GL submission, the present/swap and the whole flip, once per `bench_report` flips
//                     (default 60) and at the end. bench_flips=N stops after N replayed flips (default: loop until START is pressed),
//                     bench_dump=<dir> picks another directory below the data root.
//   bench=cpu         Runs the game normally with the framework's bench (INFECTED_BENCH=flips:N,from:F): bench_flips=N flips (default 3600)
//                     from flip bench_from=F (default 0), timed by category (guest code, GE walk, vertex decode, GL submission, present, idle,
//                     kernel, audio, movie decode, HLE per library) and calls per flip. The report is printed to log.txt and the run stops.
//                     The threaded renderer is off while it runs (the accounting is per thread).
//   bench=play        The play-through bot (config.txt `bot`, on by default in this mode) plays a mission with everything as the player has
//                     it (real clock, threaded renderer, settings.txt). From the first flip of the running mission, a "[play]" line every
//                     bench_interval seconds (default 10) and a FINAL line at the end: fps average and 1 % low, ms per frame in guest code,
//                     GE thread busy %, swap ms, draws per frame, infected alive, the settings in effect. The run ends when the mission is
//                     over or bench_seconds (default 420) after it began, and the game exits.

#pragma once

#include <string>

namespace app {

// "ge_replay", "cpu", "play" or "" (no benchmark).
std::string bench_mode();
// Runs the GE replay benchmark; returns the process exit code. GL must be initialised and the boot screen may be showing.
int bench_ge_replay();
// Exports the run limit of bench=cpu (INFECTED_MAX_FRAMES) before the system is installed.
void bench_cpu_setup();
// Called at the end of a bench=cpu run: logs the summary line.
void bench_cpu_report(const char* stop_reason);
// bench=play: switches the bot on (before the system is installed) ...
void bench_play_setup();
// ... and starts the [play] report (after the system is installed).
void bench_play_start();

}  // namespace app
