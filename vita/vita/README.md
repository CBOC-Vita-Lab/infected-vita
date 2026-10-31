# Infected - PS Vita side: self-test package, build, run

This directory builds `infected_selftest.vpk` (title ID `INFT00001`, bubble name "Infected Port Self-Test"), the
package that has to be run once in Vita3K and once on real hardware **before** the port is trusted with a design
decision. It exercises the same platform layer (`platform/vita/`) and the same libraries (vitaGL, FFmpeg) the port
will use, and writes plain-text results to the memory card.

Everything in this repository that could not be checked without a Vita is labelled `UNVERIFIED` and is tied to a
test (T1..T10) whose job is to settle it. Read `VITAGL_NOTES.md` for the vitaGL facts and `LOG.md` for pinned
versions and the exact commands used to build the libraries.

* No kubridge. The only plugin-ish dependency is `libshacccg.suprx` (runtime GLSL -> GXP compiler), loaded by
  vitaGL through SceShaccCgExt/taiHEN stubs. If it is missing the package still starts, logs a loud line, and the
  shader-dependent tests report `LIMIT` instead of `FAIL`.
* Game-derived data is never part of this repository or this package. T9 uses streams generated from synthetic
  input by `scripts/gen_t9_streams.py`.

## 1. What is where

| Path | Content |
|---|---|
| `platform/platform.h` | The documented `plat::` interface (the only thing the port core talks to) |
| `platform/vita/` | Vita implementation: `platform_vita.cpp`, `audio_vita.cpp`, `gl_vita.cpp` |
| `platform/desktop_headless/` | Twin used to run the harness on the build machine (no GL, no audio device) |
| `selftest/` | Harness + tests T1..T10 |
| `scripts/` | `env.sh`, `build-vitagl.sh`, `build-ffmpeg.sh`, `check-coro.sh`, `build-selftest-desktop.sh`, generators |
| `vita/build.sh` | One-shot LiveArea assets -> configure -> build -> VPK |
| `vita/assets/gen_livearea.py` | Procedural LiveArea art (icon0, pic0, bg, logo0, startup, template.xml); no external art |
| `CMakeLists.txt` | Top level; options `VITAGL_VARIANT`, `FFMPEG_VARIANT`, `VITA_ATTRIBUTE2`, `LIVEAREA_DIR` |
| `VITAGL_NOTES.md` | vitaGL flags, threading rule, cheapest draw path, GLSL translator limits, shader cache, pools |
| `LOG.md` | Pinned URLs/commits/checksums, exact build commands, what failed and how it was worked around |

Not in git (see `.gitignore`): `third_party/`, `sysroot/`, `build/`, `dist/`.

## 2. Build

Prerequisites (one-time, details and exact commands in `LOG.md`): vitasdk (GCC 15.2.0, `VITASDK` set by
`scripts/env.sh`), a private sysroot at `sysroot/` containing vitaGL (4 variants), FFmpeg (2 variants), vitaShaRK,
SceShaccCgExt, taiHEN stubs and math-neon. The shared SDK is never modified.

```sh
source scripts/env.sh
scripts/build-vitagl.sh release        # also: debug fast store     (each ~minutes at -j2, under nice)
scripts/build-ffmpeg.sh nopt           # also: pt
vita/build.sh                          # -> build/selftest-release/infected_selftest.vpk
vita/build-all.sh                      # all six flavours below, collected in dist/ (needs every library variant built)
```

`vita/build.sh [--vitagl release|debug|fast|store] [--ffmpeg nopt|pt] [--attr2 N] [--outdir DIR] [--clean]`

Flavours produced for the first test round (file sizes from the last build, `dist/`):

| File | vitaGL | FFmpeg | `ATTRIBUTE2` | Bytes | Purpose |
|---|---|---|---|---|---|
| `dist/infected_selftest.vpk` | release | nopt | 12 | 1,285,847 | **Run this first** |
| `dist/variants/infected_selftest-attr2-0.vpk` | release | nopt | 0 | 1,285,865 | Retail memory, to compare / to fall back to |
| `dist/variants/infected_selftest-debug.vpk` | debug | nopt | 12 | 1,347,843 | Error log, thread-safeness checker, GLSL translator dumps |
| `dist/variants/infected_selftest-fast.vpk` | fast | nopt | 12 | 1,279,253 | DRAW/SAMPLERS/BUFFERS speedhacks + scratch memory |
| `dist/variants/infected_selftest-store.vpk` | store | nopt | 12 | 1,285,956 | `STORE_DEPTH_STENCIL` (depth survives scene breaks) |
| `dist/variants/infected_selftest-ffmpeg-pt.vpk` | release | pt | 12 | 1,306,080 | FFmpeg with pthreads (runtime UNVERIFIED) |

All flavours use the same title ID, so they replace each other when installed, and they share `ux0:data/infected/`. Rename or
fetch `selftest.txt` between runs. Every run starts `selftest.txt` with a `T0 build` line that names the flavour (vitaGL variant,
FFmpeg variant, the `ATTRIBUTE2` this package was built with) followed by a `T0 memory-at-start` line (what the OS actually
granted), so the files are self-describing.

Sanity checks done on the build machine (no Vita code can run there):

* The ELF links with `--no-enum-size-warning` (FFmpeg is built with 32-bit enums, the rest with the toolchain's
  `-fshort-enums`; only `selftest/t_ffmpeg.cpp` sees FFmpeg types and is compiled with `-fno-short-enums`).
* `scripts/build-selftest-desktop.sh run` builds the harness against `platform/desktop_headless/` and runs it: T1, T2,
  T4, T5 (null audio device) and T10 execute; T3 (ARMv7 assembly), T6-T8 (GL) and T9 (the FFmpeg is ARM-only) report `SKIP`
  (22 passed, 0 failed at the last run).
* `scripts/check-coro.sh` assembles the real ARM coroutine switch (`selftest/coro_arm.S`) and runs it instruction by
  instruction in the Unicorn emulator (`python3 -m pip install unicorn`): star, ring and deep-C++-recursion modes with
  register/stack-canary checks, all three print `OK`. `scripts/check-coro.sh --negative` runs the same test against a
  deliberately broken switch (r7 clobbered after the restore) and must be rejected. This is the only execution test the ARM
  assembly gets before hardware; Unicorn is not a Cortex-A9 (no timing, no cache, no exclusive-monitor behaviour).

## 3. Run in Vita3K

Vita3K must be recent enough to have the CLI described below (the checked-out source in `third_party/Vita3K` is
`b75d5e6`, see `LOG.md`). Vita3K cannot compile GLSL (there is no `libshacccg`), so **T6, T7 and T8 report
`LIMIT` there**; T1-T5, T9 and T10 are meaningful (T1 clocks, T3 coroutine timings and T4 core pinning are
emulator-dependent: treat them as smoke tests).

```sh
# install and start in one go (archive given on the command line is installed, then the first "gd" title is booted)
./Vita3K dist/infected_selftest.vpk

# or, if it is already installed
./Vita3K -r INFT00001
./Vita3K -r INFT00001 --frontend sdl     # plain window that runs the app given by -r and quits with it (no Qt UI)
```

Useful flags (from `vita3k/config/src/config.cpp`): `-z` console mode, `-l <0..6>` log level (0 = trace),
`-A` archive a copy of the log under the title ID, `-F` fullscreen, `-B <backend>` renderer backend,
`-c <path>` config location, `-d INFT00001` delete the installed title.

Where things go:

* **Portable install** (a directory named `portable/` next to the Vita3K executable): everything is below it.
  * `ux0:` = `portable/fs/ux0/`
  * results: `portable/fs/ux0/data/infected/selftest.txt`
  * platform log: `portable/fs/ux0/data/infected/log.txt`
  * optional config you create: `portable/fs/ux0/data/infected/selftest.cfg`
  * the app itself (`app0:`): `portable/fs/ux0/app/INFT00001/`
  * Vita3K's own log: `portable/vita3k.log`
* **A Vita3K set up with a storage folder** (as the macOS builds usually are): `ux0:` is `<folder>/fs/ux0/`, so the files are
  `<folder>/fs/ux0/data/infected/...`.
* **Non-portable Linux**: filesystem below `~/.local/share/Vita3K/Vita3K/` (same `ux0/...` layout), Vita3K log
  `~/.cache/Vita3K/vita3k.log`. (Other OSes: the SDL pref path of org `Vita3K`, app `Vita3K`.)

Vita3K does not honour `SCE_O_TRUNC`: a file reopened for writing and rewritten shorter keeps the old run's tail. The platform layer
removes `selftest.txt` and `log.txt` before creating them, so each run starts from empty files. If a file still looks odd, read it
from the top and trust the `T0 build` line of the latest run.

The on-screen panel shows the same lines as `selftest.txt`; START (or the emulator's mapped START key) exits early.
The program exits by itself after the last test plus `exit_delay_ms` (default 4000 ms).

## 4. Run on a real Vita (HENkaku, firmware 3.74)

One-time prerequisites on the console:

1. HENkaku enabled with "Enable unsafe homebrew" turned on (the VPK contains an UNSAFE self, see `CMakeLists.txt`).
2. taiHEN is running (it always is under HENkaku; vitaGL's SceShaccCgExt stub resolves through it).
3. `ur0:data/libshacccg.suprx` (or `ur0:data/external/libshacccg.suprx`) present (the shader compiler dumped from the console's own firmware; not distributed
   here). Without it the self-test still runs: `log.txt` gets a loud line when GL is initialised, and T6/T7/T8 report `LIMIT libshacccg.suprx is missing` for everything that needs the shader compiler.
4. [vitacompanion](https://github.com/devnoname120/vitacompanion) installed and loaded (a taiHEN plugin under `*main` in
   `ur0:tai/config.txt`; follow its own README). It provides:
   * an **FTP server on port 1337**
   * a **command server on port 1338** (plain TCP text commands, one per line): `launch <TITLEID>` starts an installed
     app and `destroy` kills the running app(s). The vitacompanion 1.08 build described in the project roadmap spells the
     second one `quit all|<TITLEID>` (and adds `reboot`, `screen on|off`, `nosleep on|off|status`): send `help` once and use
     only forms it lists; malformed arguments have been seen to hang the command port.

```sh
VITA=<vita-ip>          # the console's IP (shown in the vitacompanion / VitaShell UI)

# 1. put the package where VitaShell can find it, then install it from VitaShell (cross on the .vpk)
curl -T dist/infected_selftest.vpk ftp://$VITA:1337/ux0:/data/infected_selftest.vpk
#    or any FTP client:  ftp $VITA 1337   ->   cd ux0:/data   ->   binary   ->   put infected_selftest.vpk

# 2. start / stop from the PC
printf 'launch INFT00001\n' | nc $VITA 1338
printf 'destroy\n'          | nc $VITA 1338      # kill the foreground app (e.g. when it hangs)

# 3. read the results
curl -s ftp://$VITA:1337/ux0:/data/infected/selftest.txt
curl -s ftp://$VITA:1337/ux0:/data/infected/log.txt
```

Faster edit loop after the first install: upload only the new executable over the installed one:
`curl -T build/selftest-release/infected_selftest.self ftp://$VITA:1337/ux0:/app/INFT00001/eboot.bin`, then `destroy`
and `launch INFT00001` (the VPK's `eboot.bin` is byte-for-byte this `.self`: both are 1,283,284 bytes in the release build).
Remote launches of a console that is locked or has its screen off can start the app suspended in the background (seen with
other apps, see the project roadmap): wake and unlock the Vita first, keep it awake (`nosleep on` where the build has it) and
do not transfer files during a measured run (T8 timing, T10 throughput).

The syntax of the FTP paths (`ux0:/...` as the first path component) is how vitacompanion/VitaShell expose
partitions; **UNVERIFIED** here (no console); adjust if your client needs `cd ux0:` explicitly. The exact set of
vitacompanion commands beyond `launch` and `destroy` is not relied on.

To change what the self-test does, write `ux0:data/infected/selftest.cfg` (section 6) the same way:
`curl -T selftest.cfg ftp://$VITA:1337/ux0:/data/infected/selftest.cfg`.

Result handling: `selftest.txt` is written with one `sceIoWrite` per line (no buffering in the program), and every line
is also written to `log.txt` with a timestamp and core id. A crash of the app or a `destroy` therefore leaves everything up to
the last completed step. If the whole console freezes and has to be hard-reset, un-synced data may still be lost: put
`log_sync=1` in `selftest.cfg` and read `log.txt`. The last line of a run that reached the end is
`INFO  T0 summary: RESULT: PASS|FAIL  (N passed, M failed)`. A file that ends without it means the run crashed or was killed:
the last `T<n> begin` line names the test that was running, and `log.txt` holds the finer position.

## 5. Reading the result file

Every line is `KIND TEST name: details`, KIND being:

| KIND | Meaning |
|---|---|
| `PASS` / `FAIL` | A checked property. Any `FAIL` makes the RESULT line `FAIL`. |
| `INFO` | A measurement or fact (clocks, timings, memory, GL strings, ...). The numbers are the deliverable. |
| `LIMIT` | A known limitation of the stack, reported with what the port must do about it. Does not fail the run. |
| `SKIP` | Test disabled by `selftest.cfg`, or not applicable on this platform / variant. |

| Test | What it settles |
|---|---|
| T1 | Clock readback and setting (`scePowerSet*ClockFrequency`), `yield` cost, plain CPU throughput |
| T2 | 66 MiB guest arena (`USER_RW`, 1 MiB aligned), arena page-touch cost and bandwidth, a 96 MiB newlib allocation on top, extra headroom in 16 MiB steps, guest address map check |
| T3 | Hand-written ARM coroutine switch: register/stack integrity (star, ring, deep C++ recursion), switch cost in ns |
| T4 | Core pinning of threads (cores 0/1/2), per-core throughput vs one core alone, priority change |
| T5 | Audio: MAIN port and BGM port open, rate/grain, underrun counting with a producer thread on core 2 |
| T6 | GL basics on the GL thread: GLSL probe set against the translator limits, GL strings, shader cold/warm, texture orientation, blend colour, colour-mask clear, stencil bits, depth across a scene break |
| T7 | Render to texture: FBO orientation, depth across an FBO round trip, how many FBOs coexist, pool usage |
| T8 | Draw cost: ~700 draws/frame with VBO ring, uniform updates, client arrays vs VBO, FBO switch cost, blend churn, per-frame CPU submit budget |
| T9 | FFmpeg: ATRAC3plus-capable decoder open + synthetic blocks (decoder-open and MDCT table memory), H.264 decode of the generated stream (cost per frame) |
| T10 | `app0:` readable, 16 MiB write, sequential read throughput versus chunk size and read-ahead buffer, CRC verified |

T9's H.264 numbers are for *synthetic low-motion* content (generated with `/usr/bin/ffmpeg` + libx264 by
`scripts/gen_t9_streams.py`; the streams are embedded in `selftest/t9_streams.h`). Real UMD video costs more per
frame; use the figure as a lower bound.

## 6. `ux0:data/infected/selftest.cfg`

Plain `key=value` lines, `#` starts a comment. Missing file = defaults (and a log line saying so).

| Key | Default | Meaning |
|---|---|---|
| `t1` ... `t10` | 1 | `t5=0` disables T5, ... |
| `only` | - | Run only these, e.g. `only=T8` or `only=T2,T9` |
| `skip` | - | Skip these, e.g. `skip=T5,T7` |
| `exit_delay_ms` | 4000 | How long the final summary stays on screen (START leaves earlier) |
| `log_sync` | 0 | 1 = `log.txt` is fsync'ed after every line (slower). `log.txt` mirrors every result line, so this is the record that survives a console freeze or hard reset |
| `t3.pingpongs` | 100000 | Round trips for the coroutine timing |
| `t4.ms`, `t4.rounds` | 1000, 2000 | Thread test duration / handshake rounds |
| `t5.ms`, `t5.main`, `t5.bgm` | 2000, 1, 1 | Audio run length, enable MAIN port run, enable BGM port run |
| `t6.probes` | 1 | 0 skips the GLSL probe set |
| `t8.frames`, `t8.warmup`, `t8.draws`, `t8.verts`, `t8.period` | 60, 5, 700, 28, 50 | Draw-cost shape: frames measured, warm-up frames, draws per frame, vertices per strip, program switch every N draws |
| `t8.fbo`, `t8.vsync`, `t8.nofbo_compare` | 1, 0, 1 | Include the one-FBO-round-trip-per-frame case, vsync during T8 (off so swap time shows GPU back-pressure rather than the vblank wait), also run the no-FBO comparison |
| `t9.loops` | 3 | Decoder passes over the streams |
| `t10.mib` | 16 | File test size |
| `gl.thread` | 1 | 0 = run the GL tests on the main thread (core 0) instead of the dedicated GL thread (core 1) |
| `gl.vsync` | 1 | 1 = vsync on (`vglWaitVblankStart(GL_TRUE)`), 0 = uncapped |
| `gl.msaa` | 0 | 0 (off), 2 or 4 (x MSAA; costs CDRAM) |
| `gl.buffers` | 3 | Number of display buffers (3 = triple, 2 = double) |
| `gl.ram_mib` | 24 | vitaGL RAM pool (`USER_RW_UNCACHE`: textures and vertex data that do not fit CDRAM) |
| `gl.cdram_mib` | -1 | CDRAM pool size; -1 = everything that is free |
| `gl.cdram_keep_mib` | 0 | CDRAM left unused when `cdram_mib=-1` |
| `gl.phycont_mib` | 0 | Phycont pool (kept free for SceVideodec, see section 7) |
| `gl.circular_mib` | 16 | Circular pool (`vglSetCircularPoolSize`): per-frame transient vertex data (client arrays, copied uniforms; also scratch buffers in the fast variant). T8 logs the peak use |
| `gl.vdm_kib`, `gl.vertex_kib`, `gl.fragment_kib`, `gl.usse_kib` | 0 | GXM ring-buffer sizes; 0 = vitaGL default |
| `gl.scratch_dynamic`, `gl.scratch_stream` | 0, 1 | Fast variant only: put `GL_DYNAMIC_DRAW` / `GL_STREAM_DRAW` buffers in scratch memory. Dynamic must stay 0 for a persistent ring buffer (it would be overwritten after a few frames). |

Example for a quick GL-only run of the draw-cost test with more frames:

```
only=T8
t8.frames=300
t8.vsync=0
exit_delay_ms=1000
```

## 7. Memory

Budget the platform layer plans for (see `platform/platform.h` and `VITAGL_NOTES.md` sections 2 and 3):

| Item | Size | Where it is set |
|---|---|---|
| newlib heap (`malloc`, FFmpeg, std containers) | 144 MiB | `_newlib_heap_size_user` in `selftest/st_harness.cpp` (vitasdk's weak default is 128 MiB; it is allocated as a `USER_RW` block of type `0x0C20D060`) |
| Guest arena | 66 MiB | `plat::mem_alloc_block`, a separate `USER_RW` block (T2) |
| vitaGL RAM pool | 24 MiB | `gl.ram_mib` |
| Circular pool | 16 MiB | `gl.circular_mib` |
| CDRAM pool | all free | `gl.cdram_mib=-1` |
| Phycont pool | 0 | `gl.phycont_mib=0`: left free for SceVideodec/SceAvcdec |
| Image BSS | about 6.9 MiB (7,222,248 B) | 4 MiB of it are FFmpeg's float transform tables (`ff_tx_tab_*_float`) |
| MSAA | off | `gl.msaa=0` (costs CDRAM) |

The port itself needs about 130 MiB of newlib heap plus the 66 MiB arena, i.e. about 200 MiB of user memory and
therefore more than retail apps normally get. That is what `ATTRIBUTE2` is for.

`param.sfo` (written by `vita-mksfoex` through `vita_create_vpk`, shown in the `vita/build.sh` output):

* `TITLE_ID=INFT00001`, `TITLE`/`STITLE=Infected Port Self-Test`, `CATEGORY=gd`, `APP_VER=01.00`, `VERSION=01.00`,
  `PARENTAL_LEVEL=1`, `ATTRIBUTE=0x8000`, `ATTRIBUTE_MINOR=0x10`.
* **`ATTRIBUTE2=12`** (0x0C) in the default package, `0` in the `attr2-0` package. Bits 2..3 select the extended memory
  mode; Vita3K's emulation (`SceSysmem`) treats 4 / 8 / 12 as +29 / +77 / +109 MiB of user memory.
  The value is always written explicitly: `vita-mksfoex` defaults to `ATTRIBUTE2=12` when the flag is not given (checked by
  parsing its output), so "omitting" it would not produce a retail-memory package.

**UNVERIFIED on hardware** (T2 and the `platform init` lines in `log.txt` settle them):

* Whether firmware 3.74 honours `ATTRIBUTE2` for a HENkaku-installed app, and how much user memory it really grants.
* Whether the 144 MiB newlib heap, the 66 MiB arena and the 96 MiB test allocation all fit at once; what the 16 MiB-step
  headroom probe finds after that.
* Whether extended memory costs something else (e.g. less CDRAM). Compare `memory-at-start` lines of the `attr2-0` and
  default packages.
* Whether SceVideodec/SceAvcdec can be used next to the vitaGL pools at all (`VITAGL_NOTES.md` section 12 has the header
  facts; nothing here calls them). H.264 is decoded in software by FFmpeg in T9 so that its cost is known either way.

## 8. FFmpeg (T9)

Static FFmpeg 7.1.5 (`scripts/build-ffmpeg.sh nopt|pt`, `LOG.md` has the configure line): only the `atrac3`,
`atrac3p` and `h264` decoders and the `h264` parser; no demuxers, no swscale/swresample, NEON on, Thumb-2, built with
`-fno-short-enums`. A local patch (`scripts/patches/ffmpeg-tx-float-only.patch`) drops the double and int32 transform
codelets, which would otherwise add about 12.6 MiB of static BSS.

| Variant | `libavcodec.a` | `libavutil.a` | Notes |
|---|---|---|---|
| `nopt` (`--disable-pthreads`) | 1,387,418 B | 770,746 B | Default. Frame threading is off; decoding runs on the calling thread |
| `pt` (`--enable-pthreads`) | 1,410,754 B | 776,782 B | Links against vitasdk's pthread-embedded; runtime behaviour UNVERIFIED |

Both build and link. Whether `pt` runs correctly (thread creation, affinity, stack sizes in a Vita process) is exactly
what the `infected_selftest-ffmpeg-pt.vpk` run is for; compare its T9 lines with the default package.

## 9. vitaGL variants

| Library | Flags (on top of `NO_SPLASHSCREEN=1 HAVE_SHADER_CACHE=1 HAVE_VERTEX_LAYOUT_CACHE=1 NO_TEX_COMBINER=1`) | Bytes |
|---|---|---|
| `libvitaGL.a` (release) | `NO_DEBUG=1 INDICES_SPEEDHACK=1 SAMPLERS_SPEEDHACK=1` | 6,418,202 |
| `libvitaGL_debug.a` | `LOG_ERRORS=1 HAVE_SHARK_LOG=1 HAVE_PROFILING=1 DEBUG_THREAD_SAFENESS=1 DEBUG_GLSL_TRANSLATOR=1`, `-DsceClibPrintf=vgl_hook_printf` | 6,831,756 |
| `libvitaGL_fast.a` | `NO_DEBUG=1 DRAW_SPEEDHACK=1 SAMPLERS_SPEEDHACK=1 BUFFERS_SPEEDHACK=1 USE_SCRATCH_MEMORY=1` | 6,348,044 |
| `libvitaGL_store.a` | `NO_DEBUG=1 INDICES_SPEEDHACK=1 SAMPLERS_SPEEDHACK=1 STORE_DEPTH_STENCIL=1` | 6,418,036 |
| `libvitaGL_profile.a` | `NO_DEBUG=1 INDICES_SPEEDHACK=1 SAMPLERS_SPEEDHACK=1 HAVE_PROFILING=1 LOG_ERRORS=1`, `-DsceClibPrintf=vgl_hook_printf` | 6,555,522 |

The two speed options in the release, store and profile libraries (`INDICES_SPEEDHACK`, `SAMPLERS_SPEEDHACK`) were checked by reading the source against the port's GL calls, not by a run; the list of the options left off, with a reason each, is in `VITAGL_NOTES.md` section 1.

The one rule that matters most: **all GL calls must come from one thread** (the self-test and the port use core 1).
Only the debug library checks it (`DEBUG_THREAD_SAFENESS`: a call from another thread is dropped and logged); the release
libraries do not check, and two threads inside GL corrupt the sceGxm command lists. Everything else (cheapest draw path, translator limits, shader cache format, pools) is in `VITAGL_NOTES.md`.

### Shader cache pre-shipping

vitaGL keys its on-disk cache by a hash of the **raw GLSL text** and does not need `libshacccg` to *read* a cache
entry. So users without the compiler can run the port if the cache is shipped. Procedure (full detail, including the
512-subdirectory trap and the vertex/fragment pairing hazard, in `VITAGL_NOTES.md` section 7):

1. Run the port (or the T6/T8 shader set) once on a console that has `libshacccg.suprx`.
2. FTP out `ux0:data/infected/shader_cache/v3/` (the self-test uses `ux0:data/infected/selftest_shader_cache/`). A cache written by the
   1.0 build (vitaGL `dca4b9d`) is `v2/`, and `vita/scripts/shader-cache-to-v3.sh` turns it into the `v3/` layout of vitaGL `9bf3e3f`.
3. Put it in the VPK as `shader_cache/v3/{v,f}/XX/*.gxp`.
4. On start, `plat::dir_copy_tree("shader_cache", "shader_cache", false)` into the data root, before `vglInit` (vitaGL makes the `XX`
   folders of files it writes itself).
5. Never edit the shader preamble after the warm-up run: the GLSL text is the key.

## 10. File I/O advice (what T10 is meant to confirm)

Vita `sceIo` calls have a fixed per-call cost, so stream in large blocks: reads of 64 KiB or more (256 KiB to 1 MiB
for streaming), into a 64-byte aligned buffer, and give small-sequential-access code a read-ahead buffer through
`plat::file_open(..., buffer_bytes)`. Do not issue 4 KiB reads on a hot path. T10 prints MiB/s for 1 MiB, 256 KiB, 64 KiB,
16 KiB, 4 KiB and 4 KiB-through-a-256 KiB-buffer reads; the numbers on a real memory card are the point (a read
immediately after the write may be served from cache and look faster than a cold read). `selftest_io.bin` (16 MiB by
default) is deleted at the end of T10.

## 11. If something does not work

* Nothing on screen, no `selftest.txt`: read `log.txt` (platform layer) and, in Vita3K, `vita3k.log`. If even `log.txt`
  is absent, the executable did not start: check the title is installed from the `.vpk` built for this firmware, unsafe
  homebrew is on, and (hardware) the VPK was installed with VitaShell, not copied by hand.
* `T6`..`T8` say `libshacccg.suprx is missing`: expected in Vita3K. On hardware, put the file into `ur0:data/`.
* Hang during T6/T7/T8 on hardware: run `infected_selftest-debug.vpk` (vitaGL error log and GLSL translator dump go to
  `log.txt`), then `destroy` the app. Re-run with `skip=T8` or `only=T6` to bisect.
* Out-of-memory at T2: run the `attr2-0` and default packages one after the other and send both `selftest.txt` files.
* The `RESULT` line is missing: the run crashed or was killed; send `selftest.txt` and `log.txt` as they are.
