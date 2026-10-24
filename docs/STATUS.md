# Status

What runs, where it was checked, and what has not been checked. Dates are 2026. Numbers from the console are read from the game's own
`log.txt` (the `[stat]` and `[play]` lines) on the owner's Vita; nothing here is an estimate unless it says so.

## In one paragraph

The game boots on a real PS Vita, plays through its logos, the intro movie, the menus, the tutorial and **mission 1**, and holds
**30 frames a second** there (the game's own rate on a PSP) with a worst 1 % of about 15 fps. The 2x HD texture pack works. Mission 1 is the only
mission that has been played on the console. Later missions, the 4x pack, long sessions and the experimental HD movies have not.

## On the console

| What | Result | Checked |
| --- | --- | --- |
| Boot, logos, intro movie, menus, mission 1 | Runs. The 30 fps limit and the 2x HD texture pack work. | 10 to 17 Oct |
| Speed in mission 1 (defaults, 2x pack, 444 MHz) | 30 fps with the 30 fps limit on; uncapped 30 to 32 fps on average, dips to about 27 in heavy moments, worst 1 % about 15 fps. On the morning of 16 Oct the first console build ran mission 1 at 16.6 fps; the threading work took it to 31.6 uncapped by the evening. | 16 Oct |
| 500 MHz | Needs a PSVshellPlus profile (`vita/psvshell`). With it the game measures 494 MHz. | 17 Oct |
| Where a mission frame goes (uncapped, about 31 fps) | game code 23 ms (core 0), display-list walk and vertex decode 22 ms (core 2), GL thread 25 to 29 ms (core 1) and the bottleneck at 85 to 90 % busy, GPU about 15 to 19 ms | 16 Oct |
| Ceiling | with no draw calls at all (`gl_null_draw=1`) the game and the walker alone reach 36 fps, so 60 fps is out of reach without the game code itself getting much faster | 16 Oct |
| Changes of 17 Oct (vitaGL speed options, four scenes per frame, compact vertices, HUD upscale, idle-time shader linking, faster texture decoding, 500 MHz default) | Installed. A 46 second start-up run was clean (intro at 30 fps, 494 MHz, no fatal lines). **Not measured in a mission yet**, so none of their gains is a result. | 17 Oct |
| Changes of 23 Oct (the fourth CPU core unlocked by the game itself, the shader program list shipped in the VPK, a faster strip conversion on the display-list thread, vitaGL patch 0005; the game's code linked with LTO, the music decoded ahead on the helper core, fully transparent draws skipped) | **Played on a 3.74 console.** Mission 1 on the defaults, with three cores: 30 pictures shown in every second; game code 13 to 20 ms a frame (16 Oct: 23 ms). The fourth core (after a fix the same evening): the module loads, a thread runs on core 3, the helper threads and the music decoder move there. 47 shader programs prepared during the logos in 1.5 s. With FXAA + pixel lighting + double-resolution effects + the 4x pack together: 10 to 14 fps (the GPU cannot keep up). Worst-1 % frames not measured yet. | 23 Oct |

`docs/history/` has the working logs behind these numbers, including what was tried and dropped.

## What was found along the way (applies to any vitasdk port)

* The kernel checks the stack pointer of a thread against the stack bounds in its thread-local block; code that runs on its own stacks has to
  open the bounds at every switch (PortableKit patch "open the PS Vita's per-thread stack bounds").
* A static link of the C++ runtime leaves `pthread_cancel`, `pthread_once` and friends as weak undefined symbols: `std::mutex` then locks nothing
  and `thread_local` is one variable for all threads. The link line needs `-u pthread_cancel -u pthread_once ...`, and the build checks
  `nm | grep ' w pthread_'` (`vita/VITAGL_NOTES.md`, section 4).
* System calls are the real cost on the Vita's CPU (an event-flag signal 3.9 us, a clock read 0.7 us): signalling only a thread that may be
  asleep took a mission from 25 to 30 fps by itself.
* Reading a texture-pack blob from the memory card cost 5 to 60 ms on the GL thread; a loader thread removed those hitches.

## On the PC (development builds)

* The recompiler's output is compared with PortableKit's interpreter: 57,462 random trials and a replay of 10,562 snapshots from the
  real game, without a mismatch after the last fixes (`docs/history/LEAN_LOG.md`).
* A recorded win of mission 1 replays to the same score every time. At the 10 Oct release candidate 341 recorded drawing frames were
  pixel-identical to the baseline at the original settings, plain and threaded; that set has not been re-baselined since (a later
  replay check against the 10 Oct baseline reported a mismatch that has not been investigated).
* Framework tests: 45 of 45 pass on the desktop (one timing-sensitive test can flake on a busy machine).
* A bot plays mission 1 to the end (and mission 2 on the PC) through the game's own input path; mission 3 needs the subway and the
  bot cannot do that.

## Open work

See [`KNOWN-ISSUES.md`](../KNOWN-ISSUES.md): the intro movie's tear lines, banding in fire, hitches from render-target switches, the shader
cache, and everything above that has not been tried on the console.
