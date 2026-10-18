// app_status.h - the per-second status line in ux0:data/infected/log.txt (and the optional HUD).
//
//   [stat] t=61s flips=3571(+60) presented=60 frame=16.7ms ge=thr:41% | ms/frame guest=9.8 hle=1.9 gewalk=1.1 (vertex=N.N) glsubmit=2.4 present=3.0 decode=0.3 audio=0.1 io=0.0 idle=0.0 |
//          draws/f=612 gl_draws/f=480 | switches/s=2450 | audio q=3200 underruns=0(+0) | mem user_free=... heap=.../...
//
// "guest" is what is left of the frame after the other columns: the recompiled game code, the kernel and the HLE calls that are not
// timed (hle= is only filled with stats_hle=1 in config.txt, because it costs two clock reads per HLE call).
// ge=thr:41% is the share of the frame the GE thread (core 1) was busy with list walks, GL submission and the present; with the
// renderer on the guest thread (ge=inline:) those columns are part of the frame and are subtracted from guest=.
#pragma once

namespace app::status {

// The runtime whose HLE call histogram "hle_top=N" (config.txt) reports each second. Call before install().
void set_runtime(void* runtime);
// Registers the once-per-second reporter on the kernel's vblank hook. Call after install_system(); safe to call once.
void install();
// Stops the watchdog thread (call before the platform shuts down).
void shutdown();
// One status line now (also used at the end of the run).
void report(const char* why);
// bench=play: after the bot has a mission running, a [play] line every `every_seconds` (fps average and 1 % low, ms per frame in guest code, GE
// thread busy %, swap ms, draws per frame, infected alive, settings), a FINAL line, and the run stops `max_seconds` after the mission began or
// when it is over. Call before the game runs (the flip observer is registered here).
void play_enable(long max_seconds, long every_seconds);

}  // namespace app::status
