# Known Issues

Infected plays well on a Vita, but it is a young port and a few things are still rough. If you hit something that isn't listed
here, please [open an issue](https://github.com/CBOC-Vita-Lab/infected-vita/issues) and attach `ux0:data/infected/log.txt`.

## The short version

* **Performance.** Mission 1 holds 30 fps on the default settings. FXAA, pixel lighting and double-resolution effects all cost
  frames; with all of them on (and the 4x pack) it drops to 10-14 fps. A short stutter can still happen in heavy moments.
* **Only mission 1 has been played on a real Vita.** Later missions run on a PC build and should work, but haven't been checked on a console yet.
* **Visual glitches.** Thin horizontal tear lines in the intro movie, and sharp horizontal bands across some fire and explosions.
* **New picture options pause once.** Turning on pixel lighting or FXAA prepares new shaders the first time each effect is seen
  (a fraction of a second each, only once).

## If something goes wrong

Put a line in `ux0:data/infected/config.txt` (create the file if needed) to switch a feature back to the old behaviour:

| Problem | Line to try |
| --- | --- |
| Crashes or hangs at start | `core3_unlock=0` (doesn't load the fourth-core module) |
| Crashes with no log | `log_sync=1` (saves every log line, so the cause is kept) |
| Missing or broken music | `atrac_ahead=0` |
| Slow, stuttering or white textures | set **HD textures** to Off in the settings menu, then MSAA, then pixel lighting |

Delete `ux0:data/infected/settings.txt` to put every setting back to its default.

---

# Details for testers

Everything wrong or unchecked in version 1.0.

## Seen on the console

* **The intro movie shows horizontal tear lines.** The movie is a 512x512 texture that is re-uploaded every frame (about 15 ms
  each). Most likely the movie decoder writes the next picture while the GL thread still reads this one. Not checked against the
  single-thread renderer, not fixed.
* **Fire and explosions show sharp horizontal bands** in some captures from the console (the capture on the front page shows them).
  Not traced yet to the game or to the way the picture is captured.
* **Occasional hitches.** Known causes: the first draw after a render-target switch takes about 8 ms (worst 16 to 20 ms), and a new
  texture's decode plus upload takes 3 to 12 ms. How low the worst frames go has not been measured yet.
* **Shaders are built on the console.** The package lists the programs the game uses and they are prepared during the logos (47 in about
  1.5 s on a 3.74 console); the results are kept in `ux0:data/infected/shader_cache`. A picture option that needs new shaders
  (pixel lighting, FXAA) builds them the first time it is used, a pause of a few tenths of a second each, once.
* **30 fps, not 60.** The game runs at 30, and the drawing thread (vitaGL's draw calls) is the busiest part. With no draw calls at all the
  game and the display-list walker reach 36 fps, so 60 is out of reach.

## Not checked on a console

* Missions after the first (the PC bot plays missions 1 and 2; mission 3 needs the subway and the bot cannot do that).
* The 4x texture pack, and sessions of an hour or more (memory: the heap peaked at about 60 of 176 MB in mission 1).
* Writing save data to the memory-stick folder on `ux0:`, the touch-screen corner that opens the menu, MSAA on the real GPU, and
  how the sound behaves while the menu pauses the game.
* The gain of each speed feature on its own (four scenes per frame, compact vertices, the HUD upscale, shader pre-linking, the music
  decoded ahead, link-time optimisation, the fourth core). Each has a switch to turn it off (`SETTINGS.md`, `vita/README-VITA.md`); none has
  been compared on its own in a mission.
* The 500 MHz profile has been measured to run (494 MHz) but not its effect on a whole mission.
* Other firmware for the fourth core. The module (`vita/core3/`) has run on a 3.74 console: it loads, a thread runs on core 3 and the
  helper threads and the music decoder move there. Other firmware versions in 3.60-3.74 are not tried. A start that loaded it and crashed
  leaves `core3_unlock.pending` in `ux0:data/infected/`, and later starts leave the module out; `config.txt core3_unlock=0` turns it off.
* The worst frames (the 1 % lows): the average holds 30 in mission 1 on the defaults, the lows were not measured.

## HD movies are experimental

`gfx.hd_movies` is **off by default** and the 18 replacement movies are not part of this repository (you make them from your own disc with
`tools/hdmovies`). The decoder is the Vita's hardware H.264 decoder (SceAvcdec); in the Vita3K emulator its sysmodule does not load and the
game falls back to the original movies, and with an emulator-only switch the colours were wrong (green and magenta) until a fix to how the output
is read as a texture. The feature has not been run on a console. If you turn it on and the movies look wrong, turn it off again.

## The bot

`bot=1` and `bench=play` play **mission 1 only**. While the bot is on your own buttons are ignored (START + SELECT + L + R still ends the game).
They exist for repeatable speed tests, not for play.

## Tests and tooling

* `recomp/tests/run_synth.sh` does not build against the current PortableKit (a linkage clash in `difftest.cpp` and a private method); the
  harness needs porting to the current API.
* One PC test, `kernel_tests_13` ("pause scope, realtime clock"), is timing-sensitive and can fail on a loaded machine.
* The Vita3K emulator process crashes after "Game closed", and rarely died in mission 1 after the HD textures setting was changed from the menu
  on a heavily loaded host; both are emulator-side and have not been seen on a console.
* The documents in `docs/history/` were written while the work happened; their file paths and some of their statements are as of the day they were
  written. `docs/STATUS.md` and this page are kept current.

## Smaller things

* The first lines of `log.txt` list the settings before the renderer has looked for the texture packs, so the HD textures row can read
  `[greyed: No texture pack installed.]` there even with a pack in place; the later `[hdtex] mode ... pack of N textures` line and the menu are right.
* Enemy density applies at the next mission load. View distance is subtle in mission 1 (a closed street).
* The radar texture changes colour all the time; with HD on it is rebuilt a few times and then shown as the original until it is quiet.
* Only the **US version 1.02** (ULUS-10054) is supported. Other versions are refused with a message.
* The full 4x pack (274 MB) is for the PC only: it does not fit the Vita's texture memory. Use the mixed 4x pack.
