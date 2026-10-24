# Status of the Infected port (release candidate)

State at the release-candidate build: PortableKit branch `infected` + proj branch `master` merged in the integration workspace `/work/ws/all` (branch `all` in both repos;
the commit ids are at the end of `/work/ws/all/LOG.md`). Nothing has run on a real Vita. The owner's package is `/work/ws/all/dist/release/` (see "Release").

## Components

**PortableKit** (`/work/ws/all/PortableKit`, the framework). `src/` and `include/psprecomp/`: Allegrex interpreter, analyser, runtime; `host/kernel`, `host/hle`: HLE kernel and the PSP libraries;
`host/gpu` (`ge_*`: display-list walker, vertex decoder; `gl/gl_renderer.cpp`: the GLES2 renderer with HD textures, pixel lighting, FXAA, mip chains; `ge_worker`: the GE thread on core 1);
`host/settings/port_settings.*` (the 14 options), `host/overlay` (in-game menu), `host/movie` (MP4 reader, HD movies, hardware decode hooks), `host/input`, `host/audio`, `host/save_data`.
Tests: `tests/` (ctest, 42 tests). Replay tool: `tools/ge_replay` (GE dumps through the renderer, `--shader-lint`, `--threaded`, `--port baseline`).

**Game port** (`/work/ws/all/proj/port`). `host/infected_profile.cpp` (the game profile), `host/infected_tuning.*` (frame hook, enemy density, view distance, probes; built into the Vita app),
`host/infected_bot.*` (the play-through bot: a pad provider that reads guest memory; `Status status()`), `lean_count.cpp`. Builds `InfectedNative` (desktop) with the lean library.

**Recompiler** (`proj/recomp`). `lean_gen.py` + `leangen/` turn the game image into typed C++ ("lean"); `build_lib.sh vita|x86` makes `liblean_vita.a` / `liblean_x86.a`. The libraries in use are
`/work/ws/integ/private/{x86/liblean_x86.a,vita/liblean_vita.latest.a}` (not rebuilt in the integration). Synthetic regression: `recomp/tests/run_synth.sh` (see open items).

**Vita app** (`proj/vita`). `app/src/` (`app_main`, `app_config` = `config.txt`, `app_settings`, `app_bench` = bench modes, `app_status` = `[stat]` and `[play]` lines, `app_hd_movie`, `app_pad_script`, ...),
`app/CMakeLists.txt`, `app/build.sh`; `platform/` and `vita/` (the Vita layer), `selftest/`. Docs: `README-VITA.md` (developer), `SETTINGS-DEV.md`, `release/` (the owner's documents + `make_release.sh`).

**Bot and tools** (`proj/tools`). `bot/` (`run.sh` desktop harness, `replay_check.sh`, recorded win logs `mission1_win*.pad`, `README.md`, `sheet.py` contact sheets), `rc/` (release-candidate scripts: `regress_rc.sh`,
`v3k_session.py`), `tune/` (gameplay-tuning harness), `texpack/` (HD texture pack builder), `hdmovies/` (HD movie builder), `all_live.sh` and the flow scripts.

**Texture packs** (`/work/ws/tex/private/packs/final/`, linked into `ws/all/private/packs_vita/`). `texpack_2x.bin` 72,213,209 B; `texpack_4x_mixed.bin` 102,362,489 B, installed as `texpack_4x.bin`
(4x world and effects, 2x characters and menus). The 274 MB full 4x pack is PC only. "Automatic" = 2x if installed, else 4x; budgets 32 / 48 MB.

**HD movies** (EXPERIMENTAL, default off). Builder `proj/tools/hdmovies`, set `/work/ws/hdmov/private/hdbuild/movies_hd/` (18 files + `index.txt`, 394 MB, not in the release). Desktop: software decode, works.
Vita: SceAvcdec hardware decoder + GXM YUV texture (`app_hd_movie.cpp`); in Vita3K the sysmodule does not load (fallback to the original movies is fine) and with the load error ignored the chroma is wrong.
Another agent owns the fix (`/work/ws/hdmov`); it is not in this build. `docs/HD_MOVIES.md`.

**Vita3K harness** (`/work/ws/v3k`). `v3k.sh`, `bin/v3k.py` (software Vulkan, 3 to 13 flips/s, one user at a time through a lock, crash detection on the first `|E| [Memory` line, `|E|` symbolised with the ELF).
`proj/tools/rc/v3k_session.py SPEC.json` holds the lock for a whole session of runs, installs the VPK, stages `config.txt`/`settings.txt`/files into the shared data dir and puts the originals back afterwards.

## Build from scratch (this machine, 2 cores, everything under `nice`)

* Desktop: `cd /work/ws/all && ./configure_desktop.sh && nice cmake --build build/desktop -j2` (Ninja, Release; needs `ws/integ/private/x86/liblean_x86.a`). Binaries in `build/desktop/bin/`
  (`InfectedNative`, `ge_replay`, tests).
* Vita: `cd /work/ws/all && proj/vita/app/build.sh --portablekit /work/ws/all/PortableKit --lean-lib /work/ws/integ/private/vita/liblean_vita.latest.a --shader-cache <dir with v/ and f/>`
  (VitaSDK in `/work/vitasdk-root/vitasdk`, sysroot `ws/vita/sysroot`; `--clean` for a full rebuild). Writes `dist/infected.vpk` (5.7 MB) and the unstripped `dist/infected.elf` (108 MB).
  The shader cache of the release is `private/shader_cache_new/shader_cache` (70 files, from `prewarm_shaders=1` in Vita3K).
* Lean libraries: `proj/recomp/lean_gen.py --out GEN ...`, then `build_lib.sh x86|vita GEN OUT JOBS` (see `docs/LEAN_LOG.md`). Only needed if the recompiler changes.
* Release folder: `proj/vita/release/make_release.sh` (needs the packs, `private/dumps_m1`, `private/ms0.pristine`, the VPK and ELF) writes `dist/release/`.
* The GE dump set was recorded with `INFECTED_GE_DUMP=dir INFECTED_GE_DUMP_FLIPS=4200-4224,5800-5824,7400-7424,8800-8824` while `mission1_win.pad` replays (`bot/run.sh`, null draw).

## Tests and regressions

| What | Command | Result at the RC |
| --- | --- | --- |
| framework tests | `cd build/desktop/portablekit_fw && ctest` (serial, on a quiet box) | 42/42; under load average 13 `adhoc_tests` and `kernel_tests_13` (timing tests) flake and pass alone |
| replay regression, original options | `proj/tools/rc/regress_rc.sh mv run10 run9 live11` ( md5 of every picture against `private/regress/<set>/integ.md5`, plain and `--threaded`) | 341 pictures x 2: identical |
| shader lint | `EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 build/desktop/bin/ge_replay --shader-lint` | 32 world + 5 post/overlay programs, 0 problems |
| bot end-to-end (recorded win) | `BOT_WS=/work/ws/all BIN=.../InfectedNative proj/tools/bot/replay_check.sh` (also `mission1_win_fresh.pad` with `MS0=ms0.empty`) | REPLAY OK, 32 s (null draw, defaults); fresh profile 36 s |
| bot, real GL, defaults + packs | `NAME=replay_gl GL=real SCALE=2 PACKS=private/packs_vita TIMEOUT=7000 replay_check.sh` | REPLAY OK, same flip 9289 / score 21500; 2437 s wall with the box at load average 13 |
| bot, everything on | `scripts/rc_allon.sh` = `proj/tools/bot/run.sh allon SECS GL=real SCALE=2 PACKS=.. SETTINGS=.. HDMOVIES=.. INFECTED_BOT=1`, `bot/sheet.py` | mission 1 won at flip 9534 (2 deaths), 20 pictures looked at, no anomaly; 53 min wall under load |
| Vita3K sessions | `python3 -I proj/tools/rc/v3k_session.py SPEC.json` (`private/v3k_rc/spec*.json`) | bench=play default and all-on, menu, bench=cpu, bench=ge_replay: clean exits, 0 `[Memory` faults (LOG) |
| bench modes in Vita3K | `config.txt`: `bench=cpu bench_flips=400`, `bench=ge_replay bench_flips=100 bench_report=25` (dumps in `ux0:data/infected/dumps`), `bench=play` | all run |

## Open items

* **Nothing has run on a real Vita.** `release/HARDWARE-TEST.md` is the plan; the first numbers decide the defaults and the cost hints (all cost columns are estimates).
* HD movies on the Vita (chroma/format, SceAvcdec load) belong to the hdmov agent; merge their result, then rerun the HD movie checks. `gfx.hd_movies` stays off by default until then.
* The bot plays mission 1 only; mission 3 needs the subway. `fresh=1` (blank memory stick) is flip-timed and fragile on slow devices.
* `proj/recomp/tests/run_synth.sh` does not build against the merged PortableKit (difftest.cpp: `g_cpu`/`g_ram` linkage clash with `psprecomp/exec.hpp`, `GuestMemory::vram_bytes()` private). Port the harness to the current API.
* The 500 MHz CPU clock needs a plugin on the console; MSAA, the touch corner and the audio pause by ear are untested.
* Palette-animated textures (radar) still create a new renderer cache entry per palette change (original behaviour); a texture update in place would remove it.
* `kernel_tests_13` ("pause scope, realtime clock") is timing-sensitive under load.
* The shader cache has to be regenerated (`prewarm_shaders=1` in Vita3K, then copy `shader_cache/`) whenever a shader source changes; the lint tool does not check the cache.
* Vita3K: the emulator process died twice in mission 1 after an HD textures change from the menu while the host was heavily loaded (null access in Vita3K, no guest fault); three quiet-host tries were clean. `release/KNOWN-ISSUES.md`.
* The harness (`ws/v3k/bin/v3k_inner.sh:181`) prints `app-exit ... harmless` for any run whose log has "Saving pipeline cache", also for a mid-play death: check `clean exit` in the game's `log.txt`.
* Scripts `scripts/` in `ws/all` are copies of `proj/tools/rc/`; `private/` (packs, dumps, regress baselines, v3k specs and outputs, screenshots in `private/showcase3/`) is not in git.
