# Coroutine kernel log

Workspace: /work/ws/kernel (PortableKit branch coroutine-kernel, proj branch coroutine-kernel, build/ , private/ not committed).

- Baseline: PortableKit infected-bringup builds; ctest in build/portablekit_fw: 20/21 (portablekit_adhoc_tests "the status lists players and traffic" fails at baseline, unrelated).
- Old-kernel instrumented binary (bin/InfectedNative.old_instr, patch scratch/old_instr.patch: starvation counters + TRACE_SWITCH) used for the audit and hang repro.
- M1 (committed): coroutine.{hpp,cpp} + test (x86-64 passes; ARM asm assembled for thumb/arm with vitasdk and disassembled, not run), arena-backed GuestMemory, exec layer, interpret_guest_call, region histogram.
- M2 (committed 5c1cc74): coroutine kernel (kernel.cpp rewritten), KernelPlatform, pk_runtime static lib, tests/kernel_tests.cpp (9 scenarios, one process each). ctest 31/31.
  pk_runtime links with only the stand-in profile (kernel_tests, io_filter_tests); desktop-only sources are PORTABLEKIT_DESKTOP_SOURCES.
- Vita compile check (vita_check/check.sh, arm-vita-eabi-g++ 15.2 -std=gnu++20 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard): 67 of 72 objects compile
  (core 12/12). Not compiling: corpus_library.cpp + overlays.cpp (dlfcn.h), game_font.cpp (sys/mman.h), adhoc/discovery.cpp (ifaddrs.h), adhoc/server.cpp (in6addr_any).
- Pad scripts: to_mission.script is keyed to emulated time and the existing-save flow differs between runs; tools/continue_flips.script presses CROSS every 100 flips (flip-keyed)
  and plays the movies; run_flips.sh uses it on ms0.pristine. Fresh (empty ms0) = name entry flow, script times do not fit.
- Movie hang reproduced on the new kernel: INFECTED_LIBC_HOOKS=1 + run_flips.sh: flips stop at 2657 (emu ~93 s), main in the drain loop 0x08899940 polling a software queue
  (via 0x08897A7C) with a vblank wait between polls; readThread and soundThread dormant. Without hooks the same movie ends normally.
- Movie hang ROOT CAUSE (found): game's frame ring (2 slots) full at EOS + A/V sync hold of the last picture; HLE released ring packets at demux time so
  sceMpegRingbufferAvailableSize==full (the game's EOS test) fired with 2 audio AUs still queued host-side -> audio clock stuck at 6358635 < last video pts-6006
  -> pop-stage hold -> drain loop (0x08897A7C returns 1 while the ring is full, nobody pops) forever. Phase dependent: CPU_NS_PER_INSN 0/1/4 hung, 8 passed.
  Fix (8fb284b): hold packs until their AUs are fetched (hle_mpeg.cpp). Off switch INFECTED_MPEG_EAGER_RELEASE=1 (reproduces the hang, checked).
  After fix: ns 0,1,4,8 all end both movies with hooks on (k5 binary).
- Hooks A/B (k6, ms0.pristine, continue_flips.script, shots every 120 flips): memcpy-only, memset-only, both: mission HUD background black like s17 in all of them
  (hk_memcpy, hk_memset2, hk_both, k6_a). One earlier both-hooks run (k5_a, shots every 240) showed a flat grey 3D-view background for two HUD shots; four reruns
  with the same settings did not reproduce it. Not investigated further (hooks are off by default; suspect a timing dependent load race in the game).
- Stall watchdog lines "no frame for 5 s after 443 frames" appear in old-kernel logs too (old_flips: 3/164/172/1403 frames): interpreter-bound loading, not a hang.
- s18 comparison (s18cmp, k6, hooks off, to_mission.script, ms0.pristine, ns=4, frame limit 16683, no shots): menu idle median 22 mean 20.7 per flip = s18 exactly;
  gameplay (f6000-f8800) median 32 mean 30.5-31.8, 16.68 ms/flip = s18 (32, 31.0-31.9). Movie phases differ run to run (17-280 ms per flip, 20-900 calls per flip):
  the host decode time is charged to the virtual clock as wall time, so it follows the machine load (the old kernel varied the same way: old_flips 267 ms/flip).
- Default-configuration runs (hooks off, virtual clock, busy clock on, ns=4, limit 16683, continue_flips.script, ms0.pristine, no shots; scratch/default_runs.sh):
  r1 (MAX_EMU 450), r2, r3 (300): exit=4 (emulated-time limit) each, 14.2K/9.9K/9.7K flips; all reach the mission (HUD from emu ~212-240 s; flip 3000-3400) and run gameplay
  at median 32 / mean 30.8-31.9 calls per flip, 16.68 ms/flip (s18: 32, 31.0-31.9); idle menu median 22 mean 20.1-20.8 (s18: 22, 20.7). The phase with the mission loading
  (f6200 in r1) is 47 / 41.2 / 111 ms vs s18 47 / 42.0 / 111. Stalls: none.
- to_mission.script (time-keyed) on ms0.pristine: s18cmp reached the mission with the same per-flip numbers; t1 (same settings, noise) ended in a menu at 260 s emu: the script presses
  at fixed emulated times and the movie phase length varies with the busy clock. INFECTED_NO_BUSY_CLOCK=1 removes the machine-load term.
- ctest build/portablekit_fw: 31/31 (including the adhoc test that failed at baseline). Vita check: 67/72 objects compile, same five desktop-only failures; kernel.cpp
  with -DPORTABLEKIT_PLATFORM_CUSTOM -DPORTABLEKIT_NO_HOST_THREADS compiles too.
- Shot comparison to s17 (scratch/match_shots.py, new runs k6_a, k6_b, hk_both, hk_memcpy): 7 of 8 s17 screens have a new-run counterpart with 88-99.9 % identical pixels
  (differences: timer digits, radar sweep, loading bar); the NYBT logo shot has none because the timelines differ.
- Grey HUD resolved: with INFECTED_NO_BUSY_CLOCK=1 (deterministic virtual clock) hooks off (det_off) and both hooks on (det_on) give pixel-identical shots at flips 2400-2880
  and 3240-3840, 4080, 4200 (99-100 %), and BOTH show the same grey, dimmed-HUD frame at flip 3960 (median 97 vs 98): a transient full-screen overlay of the game
  (a flash effect drawn by the debug rasteriser), not a hook defect. The earlier "grey in k5_a / rt2" shots were taken during that effect.
  Hooks vs no hooks are not step-identical (flip 4: emu 317071 vs 334755 us, 88 instructions apart: spin-counter accounting), but converge to the same screens.
- Realtime (rt1: hooks off with shots, rt2: hooks on): INFECTED_CLOCK=realtime runs the logos, both movies, menus, mission selection, briefing and mission HUD
  (rt2 reached the HUD at 4588 flips in 470 s wall; rt1 was still in the tutorial movie at 360 s because the interpreter + rasteriser run it at ~1/7 speed).
- kernel_tests 9-11 (committed): lean entry reached by jal and by call_guest, blocking inside the thunk (host stack survives), callback into the interpreter,
  INTERP_ONLY forcing the twin into the interpreter with an identical log, LEAN_ONLY stopping on the first interpreted entry. ctest 34/34 with -j1
  (portablekit_adhoc_tests "the status lists players and traffic" is timing flaky: fails 2 of 3 alone, the baseline failure).
