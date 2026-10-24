# Infected (ULUS10054 v1.02) desktop bring-up log

Newest at the bottom. All game-derived data stays under /work/infected/.

## Layout
- /work/ref/PortableKit      framework checkout, local branch `infected-bringup` (base 33800c4, commit 6da8792)
- /work/infected/port        the port (profile + CMakeLists), `portablekit` symlink -> framework; build dir `out/`
- /work/infected/data        INFECTED_DATA_DIR (installed BOOT.BIN as EBOOT.ELF, settings.ini -> input/Infected.iso, ms0/)
- /work/infected/tools       run.sh, dis.sh, hotfuncs.py, sheet.py, imports.tsv, pad scripts (*.script)
- /work/infected/work        scratch: relocated image (image.bin/.o/.dis), extracted disc files, hot lists
- /work/infected/logs        run logs; stats/ per-run statistics; shots/ debug-raster PNGs
- /work/infected/generated_report  psp_recomp output, for numbers only. Never compiled.

## Log
- Input hashes verified (ISO, BOOT.BIN). PARAM.SFO: DISC_ID ULUS10054, DISC_VERSION 1.02, REGION 0x8000. BOOT.BIN is a plain
  PRX (module Rw37Skel, RenderWare 3.7); the installer accepts a plain ELF at executable_path_on_disc, so no framework change
  was needed to install it.
- Framework: configured with -DPORTABLEKIT_FFMPEG=bundled (FFmpeg 7.1.5), no renderer (no SDL3/Vulkan/glslang here).
  First build without FFmpeg left sceMpeg/sceAtrac as stubs and the game then misused async IO; rebuilt with FFmpeg.
- sceKernelReferThreadProfiler/ReferGlobalProfiler: the game calls them at start (0x0890DF2C) and writes through the result.
  They now return NULL and write nothing (UNVERIFIED stub; before, they wrote through whatever a0 held and crashed).
- The "deadlock" seen early was the intro movie decoding slowly in wall time, not a real deadlock. Added run limits
  (INFECTED_MAX_FRAMES/_WALL/_EMU) so the end-of-run report prints; SIGKILL loses buffered output.
- Pacing: Infected is not vblank-paced. Once per frame the main thread calls sceKernelGetSystemTime + sceKernelSysClock2USec
  (3 to 6 pairs in gameplay), sceDisplayGetVcount twice, sceKernelDelayThread once, then SetFrameBuf. The emulated clock
  only advances when every thread waits, so guest compute cost 0 and the game ran at ~300 us per flip. Added
  INFECTED_CPU_NS_PER_INSN (charge emulated time per interpreted instruction) and INFECTED_FRAME_LIMIT_US (flips at least
  N us apart). Used 4 ns and 16683 us for the measurement runs.
- Skipping the intro: the game polls the pad and acts on the CROSS release edge (confirm_button=1). START does nothing.
- Added a debug software rasteriser (INFECTED_SHOT_DIR) so frames can be looked at; found the screens in order: Majesco and
  Planet Moon logos -> title ("NEW OFFICER") -> Officer Name -> Officer Location (map, province) -> officer card -> intro
  movies (tut1.pmf, intro.pmf) -> Campaign menu (Mission Selection, Customize Officer, Upgrades, Kudos, Cutscenes,
  Difficulty) -> "Mission 1: NEW MEAT" briefing with a loading bar -> mission HUD (radar, weapons, timer, "HEALTH LOW").
  Scripts: tools/to_mission.script (CROSS pulses every 2-4 s plus the name-entry cursor path) reproduces it; it is keyed to
  emulated time, so it is only as repeatable as the movie timing.
- The rasteriser draws the HUD and 2D screens. The 3D world is black: gameplay renders into off-screen targets (480, 240
  and 120 wide) and reads them back as textures, which it does not model. That is the rasteriser's gap, not the game's.
- Native memcpy/memset hooks (profile patch_loaded_image): the game's memcpy (0x08A2F228) is a byte loop, 29% of all
  menu instructions and 4.3M instructions per decoded movie frame. Replacing it made the intro 7x faster in wall time, but
  with both hooks on the end of intro.pmf hangs on the interpreter (4 runs, 4 ns and 40 ns per instruction, with and without
  accounting for the skipped instructions): the movie player waits for a queue entry no thread will push, readThread and
  soundThread already dormant. With either hook alone, or neither, the run got through (the single-hook runs never played
  the movie, so that is not evidence). Cause not found; likely a thread interleaving the faster loops expose. The hooks are
  therefore off by default (INFECTED_LIBC_HOOKS=1|memcpy|memset turns them on). All measurements below are without hooks.
- psp_recomp --auto on BOOT.BIN: 53 s, 12,520 function seeds, 162 units, 3.75M lines / 169 MB of C++, 140
  `unsupported(...)` sites, all "guest break trap". Not compiled.

## Measurements (runs s9, s17, s18 under stats/; no hooks, 4 ns per instruction, 16.683 ms flip limit)
See the report for the numbers; raw files are stats/s18/{frames,calls,pc,ge}_*.csv|txt, one set per 200 flips.
Hot functions: work/hot_menu.txt, work/hot_game.txt (tools/hotfuncs.py; function starts = jal targets + stack prologues).

## Reproduce
    cd /work/infected/port && cmake -S . -B out -DPORTABLEKIT_FFMPEG=bundled -DPORTABLEKIT_RENDERER=OFF && cmake --build out -j2
    /work/infected/tools/run.sh 700 NAME INFECTED_MAX_WALL=650 INFECTED_MAX_EMU=380 \
      INFECTED_FRAME_LIMIT_US=16683 INFECTED_CPU_NS_PER_INSN=4 \
      INFECTED_PAD_SCRIPT="$(cat /work/infected/tools/to_mission.script)" \
      INFECTED_STATS=/work/infected/stats/NAME INFECTED_STATS_PHASES="$(cat /work/infected/tools/phases_200.txt)"
Add INFECTED_SHOT_DIR=<dir> INFECTED_SHOT_FLIPS=1200-30000/240 for PNGs (the rasteriser's wall time inflates emulated time
per flip, so take pacing numbers from a run without it).

## Later notes
- work/sample_generated_function.cpp: 30 lines of psp_recomp output (guest 0x088CEB40..0x088CEB78, the thread_sfx kill
  function). Basic-block form: one label per guest instruction, `ctx.gpr[]` as registers, imports reached by setting
  ctx.pc to the stub and returning.
- Unthrottled (no flip limiter, 4 ns per instruction), emulated time per flip: menu 2.35 ms median, mission 7.3 ms median.
  Each flip is sceDisplaySetFrameBuf(fb, 512, pixelformat 3, sync 0 = immediate) after sceKernelDelayThread(300 us).
- Commits: framework 6da8792, df5ff52 on infected-bringup (base 33800c4); port 9ab4b79, 6759205. ctest in
  port/out/portablekit_fw: 21/21 pass.
