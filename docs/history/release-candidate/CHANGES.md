# What is new in this build

This is the release candidate: PortableKit branch `infected` 6e0a37c (+ one settings text change) and the Infected project, master 8bde47f (+ the bot in the Vita app, bench=play and these documents).
No game features were added to the game itself. The list is the work since the VPK that went through the earlier Vita3K runs (PortableKit fa635cd).

## New in this candidate

* **The play-through bot is in the Vita app** (`bot=1` in `config.txt`; off by default; nothing changes without that line).
  It plays mission 1 by pressing the buttons through the game's own input path. It reads the game's memory to find enemies. It is used for repeatable speed tests.
  While it plays, your own buttons do nothing (except START + SELECT + L + R, which still ends the game), so the settings menu cannot be opened.
  The same bot, on the PC, plays mission 1 to a win in 32 s of real time and gives the same result every time (score 21500, 39 kills, silver medal).
* **`bench=play`**: the bot plays mission 1 with the normal two-core renderer and the log gets a line every 10 s (`bench_interval`) and a FINAL line:
  average fps and the 1 % low, milliseconds per picture spent in the game's code, how busy the drawing core is, time waiting for the screen, drawing commands per picture,
  enemies alive, deaths, and the settings in effect. It ends by itself when the mission is over, or after `bench_seconds` (420 by default). The recipe is in `HARDWARE-TEST.md`.
* **A small set of recorded drawing frames** for `bench=ge_replay`: `dumps/`, 100 pictures of mission 1 in four bursts of 25 (55.4 MB), no game logic involved.
* **HD movies (experimental)**: the game's 18 movies can be replaced by 960x544 remakes (`ux0:data/infected/movies_hd/`), decoded by the Vita's hardware H.264 decoder.
  Off by default. The remakes are not part of this package. The settings row is marked EXPERIMENTAL. Read `KNOWN-ISSUES.md` before you turn it on.
* **The shader cache is complete and ships in the VPK**: 70 compiled shaders cover every combination of the options (checked: a start with this cache compiles nothing).
  A shader check over all variants (32 world programs and 5 post-processing programs) finds no problem.
* **Two bugs that only the emulator showed, fixed**: the Vita's C library has no `%zu` in `printf`. Four log lines of the game's helper code used it. One of them ended the program with a memory fault when the log line was printed.
  They are gone. (The helper code is only active with `tune_log`, `bot` or `bench=play`.)

## New since the earlier VPK (already in the candidate)

* **Enemy density** and **View distance** rows of the settings menu work (Performance page).
  * Enemy density 100 / 125 / 150 / 200 %: how many enemies are alive at once in each area. Applies at the next mission load; the mission you are in keeps its number.
    Cost hint High at 150 and 200 % (desktop counts: 150 % = +39..41 % draw calls and +14 % game code per picture; 200 % = +43..111 % draw calls and +25..40 % game code).
  * View distance 100 / 150 / 200 %: the far plane and fog start of each area are multiplied; takes effect at once. In a closed level such as mission 1 it mostly thins the haze.
* **HD textures: "Automatic" means the 2x pack** when it is installed (it used to pick the 4x pack). The 4x pack on the Vita is the **mixed pack**
  (4x world and effects, 2x characters and menus; 102.4 MB) installed as `texpack_4x.bin`. The 274 MB full 4x pack does not fit the Vita (the game logs a warning if it finds it under that name).
* New final packs: `texpack_2x.bin` 72.2 MB, mixed 4x 102.4 MB. The renderer keeps the mip levels stored in a pack (street lights and fires look right).
* **HD textures no longer fill their memory budget for good**: replacements of a level that was left, or old colour variants, are released when nobody has drawn them for 10 seconds and the budget is over 85 % spent.
  (In an emulator run the 32 MB budget of the 2x pack was spent after three retries of mission 1, and later textures kept their originals.)
* **HD memory budget per mode**: 2x 32 MB, 4x 48 MB (`hd_budget_2x_mb`, `hd_budget_4x_mb`; `hd_budget_mb` sets both).
* **The radar and other textures whose colours animate are cheap with HD on**: such a texture is rebuilt a few times and then left as the original until it is quiet.
  (Desktop, 300 frames, 2x pack: 191 rebuilds / 1,640 ms without the watch, 6 / 26 ms now.)
* The options of the earlier builds (pixel lighting, FXAA, MSAA preview, smooth filter, mip chains, movie filter, effects scale, frame cap, CPU clock) are unchanged.

## Under the hood

* The game-specific tuning and bot code is built into the Vita app. Before, only the PC build had it.
* `GameProfile::frame_hook` (once per picture), the interpreter write watch and `ge_replay --zstats` came with the tuning work.
* `set_flip_observer` in the tuning module is the one extra hook `bench=play` uses to count pictures.

## What did not change

Game logic, saves, controls, the packs' format, and the default settings (HD textures Automatic = 2x, filter Smooth, mipmaps On, anti-aliasing Off, pixel lighting Off, effects scale 1, HD movies Off,
60 fps cap, renderer thread On, enemy density 100 %, view distance 100 %). On the PC at the original options the picture of every recorded frame is identical to the build before
(341 pictures, plain and threaded).
