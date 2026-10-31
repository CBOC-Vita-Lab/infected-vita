# Status

What runs, where it was checked, and what has not been checked. Dates are 2026. Numbers from the console are read from the game's own
`log.txt` (the `[stat]` and `[play]` lines) on the owner's Vita; nothing here is an estimate unless it says so.

## In one paragraph

Version 1.0 boots on a real PS Vita, plays through its logos, the intro movie, the menus, the tutorial and **mission 1**, and shows
**30 frames a second** there on the default settings (the game's own rate on a PSP), using all four CPU cores. The 2x HD texture pack works.
Mission 1 is the only mission that has been played on the console. Later missions, the 4x pack, long sessions and the experimental HD
movies have not.

## Version 1.0 on the console (PS Vita, firmware 3.74, 23 October 2026)

| What | Result |
| --- | --- |
| Boot | Logos, intro movie and menus run. 47 shader programs are prepared during the logos (about 1.5 s on the first start). |
| Mission 1, default settings (2x pack, Split renderer, frame limit 30) | **30 pictures shown in every second** of the mission. |
| Where a mission frame goes | game code 13 to 20 ms (core 0), display-list walk and vertex decode 13 to 27 ms (core 2), drawing on core 1, which is the busiest; helper threads, audio and the music decoder on core 3 |
| Fourth core | The module in the package loads, a thread runs on core 3, the helper threads and the music decoder run there; unloaded at exit. |
| Defaults + FXAA | 21 to 30 fps |
| 4x pack + FXAA + pixel lighting + double-resolution effects | 10 to 14 fps: the GPU cannot keep up |
| 500 MHz | Needs a PSVshellPlus profile (`vita/psvshell`). With it the CPU measures about 494-497 MHz. |
| Ceiling | With no draw calls at all (`gl_null_draw=1`) the game and the display-list walk alone reach 36 fps, so 60 fps is out of reach; the game itself runs at 30. |
| Not measured yet | the worst frames (1 % lows), the gain of each speed feature on its own, 500 MHz over a whole mission, the cheaper FXAA pass (`gl_fxaa_quality=fast`), anti-aliasing Auto (version 1.1: how often it has FXAA on in a mission, whether its thresholds are right), the other settings rows on the console except the ones above |

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

See [`KNOWN-ISSUES.md`](../KNOWN-ISSUES.md): the intro movie's tear lines, banding in fire, hitches from render-target switches, and
everything above that has not been tried on the console.
