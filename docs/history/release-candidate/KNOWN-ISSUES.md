# Known issues and what is not checked

Read the first section before you test. The rest is for the developer.

## Not checked on a real Vita (that is everything)

The build has run only in the **Vita3K emulator** (on a PC, with a software renderer at 3 to 13 pictures per second) and, for the game logic and the picture, on the PC build.
Nothing below is known to work on the console, and no speed number from the emulator means anything for the console.

* **Speed.** The target is 60 pictures per second at the default settings. It is not known whether the Vita reaches it. The "cost" shown in the settings menu (None, Medium, High) is an estimate from PC counts, not a Vita measurement.
* **Graphics memory.** The 2x pack (budget 32 MB) and the mixed 4x pack (budget 48 MB) were tuned by counting on a PC. Whether they fit next to the frame buffers on a real Vita is unknown. Watch for white or missing textures and the `[gl] textures` lines in the log.
* **Shaders.** 70 compiled shaders ship in the VPK and the emulator takes all of them without compiling anything. The real GPU has not seen them.
* **The 500 MHz clock** (`perf.cpu_clock`) needs a plugin on the console. Without it the system stays at 444 MHz and the log says so (the emulator behaves the same).
* **MSAA** (`gfx.antialiasing=msaa4`) reaches vitaGL in the emulator. How it looks and costs on the real GPU is unknown.
* **The touch-screen corner** that opens the settings menu was never tested: the emulator harness cannot touch. The button chord SELECT + START was tested.
* **Audio** pauses while the settings menu is open (logged and checked on the PC). Nobody has listened to it on a console.
* **Save data and memory stick folder** work in the emulator. Not checked on `ux0:` of a real Vita.

## HD movies are experimental

`gfx.hd_movies` is **off by default** and its row says EXPERIMENTAL. The 18 replacement movies are not in the package.

* The decoder is the Vita's hardware H.264 decoder (SceAvcdec). It has never run on a real Vita.
* In the emulator the system module that provides the decoder does not load (`sceSysmoduleLoadModule` returns an error). The game then falls back to the original movies, with no fault. This fallback is tested.
* With an emulator-only switch that ignores that error, the replacement movies play but the **colours are wrong** (green and magenta): the way the decoder output is read as a texture needs fixing. Another developer is working on it. Their fix is **not** in this build.
* If you turn it on and the movies look wrong, turn it off again (the option, or delete `settings.txt`). Nothing else depends on it.

## The bot (`bot=1`, `bench=play`)

* It plays **mission 1** from a fresh start to the results screen and, on the PC, mission 2. Mission 3 ("Uptown Hurl") needs the subway and the bot cannot do that: it enters the second zone only by chance and dies there.
  Do not use the bot for anything but mission 1.
* While the bot is on, **your own buttons are ignored**. That includes the settings menu: it cannot be opened. START + SELECT + L + R still ends the game. For the settings, use `config.txt` lines (see `SETTINGS.md`).
* The bot starts mission 1 from the memory stick profile. It handles a blank memory stick (`bot_params=fresh=1`: it enters a name and chooses the first mission by timed button presses), but that flow is **fragile on a slow device**.
  Use the supplied `bench_profile_ms0` as described in `HARDWARE-TEST.md`.
* The game is driven by time. A slow device (like the emulator at a few pictures per second) lets the game run ahead of the bot, which then reacts late. In the emulator the bot died in the first minute and did not finish the mission.
  This is expected there and says nothing about the Vita.
* `bench=play` ends after the mission or `bench_seconds` (420), whichever comes first. If the bot dies, the mission goes on after the game revives you.
* The numbers of the bot's run are not all comparable between settings: enemy density and the bot's luck change the amount of fighting. Compare the `guest`, `ge` and `draws/f` columns, and run twice when a number looks odd.

## The recorded drawing frames (`dumps/`)

The set is 100 pictures from mission 1 only (a street, no inside scenes, little fire). `bench=ge_replay` therefore measures that street. Busier levels cost more.
The first replay window includes building shaders. Read the later windows.

## Open: the emulator process died twice after a live HD textures change

In the Vita3K emulator, in mission 1, the settings menu was used to change **HD textures** from Automatic to Off while the PC running the emulator was heavily loaded (several other jobs, load average 10 to 15).
Both times the **emulator process itself** died 9 and 30 seconds later: a null-pointer access inside Vita3K's own code (`Unhandled access to 0x0`, exit status 133). There was no fault inside the game (no `|E| [Memory` line) and the game's log has no `clean exit` line.
The same sequence on a quiet PC ran cleanly: in mission 1 (Off, then back to the 2x pack, both changes took effect, clean exit) and, twice, in the front-end menus. Opening and closing the menu without a change was clean under the same heavy load.
So it was seen 2 times in 3 tries in mission 1 and not reproduced. Whether the emulator or this port is at fault is **not known**. The first run on the real Vita should repeat the change (`HARDWARE-TEST.md`, part A, step 4).
If the real Vita stops here, the quick workaround is to leave HD textures alone in the menu and set it with `gfx.hd_textures=` in `config.txt` before the start.

## Found in the emulator, not in this game

* In earlier runs the Vita3K process itself crashed at the very end, after the game had exited cleanly. This is an emulator teardown bug. It did not show in the runs of this candidate that ended normally.
* The emulator test harness calls a run "app-exit, harmless" whenever the log contains "Saving pipeline cache", which is also true for a run where the emulator died in the middle of play. For this reason the evidence for a clean exit here is `clean exit` in the game's own `log.txt`.
* The emulator's `glClear` does nothing. The renderer has an optional workaround (`gl_quad_clear=1` in `config.txt`), off by default. A real Vita should not need it.
* Button presses sent to the settings menu in the emulator are unreliable at 3 to 13 pictures per second (some are lost, some count twice). A real Vita should not do that.
* The emulator's clock runs in real time, so the game runs at a lower picture number for the same mission time (mission 1 starts at picture 2200 there, 3300 on the PC at 60 pictures per second).

## Smaller things

* The first lines of `log.txt` list the settings before the renderer has looked for the packs, so the HD textures row can read `[greyed: No texture pack installed.]` there even with a pack in place.
  The line `[hdtex] mode ... pack of 2625 textures` a moment later, and the menu, are right.
* **Enemy density** applies at the next mission load. The mission you are in keeps its number. **View distance** is subtle in mission 1 (a closed street).
* **The radar (minimap) texture** changes its colours all the time. With HD on, such a texture is rebuilt a few times and then shown as the original until it is quiet.
  Every palette change still makes a new texture cache entry in the renderer, even without HD. That is the original behaviour and was left alone. Stale HD copies hold memory until they are released (10 seconds without being drawn, once the budget is 85 % spent).
* **One PC test is timing-sensitive**: `portablekit_kernel_tests_13` (an emulated-clock check inside a pause) failed once when the PC was heavily loaded and passes alone. A flake, not a fault of the build.
* **Only the US version 1.02 of the game is supported** (ULUS-10054). Other versions are refused with a message.
* The full 4x pack (274 MB) is for the PC only. Do not copy it to the Vita. The game only writes a warning in `log.txt` (over 160 MB) and then uses it anyway; past the memory budget its textures fall back to the originals.
