# Infected for PS Vita (native port)

A native PS Vita build of the PSP game Infected (ULUS-10054, version 1.02). The game's MIPS code is recompiled ahead of time to ARM
(the "lean" library inside the VPK), the PSP's system calls are re-implemented on top of the Vita's, and the PSP's graphics chip is
translated to OpenGL ES 2 on vitaGL. **The game itself is not in the VPK**: you bring your own disc image.

This build has everything together: the in-game **settings menu** (texture filter, mipmaps, anti-aliasing, pixel lighting, effects
resolution, frame limit, FPS counter, CPU clock, **enemy density** and **view distance**), **HD texture packs** (a 2x pack and a
mixed 4x pack), the render thread on its own core, and (when the build is given a shader cache: `build.sh --shader-cache`) a shipped set of compiled shaders. It has run in the Vita3K emulator from the logos to
mission 1 with the menu and the HD packs in use (see "What was and was not verified"), and has run on a real Vita since 10 October 2026 (mission 1, capped at 30 fps).
The documents for people who run it are `INSTALL.md` (the three steps, at the repository root), `SETTINGS.md` (every option) and `KNOWN-ISSUES.md`.
The hand-off documents of the first release candidate (a hardware test sheet, a change list) are kept in `docs/history/release-candidate/`, and
`vita/release/make_release.sh` assembles a private release folder. `SETTINGS-DEV.md` (next to this file) has the
developer side of the settings: pack memory, the palette watch, what was and was not checked. The benchmark modes at the end exist so that real-hardware numbers can come back.
The play-through **bot** (`bot=1`, off by default) and `bench=play` are described there too.

## What you need

* A PS Vita or PS TV with custom firmware (HENkaku / ensō), taiHEN and VitaShell.
* **`libshacccg.suprx` in `ur0:data/`** (that is, `ur0:data/libshacccg.suprx`): the Sony shader compiler, installed from your own
  console's firmware with one of the usual community installers. vitaGL compiles the game's GLSL shaders with it at run time. Shaders
  that are already compiled (a VPK built with `--shader-cache` ships all 35 programs the renderer can use, 70 files, copied to `ux0:data/infected/shader_cache/` at the first start; without one, or for anything else, the Vita compiles each shader when it is first drawn and keeps the result there)
  do not need it, but the first run of a shader that is not in the cache does. Without the module the game starts, and the
  boot screen says so.
* Your own dump of Infected (US, ULUS-10054, 1.02) as an ISO. The build refuses any other executable and says what it found.
* About 600 MB free on `ux0:` for the ISO, plus 72 MB (2x pack) and/or 102 MB (4x pack) if you use HD textures, and some tens of MB for the shader cache, saves and log.

## Install

**`INSTALL.md` has the three steps** (install the VPK, copy your ISO into `ux0:data/infected/`, open the bubble). Any file name ending
in `.iso` works there. The rest of this section is the detail for people who build the package or drive the console from a PC.

* The VPK carries the bubble's artwork and a RetroFlow cover and background. All of it is original artwork drawn by `vita/assets/gen_bubble.py`
  (nothing from the game). At the first start the app copies the cover and background to `ux0:data/RetroFlow/COVERS|BACKGROUNDS/Homebrew/`
  when RetroFlow is installed, and never replaces a picture that is already there.
* Over FTP with vitacompanion: `curl -T infected.vpk ftp://<vita-ip>:1337/ux0:/infected.vpk`, then install from VitaShell; the ISO the same way to
  `ux0:/data/infected/`. `echo "launch INFC00001" | nc <vita-ip> 1338` starts the game, `echo destroy | nc <vita-ip> 1338` closes it.
* HD textures (optional): `texpack_2x.bin` (about 72 MB) and/or the mixed 4x pack (about 102 MB; **it must be called `texpack_4x.bin`
  on the Vita**, rename `texpack_4x_mixed.bin` when you copy it) go to `ux0:data/infected/`. Do not use the 274 MB full 4x pack: it
  does not fit the Vita's texture memory. They are not part of the VPK. The game uses them as soon as they are there (settings menu, row
  "HD textures"; "Automatic" picks the 2x pack). `SETTINGS.md` and `SETTINGS-DEV.md` have the details and the memory advice.
* `ux0:data/infected/config.txt` is optional (see below).
* **Your own art instead of ours.** `vita/assets/art_from_disc.py Infected.iso art/ [screenshot.png]` makes the bubble pictures
  (`art/livearea`) and a RetroFlow cover and background (`art/retroflow`) from your own disc image. Build with
  `vita/app/build.sh --art art/` to put them in the package. They stay on your machine; do not publish them.
  `vita/vita/assets/art_from_image.py IMAGE art/` does the same from any picture you provide (fan art, a poster; `--cover`, `--background` and
  `--focus` pick the pictures and the crop). Keep such pictures and the output out of git. `vita/app/build.sh` without `--art` draws the original artwork.

### First run on a real console (five minutes)

1. Install the VPK, copy the ISO (and, if you want to try them, the packs).
2. Start "Infected" and play **about one minute of mission 1** with the default settings (skip the movies with CROSS, choose a new game, start mission 1).
3. Leave with **START + SELECT + L + R** (hold for a second).
4. Copy **`ux0:data/infected/log.txt`** off the console and send it with a line on how it felt (smooth, stuttering, anything wrong in the picture).

That is enough to learn whether the port runs at all and how fast. `docs/history/release-candidate/HARDWARE-TEST.md` (the original test sheet) then goes through the costly options one at a time with `bench=play`.

The first start shows a boot screen (arena, executable, system), then the game's logos and movies. During the logos the game prepares
the shader programs the package lists (about 1.5 s the first time; kept in `shader_cache` for later starts).

## Controls and leaving the game

The Vita's buttons are the PSP's. The game's own menus work as on the PSP (CROSS to confirm; the intro movies can be skipped with CROSS).

* **Exit**: hold **START + SELECT + L + R** for about one second, or press the **PS button** if the system lets the application see
  it. The game stops, the log is flushed and the process ends ("clean exit" in `log.txt`).
* If the game has stopped reading the pad (a hang), holding the same four buttons for three seconds ends the process from a watchdog
  thread.

## The settings menu

Hold **SELECT + START** for half a second (or tap the **top-right corner of the touch screen**) while playing. The game pauses, a menu with
two pages (Graphics, Performance) opens; UP/DOWN choose a row, LEFT/RIGHT change it, L/R switch page, CIRCLE or START close it.
Enemy density and view distance are on the Performance page: view distance changes at once, enemy density at the next mission.
The choices are saved in `ux0:data/infected/settings.txt`. SELECT + START together with L or R is not the menu (that is the way out
of the game). `SETTINGS.md` lists every option, what it costs and which ones need a restart.

## Where things are (`ux0:data/infected/`)

| Path | What |
| --- | --- |
| `Infected.iso` | the game disc (yours) |
| `config.txt` | optional settings of this port (below); read once at start |
| `settings.txt` | the port's user settings (`key=value`), written by the in-game settings menu (every option of `SETTINGS.md`); delete it for the defaults |
| `texpack_2x.bin`, `texpack_4x.bin` | optional HD texture packs (yours to copy; the menu row "HD textures" turns on when one is there). `texpack_4x.bin` is the **mixed** 4x pack (about 98 MB) |
| `log.txt` | the log of the **last** run (rewritten each start); send it with any report |
| `ms0/PSP/SAVEDATA/ULUS10054xxxx/` | the game's save data (the PSP memory stick is emulated by the folder `ms0/`); a copy of a PSP save folder dropped in works |
| `shader_cache/v3/{v,f}/XX/<hash>.gxp` | compiled shaders, one file per GLSL source (`v2/` is the 1.0 build's: the two sit side by side) |
| `dumps/` | GE dumps for `bench=ge_replay` (below) |
| `pad.script` | optional scripted input (`pad_script_file=pad.script`) for unattended runs |

If a save is missing, the game's own "Auto Load" screen reports that no data was found and goes on to a new game, as on a PSP.

## config.txt

One `key=value` per line, `#` or `;` starts a comment. Every key is also an environment variable `INFECTED_<KEY>` for desktop and emulator work.
Everything has a default; the file is only needed to change something.

| Key | Default | Meaning |
| --- | --- | --- |
| `threaded_renderer` | `1` | `0` runs the game, display lists and drawing on one core as in the first build. The user-facing switch is `perf.threaded_renderer=split/on/off` in `settings.txt` (the menu's Renderer thread; the Vita's default is Split); this key overrides it with on or off (restart needed) |
| `log_sync` | `0` | flush `log.txt` after every line (slower; use it to find a crash) |
| `fatal_wait` | `1` | the error screen waits for START; `0` shows it for three seconds then exits |
| `watchdog` | `8` | seconds without a flip before `[watchdog]` lines are logged (0 = off) |
| `clocks` | `1` | set CPU/GPU/bus to the maximum clocks |
| `hud` / `overlay` | `0` / | on-screen status text; `overlay=cpu` or `composite` |
| `trace_hle=N` / `hle_top=N` / `dump_threads=N` / `stats_hle=1` | off | diagnostics (first N system calls, the N most called ones each second, a thread table every N seconds, timing of system calls) |
| `verify_iso=1`, `iso_sha256=<hex>` | off | hash the whole ISO at start and compare |
| `pad_script_file=pad.script` / `pad_script=...` | none | scripted input |
| `gl_msaa` (0/2/4), `gl_vsync` (1), `gl_buffers` (3) | | vitaGL display setup |
| `gl_ram_pool_mib` (24), `gl_circular_pool_mib` (16), `gl_cdram_pool_mib` (rest of CDRAM), `gl_vdm_kib`, `gl_vertex_kib`, `gl_fragment_kib`, `gl_usse_kib` | | vitaGL memory pools (0 = vitaGL default) |
| `gl_shark_opt` | `2` | shader compiler optimisation level |
| `gl_specialize` | `1` | each draw's texture function, alpha comparison, blend fix-up and light shapes compiled into its shader program (short programs on the Vita's GPU); `0` goes back to programs that choose them per pixel |
| `gl_rt_scenes`, `gl_rt_scenes_fbo` | `4`, same | scenes per frame the render targets are created for (vitaGL `vglSetupRenderTargetScenesNum`; its own default is 1); `0` = that default. Unmeasured on the console |
| `gl_compact_vertices` | `1` | 3D draws are written without the vertex fields their shader does not read (24 or 36 bytes a vertex instead of 40); `0` writes the whole vertex. Same pictures on the PC replay; unmeasured on the console |
| `gl_scratch_buffers` | `1` | draw from vitaGL's per-frame GPU memory instead of client arrays (no per-draw copy and index scan); `0` turns it off |
| `gl_vgl_skip_redundant` | `1` | vitaGL leaves out GXM program, texture and vertex-stream calls that set what the draw before set (our vitaGL patch 0003). Measured: vitaGL's draws 4.2 ms a frame against 4.6-7.4, fps within the run-to-run spread (31.5 against 30-31), pictures right. `0` sets everything for every draw |
| `gl_timing=N`, `gl_gpu_timing=1` | off | every N frames a `[gl-time]` line: where the GL thread's time goes (`submit()` as a whole with the `append` into scratch memory and the `flush` inside it, draws, uploads, read-backs, present; the extra clock reads cost about 3 ms a frame); with `gl_gpu_timing` the present also waits for the GPU, which shows its backlog |
| `gl_fill_test=1`, `gl_null_draw=1` | off | bottleneck tests: every draw cut to 2x2 pixels (no fill or shading left), or no draw calls at all |
| `gl_display_scale_percent` | `0` | the display drawn at this percentage of the PSP's size (150 = 720x408); `perf.resolution` in the menu sets 150 |
| `ge_split` | from the menu | `1`/`0` overrides the menu's Renderer thread = Split (the default on the Vita): display lists and vertex decoding on core 2, the GL calls on core 1. Mission 1 with the bot: 29-31 fps split, 25 with one renderer thread |
| `ge_split_slots`, `ge_split_presents_ahead` | `1024`, `1` | the split's draw ring and how many presents the walker may be ahead of the GL thread (more of either measured no faster) |
| `ge_split_early_sync` | `0` | `1`: a GE job counts as done once the walker is through it, not once it is drawn, so the game's `sceGeDrawSync` waits less. Measured slower (27 against 30 fps: the GL thread, already the busiest, then runs at 100 %) and a texture the game rewrites right after the sync may show its new contents a frame early |
| `atrac_ahead` | `1` | the game's ATRAC3plus music and voice: the frame after the one the game just took is decoded on the helper core (core 3 when the system lets the game use it, else core 2) while the game runs on, so the next request only copies samples (about 3 ms a frame was spent decoding on the game's core in mission 1). Same samples as decoding in place (83,709 requests compared on the PC); unmeasured on the console. `0` decodes on the game's core as before |
| `gl_skip_invisible` | `1` | draws whose every fragment has alpha 0 and that then change nothing (a sprite or effect faded out) are not sent to vitaGL. About 2 draws a frame in the mission-1 replay, pictures identical; `0` draws them |
| `hd_async` | `1` | HD replacement textures are read from the memory card by a thread of their own; the original is drawn until the replacement arrives (a frame or two). `0` reads them on the GL thread as before: every HD texture from its first frame, and a 5-60 ms hitch each time a new one appears |
| `cost_probe` | `1` | the start-up `costs:` line (what a clock read, an event signal, a mutex and `thread_local` cost on this console; a few ms) |
| `gl_<name>` | | any renderer setting (`gl_quad_clear`, `gl_trace`, `gl_merge`, `gl_texture_budget_mb`, `gl_stats_every`, ...) |
| `gfx.<name>` / `perf.<name>` | | developer override of a menu option (`gfx.texture_filter=original`): decides for the run, the menu cannot change it, never saved |
| `hd_budget_mb` | per mode | GPU memory (MB) the HD replacement textures may use, for either pack |
| `hd_budget_2x_mb`, `hd_budget_4x_mb` | `32`, `48` | the same per pack; `hd_budget_mb` wins over both |
| `hd_stale_frames` | `600` | a replacement texture not drawn for this many frames is released when the HD budget is over 85 % spent |
| `hd_trace` | `0` | log every HD texture level as it is uploaded (large log) |
| `tune_log=1`, `tune_cells=N`, `tune_pools=N` | off | the gameplay options' log lines: every change of enemy density / view distance, the enemy cap of each area and the object pools every N frames (`[tune]`, `[cells]`, `[pool]`) |
| `prewarm_shaders` | `0` | build every shader variant at start (fills `shader_cache/`; a VPK built with `--shader-cache` already ships them) |
| `bench`, `bench_flips`, `bench_from`, `bench_dump`, `bench_report` | off | benchmark modes (below) |
| `bench_seconds`, `bench_interval` | `420`, `10` | `bench=play`: the longest run (seconds of play) and the seconds between `[play]` lines |
| `bot`, `bot_params` | off | the play-through bot (`bot=1`); `bot_params=k=v,k=v` are its parameters (`tools/bot/README.md`: `approach`, `lock`, `phase`, `retry`, `fresh`, ...); `bot_observe=1` only logs `[bot] event` lines |

## Reading log.txt

* Start of the file: build date, config actually used, the settings list (the HD row can read `greyed: No texture pack installed.` there even when a pack is in place: it is printed before the renderer looks for the packs; the `[hdtex] mode ... pack of N textures` line and the menu are right), memory line (`mem user_free=... heap=.../...`), `shader cache: N shipped file(s) copied`,
  the ISO check, `threaded renderer: guest executor on core 0, GE thread on core 1` (or `off`), and `[gl] program ... linked`
  lines (one per shader variant as the game first uses it, with the number of variants so far).
* Once a second: `[stat] ... t=61s flips=3571(+30) presented=30 frame=33.6ms ge=split:walk 73% gl 89% | ms/frame guest=22.4 hle=off gewalk=24.7 (vertex=N.N) glsubmit=0.0 present=1.7 decode=3.2 audio=0.5 io=0.0 idle=7.5 (gesync=8.4 flipwait=0.0) | splitwait=0.0 walkidle=9.2 glidle=3.7 | draws/f=719 gl_draws/f=383 | switches/s=... | audio q=... underruns=0(+0)`.
  `flips(+N)` is the game's frames in the last second. `ge=split:walk 73% gl 89%` is how busy the two renderer threads were
  (a thread is busy whenever it is not asleep: `walkidle`/`glidle` are their sleeps, `splitwait` the walker waiting for the GL
  thread); with one renderer thread it reads `ge=thr:NN%`, with `threaded_renderer=0` `ge=inline:` (gewalk/glsubmit/present are
  then part of the frame). `guest` is the game's own busy time: the frame less what it slept (`idle`), of which `gesync` waited in
  `sceGeDrawSync`/`sceGeListSync` for the renderer and `flipwait` at a flip for a renderer frames behind. The thread nearest 100 %
  is the bottleneck. `(vertex=N)` is the part of `gewalk` spent decoding (and skinning) the draws' vertices; it is already inside `gewalk`, not beside it.
  `[split] walker on core 2, GL on core 1; 1024 draw slots, ...` once at start says the split is running, and
  `[split] thread_local storage is shared between threads` would mean a build whose C++ runtime is single-threaded (VITAGL_NOTES.md section 4).
  `[gl-time] slow texture upload 11.2 ms: 0x... 256x128 format 5 ... | HD replacement ... | pack read 0.0 ms, GPU waits 0.0 ms`: an upload that
  took more than 3 ms (a hitch), each texture at its 1st, 2nd, 4th, 8th... slow upload.
  `[stat] mem ...` every minute: free user memory and the newlib heap. `[gl] textures: ...` (every `gl_stats_every` flips, default 1800, about 30 s):
  textures cached by the renderer and, for the HD pack, how many replacements and how many KiB are on the GPU (resident, peak).
* `[watchdog] no flip for N s ... last HLE import X ... guest pc 0x...` means the game stopped flipping. `0x...` can be turned into a
  function with `arm-vita-eabi-addr2line -e infected.elf 0x...`.
* `quit requested from the pad`, `runtime stopped: ...`, `clean exit` end a normal run. `FATAL:` lines come with an error screen.
* `[savedata]` lines show save loads and writes with their paths.

## Benchmarks (what to send back)

Both modes are selected in `config.txt` and need the VPK, the ISO (for `cpu`) or dumps (for `ge_replay`) in place. Run them with the
Vita on AC power, screen brightness at default, nothing else running. Send `ux0:data/infected/log.txt` afterwards.

### `bench=ge_replay` - graphics only

Replays recorded GE frames through the display list walker, the vertex decoder and the GL renderer, with no game code, so it measures the
GPU/GL path alone.

1. Record on the desktop: run the desktop build with `INFECTED_GE_DUMP=<dir>` for the scene you care about, or use the set the developer
   hands out.
2. Copy that folder (`flip_*.ge`, `pages.pack`, ...) to `ux0:data/infected/dumps/` (another folder: `bench_dump=<name>` below `ux0:data/infected/`).
3. `config.txt`:
   ```
   bench=ge_replay
   bench_flips=600
   bench_report=60
   ```
   `bench_flips=0` loops until START. `bench_report` is the window in flips.
4. Start the game. The screen shows the replayed frames. The log gets, per window and at the end:
   `[bench] ge_replay window flips=N | per flip: walk+decode X ms, GL submission Y ms, present+swap Z ms, total T ms (F fps) | submits ... gl_draws ... tex miss ... hit ... uploaded ... KiB`
   and a `totals` line (draws, merged draws, textures, shader variants and their compile time).

Send the `[bench] ge_replay window` and `totals` lines. The first window includes shader compilation; read the later ones.

### `bench=cpu` - the whole game

Runs the game normally (the threaded renderer is off for the measurement, so all categories are on one thread) and times N flips by category.

```
bench=cpu
bench_flips=3600
bench_from=0
pad_script_file=pad.script
```

`bench_from` is the first flip counted (use the flip where mission 1 starts to measure gameplay; the `[stat]` line shows the flip number),
`bench_flips` the window. At the end the log has a block of `[bench]` lines: wall time per flip in total / median / p95 / max, then
milliseconds per flip by category (guest code, GE list walk, vertex decode, GL submission, present, idle, kernel scheduler, audio, movie
decode), HLE time per library, and calls per flip. The game stops after the window. For a real player's-eye number, also send ten
seconds of `[stat]` lines from normal play with the default `threaded_renderer`.


### `bench=play` - the bot plays mission 1 (the number that matters)

```
bench=play
```

Turns the bot on (`bot=1`), keeps the **normal threaded renderer** (the framework's own `bench` modes would turn it off, so `INFECTED_BENCH` is cleared) and starts counting when the bot
is in a mission. The bot starts mission 1 from the profile on the memory stick (`ms0/`; the release has `bench_profile_ms0`). Every `bench_interval` seconds (10) the log gets one line, and at the end a
`FINAL` line and a `[play] end:` line (mission COMPLETED or not, deaths, memory):

```
[play] t=  20s flips=598 fps=59.8 low1%=41.2 | guest=8.9ms ge=63% swap=3.1ms | draws/f=640 | infected=4(avg 3.1 max 4) deaths=0 | HD pack in use=2x; settings: all defaults
```

`t=` is the time since the mission began, the other numbers are for the last window (FINAL: the whole run). `fps` is the average rate of the flips of the window and `low1%` the mean rate of the slowest 1 % of
the flip intervals; `guest` the milliseconds per flip spent in game code (the flip time minus everything the port accounts for), `ge` how busy the GE thread is (over 100 % = the GPU side is the limit),
`swap` the time in the present/swap, `draws/f` GE draw submissions per flip, `infected` enemies alive now (average and maximum of the window), `settings:` every setting that differs from its default
plus the HD pack really in use. It ends at the results screen or after `bench_seconds` of play, then stops the game (`runtime stopped: bench complete (play)`).
The bot ignores the Vita's own buttons while it plays (only START + SELECT + L + R, handled by the watchdog thread, still ends the game). Dotted settings in `config.txt` (`gfx.hd_textures=4x`) decide for the run.
On the PC: `tools/bot/run.sh` and `replay_check.sh` (`tools/bot/README.md`).

Measured on the console (16 Oct 2026, defaults, HD 2x pack, 444 MHz; each run its own bot game, so about 2 fps of spread between runs):

| Build | fps | 1 % low | Notes |
| --- | --- | --- | --- |
| one renderer thread (`perf.threaded_renderer=on`) | 25 | 7 | the GE thread 97 % busy: list walk 15 ms + GL 25 ms a frame |
| Split before the threading fix | 23 | 7 | everything ran on the walker (VITAGL_NOTES.md section 4) |
| Split, threading fixed | 25 | 8 | two system calls a draw between the threads |
| Split, signals only to a sleeping thread | 30-31 | 6-7 | game 23 ms, walker ~22 ms, GL ~25 ms a frame |
| + HD textures without GPU waits, read and recoloured off the GL thread | 32 | 15 | the default now |
| the same with no draw calls (`gl_null_draw=1`) | 36 | 9 | the CPU's ceiling: vitaGL's draws are ~13 ms of the GL thread |

## What was and was not verified

See `KNOWN-ISSUES.md` and `docs/STATUS.md` (the state of every component, and what has and has not run on a console). In short: the first runs were in the Vita3K emulator with the release
candidate VPK: the logos, intro, menus and mission 1 with the default settings; the settings menu opened with the pad chord; `bench=play` (the bot plays, `[play]` lines, a clean exit),
`bench=cpu`, `bench=ge_replay`; the shader cache (70 files, nothing compiled at the start); no guest memory faults. The emulator process itself crashes after "Game closed" (an emulator
teardown bug). On a real Vita (from 10 October 2026) the game boots and plays mission 1 at a capped 30 fps (table above). **Not verified on a console:** later missions, the 4x pack, long sessions, MSAA, the touch corner, HD movies.
