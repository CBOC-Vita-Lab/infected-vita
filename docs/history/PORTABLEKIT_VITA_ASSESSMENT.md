# PortableKit as the runtime for a native PS Vita port of "Infected" (ULUS10054)

Read-only engineering assessment. Read copy of PortableKit: `/work/assess/PortableKit` at commit `33800c4` (`git clone --depth 1 https://github.com/TeamGDB/PortableKit.git`, MIT). The reference checkout in `/work/ref/PortableKit` and the bring-up agent's tree were not touched. All experiments ran under `/work/assess/experiments/` with one CPU core (`-j1`, `nice`). Date of the work: 2026-10-09.

## Evidence labels

| Label | Meaning |
|---|---|
| **[M]** | Measured here: read in the code (file:line), compiled with the Vita toolchain (`arm-vita-eabi-g++` 15.2, `-O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb`), disassembled, or counted from the bring-up agent's interpreter statistics. |
| **[E]** | Estimated from measurements by the stated arithmetic. |
| **[U]** | Not verified. Vita SDK behaviour (GXM, `libshacccg`, memory block types, hardware decoders) and anything that needs a Vita. **Nothing in this document was run on a Vita or in an ARM emulator**: every speed figure derives from instruction counts. |
| **[G]** | Given in the task statement. |

## Executive summary

**Recommendation: option (2).** Build a slim Vita runtime from selected PortableKit components (about 22 k of its 66 k lines are reusable as is: `GeState` and the `DrawCall` IR, `texture_decode`, SAS, the kernel's data model, about 480 HLE functions through the `AllegrexContext` ABI, PSMF demux, savedata, ISO reader, the interpreter as an oracle) and replace the three parts that cannot survive on a 444 MHz Cortex-A9: the code generator, the kernel's execution model and the renderer. Do not port `host/` as it stands (option 1). About 14 to 20 k new or changed lines (I.4).

The facts that decide it:

1. **The generated-code model is the blocker, not the framework.** Today's generator emits 4,096-word functions with a per-word dispatch label and registers every guest word in a hash map. Compiled with the Vita compiler, a real unit costs **76.6 to 80.3 B and 25 to 26 ARM instructions per guest word**, about 52 MB of `.text` for BOOT.BIN, and the registry adds about 65 MB of heap on a 32-bit target (D.1, G) [M/E].
2. **A function-granular "lean" generator is 15x smaller and 10x leaner.** Prototype (790 lines of Python) on 9,229 of BOOT.BIN's 10,276 functions: **1.70 ARM instructions and 5.18 B per guest word with the mandatory memory mask** (1.50 and 4.54 unmasked), about 3.3 MB of `.text` for the whole game, 8.5 minutes of compile CPU instead of about 6.6 core-hours. On six real functions it is 10.5x fewer instructions and 9.9x fewer bytes than today's code (12.1x and 11.6x unmasked). Differential test against PortableKit's interpreter: 4,430 of 4,430 trials equal on 317 leaf functions (D.3, D.4) [M].
3. **The lean form needs real host stacks for guest threads.** PortableKit blocks by unwinding native frames to a dispatcher; function-granular C calls cannot be unwound. Vita has no `ucontext`, so the kernel becomes a coroutine kernel (ARM `swap_context`, 300 to 500 lines, 24 `block` call sites; B.1, B.2) [M].
4. **Memory: one 32 MiB arena with an unchecked, masked access, `g_ram + (a & 0x01FFFFFF)`.** The mask is mandatory because BOOT.BIN's texture swizzle routines use the PSP scratchpad at its absolute address `0x00010000`, and 15 sites use the uncached alias; the mask folds both onto the arena for +13% instructions. EDRAM must be relocated into a free block through `sceGeEdramGetAddr` (BOOT.BIN imports it) (C) [M].
5. **CPU budget is tight, not impossible.** 1.94 M guest instructions per flip [M] times 1.7 to 2.0 ARM instructions is 3.3 to 3.9 M, which is **9 to 18 ms of the 16.7 ms** guest-core budget at an assumed 0.5 to 0.8 IPC [E, IPC unmeasured], plus SAS (10 to 25% of the core, F.1) (D.5).
6. **The GPU side is a draw-count problem, not a shader problem.** Infected uses a narrow GE feature set (CLUT4/8 textures, 4 blend combinations, GEQUAL depth, up to 8 bones) but issues **600 to 760 draws per flip** and uses alpha test (`discard`) on 89% of them (E.3).

## Answers at a glance

| Q | Answer | Where |
|---|---|---|
| A | 66 k lines of framework outside tests and third party; 22 k portable as is, 12 k to rewrite (renderer 8.4 k, kernel execution model, dispatcher, audio sink, decoders), 32 k not needed. Generated code touches only `CorpusRuntime`/`AotFastView`/`AllegrexContext`, which is a clean seam for a new generator. | A |
| B | One host thread, cooperative, virtual clock, imports unwind native frames, GE walked synchronously at enqueue. On Vita: coroutine per guest thread, interrupts as nested calls, loop yield counters, real vsync pacing, core 0 guest / core 1 GE / core 2 audio+IO. | B |
| C | Two vectors today (RAM 32 or 64 MiB, EDRAM+scratchpad), canonicalise by `& 0x1FFFFFFF`, slow path for non-RAM, no write watches in use. Cheapest correct 32-bit model: 32 MiB arena, `g_ram + (a & 0x01FFFFFF)`, EDRAM relocated. | C |
| D | Today 76.6 to 80.3 B and about 25 ARM instructions per MIPS instruction. Lean: 5.18 B and 1.70 instructions (masked), measured on 68% of the program; validated by a differential test and a whole-program compile. | D |
| E | `GeState` gives a backend-independent `DrawCall` stream; the backend interface is `initialize`, `begin_display_list`, `gpu_decode`, `submit`, `present`, `upload_frame`, `write_back_frame`, `read_back_framebuffer`. GXM backend outline with its limits (no constant blend colour, no logic ops, blend baked into fragment programs). | E |
| F | Audio is SAS plus ATRAC3plus (all 75 `.at3` files are ATRAC3plus; the Vita has no hardware decoder for it), movies are PSMF/H.264 (hardware decoder possible), save data and input are small. | F |
| G | About 57 to 80 MiB of main RAM and 15 to 45 MB of CDRAM; the current registry would add about 65 MB of heap. | G |
| H | 44 of 46 translation units compile clean with the Vita compiler at the first attempt, the other two need `-std=gnu++20` (`strnlen`). Exceptions are error-path only. | H |
| I | Option (2); ordered work in seven steps, 14 to 20 k lines, step 0 to 2 testable on the desktop. | I |

## A. Architecture map

All paths are relative to the read copy `/work/assess/PortableKit` (commit 33800c4). LOC are physical lines counted with `wc -l` on `*.cpp *.hpp *.h *.vert *.frag *.in` in the directory (non-recursive where a subdirectory is listed separately). Verdicts: **P** = portable as is (compiles with the Vita toolchain; see H), **V** = needs a Vita-specific implementation or rework, **N** = not needed on Vita.

| Group (path) | Purpose | External deps | LOC | Verdict |
|---|---|---|---|---|
| `src/decoder.cpp`, `include/psprecomp/decoder.hpp` | Allegrex instruction decoder (`OpcodeKind`, operand fields) | none | 454 + 48 | **P** |
| `src/elf32.cpp`, `elf32.hpp` | ELF/PRX parse, relocation, import scan (`Elf32Image::load_and_relocate`, `scan_imports`) | none | 418 + 135 | **P** (install/tool side; the Vita runtime can load a pre-relocated image, like `work/image.bin`) |
| `src/guest_memory.cpp`, `guest_memory.hpp` | Guest RAM (`std::vector`), EDRAM + scratchpad, mirrors, checked and "AOT fast" accessors | none | 528 + 246 | **V** (replace by an arena, see C) |
| `include/psprecomp/allegrex_context.hpp` | `AllegrexContext` (gpr/hi/lo/pc/fpr/fcr31/`vfpu[128]`/`vfpu_ctrl`), FPU/VFPU semantic helpers (`apply_vfpu_source_prefix_ct` etc., line 213) | `<bit>`, `<cmath>` | 1,581 | **P** (it is also the HLE calling convention, B and D) |
| `src/interpreter.cpp`, `extra_instructions.cpp` (+hpp) | Reference interpreter and the shared "extra instruction" semantics | none | 997 + 377 + 99 | **P** (keep as fallback and as the oracle for differential tests) |
| `src/runtime.cpp`, `runtime.hpp`, `corpus_abi.hpp` | Dispatch loop (`Runtime::run`, `runtime.cpp:726-790`), function registry (`functions_` unordered_map + direct tables, `runtime.cpp:430-481`), HLE/NID binding (`invoke_import_cached`, `runtime.cpp:1276`), chained-call machinery (`corpus_abi.hpp:318-380`), starvation hook | `<unordered_map>`, `<functional>`, `<string>` | 1,378 + 297 + 413 | **V** (replace the dispatch/registry machinery; keep the HLE binding and NID registry) |
| `src/nid_registry.cpp`, `sha256.cpp`, `deflate.cpp`, `program_analysis.cpp`, `codegen_policy.hpp`, `common.hpp` | NID database, hashing, inflate, function discovery for the generator | none | 154+45+289+512+~120 | **P** |
| `tools/codegen_main.cpp`, `analyzer_main.cpp`, `dump_function.cpp` | `psp_recomp`: ELF → C++ units (`emit_regular` `:88-890`, `emit_function_source` `:1033-1281`, `generate_auto` `:1598-1821`) | core only | 1,871 + 205 + 24 | **N** at run time (host tool); the emitter is what D replaces |
| `host/kernel/` | Cooperative PSP kernel: threads, semaphores, event flags, mailboxes, mutexes, VTimers, partitions, interrupts/callbacks, virtual clock and real-time pacing (`kernel.cpp` 1,071 lines), `fixed_pool`, `message_pipe`, `iso_image`, `fast_loading`, `load_detector/trace`, `stall_watchdog` | `<chrono>`, `<thread>`, `<map>`, `<deque>`; `stall_watchdog` starts a detached `std::thread` (`stall_watchdog.cpp:108`) | 2,761 | **V** (execution model, B; the data model and wait semantics are reusable) |
| `host/hle/` (17 files) | HLE of ~480 firmware imports (`hle.add(...)` count per file: threadman 104, system 64, media 53, adhoc 43, io 40, net_offline 35, mpeg 33, atrac 20, sysmem 18, psmf 16, psmfplayer 15, utility 13, font 10, savedata 4) | `<filesystem>` (io), FFmpeg-backed helpers, `kernel()` | 10,315 | **P** for most through the `AllegrexContext &ctx` ABI; **V** for `hle_media.cpp` (display, ctrl, GE and audio output glue, 1,184), `hle_threadman.cpp` (1,686, kernel model), `hle_io.cpp` (ISO, filesystem), `hle_mpeg/psmfplayer/atrac` (decoders); **N** for `hle_adhoc`, `hle_net_offline` (1,231) and `hle_font` (Infected imports no `sceFont`) |
| `host/gpu/ge_state.*` | GE command interpreter: display list walker → `DrawCall` IR (`GeState::execute` `:1351`, `draw_primitive` `:994`, `decode_vertices` `:389`, patches `:1204`) | `GuestMemory` only | 1,473 + 352 | **P** (backend independent, E) |
| `host/gpu/texture_decode.*` | PSP texture formats → RGBA8, unswizzle, CLUT, DXT; texture content key (`texture_key`) | `GuestMemory` | 529 + 34 | **P** (decode targets must change for GXM, E) |
| `host/gpu/vulkan_renderer.*`, `shaders/*` | Vulkan renderer (uber shader, pipelines, render targets, readback, present, ImGui hookup) | Vulkan, SDL3, glslang, ImGui | 8,085 + 280 + 457 | **V** (replace by a GXM backend) |
| `host/gpu/frame_interpolation`, `frame_pacing`, `texture_pack*`, `replacement_textures` | Frame generation, pacing, HD texture packs | Vulkan types | 663 + 429 + 1,775 + 488 | **N** |
| `host/audio/sas_core.*` | SAS (VAG decode, ADSR, mixing; 32 voices) | none | 615 + ~100 | **P** |
| `host/audio/audio_sink.*`, `atrac_decoder.*` | Output ring (SDL callback, `std::mutex`), ATRAC3/3+ decode through libavcodec | SDL3, FFmpeg | 350 + 144 | **V** |
| `host/movie/` | PSMF header/demux (P), H.264 decode through libavcodec (V) | FFmpeg | 543 | demux **P**, `avc_decoder.cpp` (131) **V** |
| `host/save_data/`, `host/crypto/` | PARAM.SFO, savedata store/crypto (AES), PGD | `<filesystem>`, tiny_aes | 1,503 + 338 | **P** |
| `host/ui/`, `input/`, `settings/`, `camera/`, `perf/`, `platform/`(Android), `install/`, `fonts/`, `adhoc/`, `main.cpp`, `system.cpp`, `overlays.cpp`, `camera_probe.cpp`, `profile.*` | Interface, input mapping, installer, Android glue, ad hoc multiplayer, camera/aspect hacks, per-game profile (`profile.hpp` 322) | SDL3, ImGui, sockets | 6,656 + 1,829 + 754 + 232 + 911 + 634 + 1,345 + 792 + 3,995 + 2,813 | **N** (replace by about 1.5 k lines of Vita shell; keep the `GameProfile` idea) |
| `apps/portablekit/` | Desktop launcher, `dlopen` corpus loader | SDL3 | 4,212 | **N** |
| `third_party/` | imgui, stb_*, tiny_aes, xxhash | | 83,108 | **N** except tiny_aes/xxhash |
| `tests/` | Codegen tests, savedata, adhoc | | 6,229 | **P** (desktop only) |

Totals (excluding tests and third_party): about 66 k lines. Roughly 22 k are reusable with little or no change, about 12 k must be rewritten (renderer 8.4 k, kernel execution model, dispatcher, audio sink, decoders), and about 32 k are not needed on Vita.

**Layering facts that matter for the port.**

* Generated code sees only `CorpusRuntime`, `AotFastView` and `AllegrexContext` (`include/psprecomp/corpus_abi.hpp:40-77, 185-230`); everything else is behind function pointers (`CorpusHostApi`). This is a clean seam: a new generator can target a different ABI without touching `host/`.
* A game is a `portablekit::GameProfile` (`host/profile.hpp`); the bring-up profile for Infected sets `guest_ram_bytes = 32 MiB` and `frame_vblanks = 2` (that is, 30 fps pacing is assumed by the profile) (`/work/infected/port/host/infected_profile.cpp:110,117`).
* The generated corpus is a shared object loaded with `dlopen` on desktop (`cmake/PortableKit.cmake:546, 611`, `host/corpus_module.cpp.in`). Vita has no `dlopen` of arbitrary code; the corpus must be linked statically into the eboot (the cmake already supports a static OBJECT library, `PortableKit.cmake:368-376`).
* Host code takes the whole `AllegrexContext` by reference in every HLE function (`hle.add(lib, name, [](Runtime&, AllegrexContext&) {...})`), reads arguments from `ctx.gpr[4..11]` and finishes with `kernel().finish(ctx, result)` or `kernel().block(ctx, wait, result)`. That is the interface a lean generator has to marshal into (D.3).

---

## B. Threading, scheduling, timing

### B.1 What exists today

* **One host thread runs all guest threads, cooperatively.** The execution model is stated in `host/kernel/kernel.hpp:18-30`: every HLE import runs at the outer dispatch level; blocking or preempting saves the caller's `AllegrexContext` (`pc = $ra`, `v0 = result`), loads another thread's context into `ctx` and changes the runtime thread identity; the generated import wrapper sees the identity change and leaves `ctx.pc` alone. A "thread switch" is a context copy: `Kernel::switch_to` (`kernel.cpp:315-322`), `save_current` (`:307-313`), `block` (`:352-371`), `finish` (`:324-345`, preempts only when a higher-priority thread became ready), `schedule` (`:407-441`).
* **Imports always unwind native frames.** The generated import wrapper is `invoke_import_cached(...)` followed by "leave pc alone if the identity changed" (`tools/codegen_main.cpp:1283-1295`). Chained direct calls between units check `chain_context_invalidated_` after returning (`corpus_abi.hpp:369-374`), so every native caller frame is discarded when a scheduler boundary switched thread. This is the property the lean generator cannot keep (function-granular C calls cannot be unwound by returning), see B.2.
* **No instruction-level preemption.** The only preemption is the starvation hook: after `kStarvationInterval = 20,000` chained dispatches without an import (`kernel.cpp:106-107`; the counter is incremented per chained *call*, `corpus_abi.hpp:377-380`, so a loop without calls is not counted at all), `Kernel::on_starvation` (`kernel.cpp:721-740`) charges 1 ms of virtual time (`kStarvationQuantumUs`, `:28`), runs timers, delivers interrupts and round-robins equal priorities.
* **Virtual clock.** `now_us_` only advances when every thread waits (`schedule` → `advance_clock(next_event)`, `:407-441`, `:502-514`), by the starvation quantum, or by `charge_busy_time` (>100 ms without a wait, `:700-719`). It is paced to real time with `std::chrono::steady_clock` and `sleep_until` (`pace_to_real_time`, `:516-570`; skipped with `<PREFIX>_NO_RENDER` or the unthrottled setting). Vblank is an event every 16,683 us (`kernel.hpp` `kVBlankPeriodUs`), `Kernel::on_vblank` (`:966-992`) wakes `WaitType::VBlank` waiters and queues vblank sub-interrupt handlers.
* **Interrupts, callbacks, alarms.** `queue_interrupt` (`:934`) only queues. A queued call starts at the next import return (`finish`, `:324-345`) or starvation boundary: `begin_pending_interrupt` (`:994-1008`) replaces `ctx` with a pristine context, `$ra = kInterruptReturnStub`, and the interrupted context is restored by `interrupt_return_stub` (`:1010-1036`). GE signal/finish callbacks are queued from `run_ge_list` (`hle_media.cpp:179-190`). Callbacks of `sceKernelCheckCallback` use `deliver_callbacks` (`:947-964`). Host-to-guest calls (`call_guest`, `:1043-1050`) also work by planting a return stub and letting the import wrapper return.
* **Host threads besides the executor.** `stall_watchdog` (detached `std::thread`, `stall_watchdog.cpp:108`); the SDL audio callback with a `std::mutex` ring (`audio_sink.cpp:96,186,255,300`); a texture decode pool of `clamp(cores/2, 1, 3)` threads (`vulkan_renderer.cpp:570-635, 4221-4224`); a pipeline prewarm thread (`:4803`); a font scan thread (`game_font.cpp:629`); UI/ad hoc threads (not needed). **The GE is not threaded**: `sceGeListEnQueue` and `sceGeListUpdateStallAddr` walk the whole list inline on the guest thread (`hle_media.cpp:726-747`, `run_ge_list` `:169-211`), `sceGeDrawSync`/`ListSync` only erase finished lists (`:749-760`).
* **What Infected does** (from the bring-up harness, `/work/infected/stats/s11/calls_per_frame_*.txt`, `stats/s18/frames_*.csv`): guest threads named `main`, `readThread`, `soundThread` plus the interrupt context. During FMV `readThread` wakes up ~166 times per 16.7 ms (`sceKernelDelayThread` polling around `sceMpegRingbufferPut`); `main` makes 22 to 47 HLE calls per flip in menus and about 32 in gameplay. The game is not vblank-paced (it reads `sceKernelGetSystemTime` pairs and calls `sceKernelDelayThread` once per frame; `BRINGUP_LOG.md`), i.e. it is time-step driven.

### B.2 What has to change on Vita

1. **No `ucontext`, no `fork`, no `mmap`.** Guest threads need their own host stacks switched by a small ARM routine (saves r4-r11, d8-d15, sp, lr; about 30 lines of assembler plus a C++ `Coroutine` wrapper of about 150 lines). PortableKit does **not** have per-guest-thread host stacks; it relies on unwinding to the dispatcher (B.1). The lean generator (D) makes unwinding impossible, so this is mandatory, not an optimisation. Stack size per coroutine: 64 to 128 KB (native frames of lean functions are small: measured prologue allocation over all 9,247 emitted function symbols (9,229 functions plus 18 compiler clones), `-O2 -mthumb`, unmasked: median 20 B, mean 28 B, p90 64 B, p99 128 B, max 312 B [M], D.3; guest stacks live in guest RAM, not on the host stack; rule of thumb: native stack = max(64 KB, half the guest thread's stack size)). No guard pages without `mprotect`: use a canary word and a depth counter in debug builds.
2. **Blocking HLE becomes a coroutine switch.** `Kernel::block()` today saves context and *returns* so that the import wrapper can leave. In the coroutine model `block()` suspends the calling coroutine and returns when the thread is woken; `wake()` already writes the result into `thread.context.gpr[2]` (`kernel.cpp:373-380`), which maps to "the value `block()` returns". There are only 24 call sites of `block`, `delay_current` and `wait_host` in `host/hle` and `host/kernel` (`grep`, [M]); in `hle_threadman.cpp` (lines 93, 119, 130, 167, 276, 386, 539, 600, 826, 924, 1171, 1373, 1554, 1602) and `hle_media.cpp` (`:643-680` vblank waits, `:560-640` ctrl reads) each one is the last statement of its lambda, so the "suspend and resume inside the call" semantics need no HLE changes. Estimated change: 300 to 500 lines in `kernel.cpp` plus the thread-start path in `hle_threadman.cpp`.
3. **Interrupts and callbacks run as nested native calls** at the same safe points (import return, yield points) instead of replacing `ctx`: `run_guest_call(fn, args)` is just a call through the flat function table. Handlers cannot block (PSP semantics), so they cannot switch away; after they return, the existing "is a higher priority thread ready" check in `interrupt_return_stub` (`:1010-1036`) decides about a switch. `call_guest` becomes a direct call too (simpler than the return-stub dance).
4. **Yield points in loops.** Today pure loops never yield (the starvation counter counts calls). A guest thread spinning on a memory flag set by an interrupt handler would hang in both the old and the lean model. Lean form: a decrement-and-branch on back edges of loops that contain no call and no store to non-stack memory (about 2 ARM instructions per iteration, [E] from the structure; implemented nowhere yet).
5. **Clock.** `std::chrono::steady_clock` and `std::this_thread::sleep_for` link on the Vita toolchain (link test `experiments/hazard/h1.cpp`, [M]); whether they are cheap and monotonic at microsecond resolution is [U]. Use `sceKernelGetSystemTimeWide` directly for `now_us()`. Pace to the real display vsync (GXM display callback) instead of `sleep_until`, so frame pacing is exact at 59.94 Hz and no 1 ms sleeps are needed; `pace_to_real_time` (`:516-570`) is replaced by about 40 lines.
6. **Core assignment (given in the task, consistent with the code structure).** Core 0: guest execution + kernel + HLE (all guest threads multiplexed, single host thread, as today). Core 1: GE worker: list walk, state tracking, vertex setup and GXM submission (one thread owns the GXM context). Core 2: audio mixer (SAS + `sceAudioOut` thread), ATRAC decode, file IO. The GE worker needs a producer/consumer queue (SPSC ring of "list ready" records) with `sceGeListSync`/`sceGeDrawSync` blocking the guest thread (a `WaitType::Host`-style wait already exists, `kernel.hpp:~113, kernel.cpp:389-405`). **Semantic risk:** PortableKit completes every list synchronously at enqueue, so a game that never calls `sceGeDrawSync` before reusing a vertex or texture buffer works there and would race on a real asynchronous GE (also on PSP hardware, where the GE is asynchronous, so correct games sync; Infected's per-frame calls show `sceGeListEnQueue`, `sceGeListUpdateStallAddr` x2, `sceGeDrawSync` once per flip, `stats/s11/calls_per_frame_f1200.txt`). Keep a `GE_SYNC=1` switch that restores synchronous execution for debugging.
7. **HLE call rate.** The bring-up harness shows up to ~1,000 HLE calls per flip in some movie phases (readThread polling) and ~32 in gameplay. At 60 Hz the 1,000/flip case is 60 k calls/s; each call today goes through `invoke_import_cached` (`runtime.cpp:1276`: histogram check, `import_bindings_[slot]` lookup, `std::function` call) plus a `Kernel::finish` scheduling check, so a Vita-side dispatch must be a plain function-pointer table indexed by import slot and must not allocate (`std::string`, `std::map` lookups, `std::function` captures) on the per-call path. [E] 2 to 5 us per call is acceptable (about 12 to 30% of a frame at 1,000 calls, but only 1% in gameplay at 32 calls).
---

## C. Guest memory model

### C.1 What PortableKit does today

| Aspect | Fact (file:line) |
|---|---|
| Allocation | Two `std::vector<uint8_t>`: `bytes_` = guest RAM, 32 or 64 MiB (the constructor throws for any other size, `guest_memory.cpp:105-118`), and `vram_` = 2 MiB EDRAM + 16 KiB scratchpad in one buffer (`guest_memory.hpp:228-229`, scratchpad placed after EDRAM, `guest_memory.cpp:88-103`). No `mmap`, no guard pages, no fixed address. The inline fast paths cache `ram_data_` and three limits (`guest_memory.hpp:236-243`), so the object must never move (copy/move deleted, `:35-39`). |
| Address aliasing | `canonical(a) = a & 0x1FFFFFFF` (`guest_memory.hpp:53-55`) folds the cached (0x0), uncached (0x4) and kernel (0x8, 0xA) mirrors of RAM (0x08/0x48/0x88 in the task's wording) and of EDRAM. EDRAM is 2 MiB mirrored 4 times over 8 MiB (`kVramMirrorCount`, `guest_memory.hpp:29-30`), implemented by `(c - 0x04000000) & (2 MiB - 1)` (`guest_memory.cpp:127-131`). |
| Fast path | Inline, RAM only: `offset = (a & 0x1FFFFFFF) - 0x08000000; if (offset <= limit) *(ram_data_ + offset)` (`guest_memory.hpp:80-131, 186-188`). Anything below 0x08000000 (EDRAM, scratchpad) wraps to a huge unsigned offset and falls to the out-of-line `aot_*_slow` functions (`guest_memory.cpp:190-300`). Stores additionally test `write_watch_enabled_` unless `PSPRECOMP_AOT_ASSUME_NO_WRITE_WATCH` is defined (`:95-131`; both options default OFF, `CMakeLists.txt:64-67`). |
| Invalid accesses | The checked accessors `load8/16/32`, `store*` resolve through `contains()` and throw `Error("Guest memory access outside PSP RAM/EDRAM")` (`guest_memory.cpp:132-152, 451, 466, 480`). HLE code uses them for every guest pointer, which is the right behaviour to keep for HLE. |
| Write watches / dirty tracking | There is **no** write-watch or dirty-page mechanism for GPU or cache coherence. `write_watch_enabled_` is a debug aid armed by `PSPRECOMP_WATCH_WRITE` (`guest_memory.cpp:27-48, 105-107`), it only logs. Texture invalidation is by content hashing: `texture_key()` once per display list (`vulkan_renderer.hpp` `begin_display_list` comment; `texture_decode.hpp:33`). `CACHE` instructions are no-ops, `SYNC` is a fence (`codegen_policy.hpp:15-22`). So **no feature depends on write watches**, and none depends on `AotFastView::write_watch_enabled`. |
| 64-bit assumptions | None in the fast path (32-bit offsets). `contains()` uses `uint64_t` arithmetic (`guest_memory.cpp:132-141`) which is slow path only. `Runtime` uses `std::uint64_t` counters. |
| Self-modifying code | Not supported by static recompilation; only overlay libraries re-register code (`host/overlays.cpp`), which Infected does not use (single `BOOT.BIN`, level data is `.zpm` streams; firmware PRXs on disc are HLE'd by name). |
| Other RAM clients | The GE walker, vertex/texture decode and block transfers read guest memory through `memory.raw_pointer()` (`:354-371`) and checked loads. `aot_copy_lz_match` is an optional guest-memcpy-pattern accelerator (`:301`). |

### C.2 The cheapest correct 32-bit model

One flat 32 MiB arena that starts at guest address `0x08000000`, and an unchecked access through a mask, i.e. `host = g_ram + (addr & 0x01FFFFFF)`. Generated code is `LW(a) = *(u32*)MEMA(a)` (the `MEMA` macro of `experiments/lean.h`, variant `LEAN_MASK25`): one `bic.w r, r, #0xFE000000` (a single Thumb-2 modified-immediate instruction) and then a register-offset `LDR`. With `g_ram` a link-time constant the compiler holds the base in one register (or a `movw/movt` pair) outside loops.

**Measured cost of the mask** (whole program, 451,251 translatable guest words, same generator, `-O2 -mthumb -mcpu=cortex-a9`): without mask 679,191 ARM instructions / 2,049,698 B (1.505 insns, 4.54 B per guest word); with mask 768,047 / 2,337,676 B (1.702 insns, 5.18 B per word): **+13% instructions, +14% bytes**, i.e. +1 instruction per memory access that the generator could not prove canonical ([M], `experiments/lean_all_measure.log`). That run used the form `g_ram - 0x08000000 + (a & 0x1FFFFFFF)`. The recommended form below (`g_ram + (a & 0x01FFFFFF)`, no bias term) was measured afterwards on the six-function slice: 411 instructions / 1,340 B against 412 / 1,356 B for the 29-bit form and 358 / 1,152 B unmasked, i.e. the same cost, and slightly less ([M], `experiments/lean_six/lean_00_mask25.o`, `LEAN_MASK25`). Where this document quotes lean code size or speed, the **masked figures are the design point** and the unmasked ones the lower bound.

**Can the mask be dropped? Not safely, and it buys correctness.** Dynamically 33.0% of executed guest instructions are loads/stores; 32.3% of those are `$sp`-relative (the guest stack is in cached user RAM, always canonical) and none are `$gp`-relative, so eliding the mask for `$sp` bases removes about a third of the penalty (about +0.22 instead of +0.33 ARM instructions per guest instruction dynamically, [E]; dynamic mix from the PC histograms in D.5). The rest is needed because BOOT.BIN really uses the other address windows:

* **The scratchpad at its absolute address.** The two sibling texture swizzle routines at `0x089A5480..0x089A55AC` and `0x089A55B0..0x089A5710` build `0x00010000` with `lui $fp, 1` / `lui $23, 1` / `lui $4, 1` (`0x089A5498`, `0x089A561C`, `0x089A5640`): the second one calls the game's `memcpy(0x00010000, texture, n)` and then reads the scratchpad back with `lbu 0..15($4)`, 16 bytes per block row, storing into the texture in swizzled order (disassembly [M]). The PSP scratchpad has no allocation API; games use `0x00010000` directly. About 140 other `lui rX, 0x0001` sites exist in the image (65,536 is a common size or mask) and were not classified one by one; the dynamic address histogram (step 0 in I.4) is the way to find all users.
* **The uncached alias** `ptr | 0x40000000`: 15 sites (`lui 0x4000; or`, first at `0x08815504`) [M].
* **Kernel/uncached mirrors** 0x48/0x88/0xA8: none seen, free with the mask.

**Recommended memory function:** `MEMA(a) = g_ram + (a & 0x01FFFFFF)` (the 32 MiB arena starts at guest 0x08000000, so there is no bias term). On ARMv7 the mask is a single instruction (`bic.w r, r, #0xFE000000`, a modified-immediate encoding; +1 instruction per access, measured above). It folds the cached/uncached/kernel mirrors of RAM onto the arena **and** folds the scratchpad: `0x00010000 & 0x01FFFFFF = 0x00010000`, i.e. guest address `0x08010000`, which lies in the kernel partition (`0x08000000..0x087FFFFF`) that the user program cannot own on a PSP and that the framework uses only at `0x08100000` (boot arguments), `0x08200000` (interrupt stack top) and `0x08400000..0x087FFFFF` (volatile memory lent to the game) (`host/kernel/kernel.hpp:71-81`). So the scratchpad needs no extra allocation and no special case in generated code. That is the main reason to keep the mask even though it costs 13% in instruction count. It also makes unchecked access safe against the mirrors; it does not make it safe against invalid pointers (those still fault as data aborts).

**Behaviour on invalid accesses.** Unchecked means a guest bug becomes a host data abort instead of `Error(...)`. The PSP also faults on those accesses, so a correct game never does it; the interpreter and the PortableKit slow path are the debugging tools. The differential test (D.4) shows the practical consequence: 102 of 4,430+102 trials where the interpreter completed a random-pointer function had the lean code touch an address outside the arena (guest VRAM 27, scratchpad 2, other unmapped 73); none involved RAM.

### C.3 VRAM: the one real design decision

PSP code reaches EDRAM at `0x04000000` (+ mirrors, + the uncached alias `0x44000000`). With the 25-bit mask `0x04000000` folds to arena offset 0, which collides with the scratchpad fold (offset `0x10000`, inside the first framebuffer at `0x04000000..0x04088000`). So EDRAM must move. Options, cheapest first:

1. **Relocate EDRAM into a free block of the kernel partition** and hand the guest the new base through `sceGeEdramGetAddr`: BOOT.BIN imports it (NID `0xE47E40E4`, stub `0x08A899A4`, `tools/imports.tsv`), and a scan of the image finds no `lui` with a VRAM-looking immediate that is an address: the 18 `lui 0x0400` sites are bit masks (`and a0, a0, 0x04000000`; the single flag-test pattern near `0x088A6338-0x088A6360` was not followed to its end), the 5 `lui 0x44xx` sites (`0x0880CD20`, `0x0886DB84`, `0x088BFBFC`, `0x08935C78`, `0x08A262D4`) load float constants (`0x44160000` = 600.0f, `0x44098000` = 550.0f, `0x44000000` = 512.0f) [M, disassembly]. A free 2 MiB block exists at `0x08200000..0x083FFFFF` (between the interrupt stack and the volatile-memory area) [M from `kernel.hpp`], provided the interrupt stack (grows down from `0x08200000`) is moved or sized below it. **If the 18 mask sites turn out to be "is this pointer VRAM" tests on bit 26**, return `0x0C200000` instead: the 25-bit mask folds it onto the same arena offset but bit 26 stays set, and the GE's 28-bit address arithmetic (`GeState::relative_address`, `ge_state.hpp:~246`) can express it. The GE walker must then treat the relocated window as EDRAM. In the framework this is five constants plus the window test: `GuestMemory::kVramPhysicalBase` (`guest_memory.hpp:27`) with `is_vram_window/vram_offset` (`guest_memory.cpp:122-131`), `kEdramBase` (`hle_media.cpp:76`), the offset-to-VRAM promotion in `present_frame` (`hle_media.cpp:287`), `resolve_ge_address` (`ge_state.cpp:157`) and the `in_vram` test in `hle_sysmem.cpp:147` (a `grep` for `0x04000000` finds exactly these, [M]). The probe for the open question already exists in the framework, `PSPRECOMP_TRACE_VRAM_READS` (`guest_memory.cpp:161-181`), which reports CPU loads from VRAM per 64 KiB block, but only on the slow path of translated code; a run under the interpreter needs the same counter in `GuestMemory::load8/16/32` (about 10 lines). **Validate on desktop** (step 0): change the constants and boot to a mission with the existing interpreter and Vulkan renderer; the per-phase GE statistics (`ge_*.txt`, draws, targets, textures) must match the baseline run.
2. **Keep the true addresses with a hole.** Not viable: the scratchpad at `0x00010000` is 127.9 MiB below the RAM base and the Vita has no virtual reservation without commit (no `mmap`), so a contiguous 128 MiB block would be committed memory for 34 MiB of content (and the mask-free access it would allow gains at most the 13% above) [U on whether a block of that size is obtainable at all].
3. **Select by address at every access** (`cmp/ite` or a table): +2 ARM instructions per memory access, about +0.5 per guest instruction dynamically ([E]); unnecessary given (1).

Recommendation: (1), with the histogram and the VRAM probe as the gate; fall back to patching individual guest sites only if the game turns out to hard-code VRAM addresses.

### C.4 Which features depend on the slow path today

* CPU access to EDRAM or scratchpad (framebuffer clears or copies by the CPU, scratchpad use): `aot_*_slow`.
* Unaligned `lwl/lwr/swl/swr`: `aot_load_word_left/right` (`guest_memory.hpp:94-95`; BOOT.BIN has 358 static words of these four opcodes). They must be emitted as inline byte/halfword composites in the lean generator (two loads and a merge, [E] 4 to 6 ARM instructions).
* Block copy accelerators (`aot_copy_lz_match`), write-watch debugging, and error reporting on invalid addresses.
* Everything on the HLE and GE side uses `load32/store32/raw_pointer` (checked, throwing); these stay as they are and are not on the guest hot path.
* Cortex-A9 alignment: ARMv7 `LDR/STR/LDRH/STRH` tolerate unaligned addresses (the OS keeps `SCTLR.A = 0` on Vita, [U]); `LDM/STM/LDRD/VLDR/VLD1` with alignment hints do not. PSP `lw/sw` fault on unaligned addresses, so correct guest code is aligned; the lean runtime must not use `LDRD/LDM` for guest accesses and must use `vld1.32` without an alignment qualifier for `lv.q` (PSP `lv.q` requires 16-byte alignment, so the hint would be legal but is not needed).

---

## D. Code generator

Evidence labels in this section: **[M]** measured here with `arm-vita-eabi-g++ -std=c++20 -O2 -g0 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -fno-exceptions -fno-rtti` (GCC 15.2 from the vitasdk copy under `/work/vitasdk-root`), **[E]** estimated from measurements, **[U]** not verified. Nothing could be run on a Vita or in an ARM emulator, so every speed figure is derived from instruction counts, not timing.

### D.1 What `psp_recomp` emits today

All of this was read in `tools/codegen_main.cpp` and confirmed on the real generator output for BOOT.BIN (`experiments/gen/`, 162 units, produced in 42 s; `gen_run.log`).

| Aspect | Today (file:line, or measured) |
|---|---|
| Partitioning | Fixed 16 KiB spans of the executable (`unit_span_bytes 16384`), 162 units for BOOT.BIN, `unique_registered_block_entries 661,557` (every guest word is registered), `import_wrappers 234`, `emission single_copy_basic_blocks` (`gen/auto_codegen_report.json`) [M]. This is "unit-level", not function-level, emission. |
| Shape of a unit | One C++ function per unit, `<unit>_entry(CorpusRuntime&, AllegrexContext&, uint16 direct_entry_id, AotFastView&)` (`codegen_main.cpp:1068, 1099`), starting with `LOCAL_DISPATCH:` and a `switch` over dense entry ids with one `case` per guest word (`:1073, :1103`), then one `L_xxxxxxxx:` label per guest word. A 4,096-word function. |
| Registers | `ctx.gpr[n]`, `ctx.fpr[n]`, `ctx.vfpu[]` are members of the `AllegrexContext` that is passed by reference. Because stores go through `aot_mem` (a `uint8_t*`), the compiler must assume every store may alias `ctx`, so it reloads registers after stores. |
| Memory | `aot_mem.aot_load32(ctx.gpr[4] + imm)`: canonicalise (`& 0x1FFFFFFF`), subtract 0x08000000, compare with a cached limit, then branch to `aot_*_slow` (`guest_memory.hpp:80-131, 186-188`). `PSPRECOMP_AOT_ASSUME_NO_WRITE_WATCH` only removes the write-watch test of stores (`corpus_abi.hpp:102-126`); `PSPRECOMP_AOT_PRODUCTION_FASTPATHS` removes the observer/counter bookkeeping of chained calls (`corpus_abi.hpp:322-330, 365-372`). Both default OFF (`CMakeLists.txt:64-67`). Neither changes the model. |
| `jal` | Fixed target in the same unit: `goto L_target` after writing `$ra` (`codegen_main.cpp:1178-1184`). Cross-unit: `rt.invoke_chained_direct<&unit_fn, N, entry_id, target>(ctx, &aot_mem)` (`:965-970, 1209-1238`), an inline template with unit-disabled check, depth guard (`chain_depth_limit_ = 48`, `runtime.cpp:167`), a call through a function pointer, then "was the context invalidated?" and a starvation counter (`corpus_abi.hpp:318-380`). On return the caller continues only `if (... && ctx.pc == return_pc) goto L_return_pc`, so after every call there is a comparison against the expected return address. Calls to import stubs set `ctx.pc` and `return` to the outer dispatcher (`:1197-1202`). |
| `jr` / `jalr` | `jr` (including `jr $ra`): `local_pc = target; if (++local_transfers < 2048u) { entry_id = 0; goto LOCAL_DISPATCH; } ctx.pc = target; return;` (`:1241-1250`), i.e. every return is an entry-id table lookup plus a `switch` jump. `jalr`: `ctx.pc = target` and `rt.invoke_chained_call` (map/table lookup in `Runtime`) (`:1226-1240`). There is no jump-table recognition: a computed `jr` is the same dispatch. |
| Delay slots | Emitted inline before the transfer (`emit_regular(slot, pc + 4u)`, `:1170, 1205`). Branch-likely handled by branching around the slot. |
| FPU / VFPU | FPU is inline C++ on `ctx.fpr`. VFPU is lowered, not rejected: every VFPU instruction becomes a helper-heavy block (`ctx.read_vfpu_vector`, `apply_vfpu_source_prefix`, `write_vfpu_vector_with_destination_prefix`, `codegen_main.cpp:332-430` and the `Vfpu*` cases of `OpcodeKind`) with full prefix semantics; only truly unlowered opcodes become `rt.unsupported(...)` (`:1252-1260`; the generator reports 140 such sites, all guest `break` traps, `BRINGUP_LOG.md`). |
| Loops / yields | No loop counters. Preemption is only the chained-call starvation counter (`corpus_abi.hpp:374-380`), so a loop without calls never yields. |
| Exceptions | The generated code does not throw and compiles unchanged with `-fno-exceptions -fno-rtti` [M]: `generated_unit_0010.cpp` compiled with the flags above (with and without the two fast-path defines). The framework runtime does throw (H, hazard 2). |

**Measured baseline on the Vita compiler** [M]. The real unit `generated_unit_0010.cpp` (4,096 guest words, one function), `-O2 -mthumb`:

| Build of unit 0010 | `.text` bytes | ARM instructions in `.text` | bytes per guest word | compile wall time (1 core) |
|---|---|---|---|---|
| default | 328,780 | 107,239 | 80.3 | 147 s |
| `-DPSPRECOMP_AOT_PRODUCTION_FASTPATHS -DPSPRECOMP_AOT_ASSUME_NO_WRITE_WATCH` | 313,684 | 102,432 | 76.6 | 139 s |

Extrapolated to 661,557 words: **about 50.7 to 53.1 MB of `.text`** and about 6.6 core-hours of compile time for 162 units [E]. The task statement quotes 36.7 MB plus 2 MB of EH tables (about 55 B per guest instruction); I could not reproduce that figure with `-O2 -mthumb` (the EH tables of unit 0010 are tiny, 3.4 KB, because generated code has no throwing calls); At `-Os` the same unit is 270,556 B of `.text` and 82,036 instructions (66.1 B and 20.0 instructions per word, extrapolated 43.7 MB) [M], so the quoted 55 B/word is the same order of magnitude (different flags or an older emitter); the conclusion does not depend on which one is right. That the old model does not fit comfortably in a Vita process is not in doubt either way: 50 MB of code through a 32 KB L1 instruction cache and 512 KB L2 means about 25 ARM instructions per guest instruction in the hot paths.

### D.2 The lean generator (design, prototype, what is missing)

**Prototype.** `experiments/lean_gen.py` (790 lines of Python, reads the relocated image `work/image.bin`, emits C++, never runs it on a guest runtime) and `experiments/lean.h` (40 lines of runtime header). It is a prototype made to measure size and to be differential-tested, not a production tool.

**Principles**

1. **Function-granular emission.** The unit of emission is a guest function (seeds: `jal` targets plus prologue heuristics, 10,276 functions in BOOT.BIN; `psprecomp::analyze_program` already finds 12,520 seeds, `program_analysis.cpp:474`). One C++ function per guest function: `RET f_ADDR(u32 r29, u32 r4, ..., float f12, ...)`.
2. **Typed signatures from interprocedural liveness.** Parameters are the registers live at entry among `$4-$11`, `f12-f19`, plus `$sp` when read (75.4% of functions need `$sp`, because most frames hold spills). The return type is the intersection of `{v0, v1, f0}` with what callers read afterwards: of 9,229 translated functions, 5,505 return void, 3,545 `u32`, 109 `float`, 70 `u64` (`lean_all_emit.log`) [M]. Argument counts: 0:1,426, 1:2,197, 2:2,645, 3:1,422, 4:685, 5:277, 6:297, 7:160, 8:115 [M]. Direct calls are plain C calls: `f_0891ECD0(r29, r4)`.
3. **Registers are C locals.** `u32 r16 = ...`, `float f0`; GCC register-allocates them; `hi/lo` and the FP condition code are locals too. No `ctx` array in generated code.
4. **`jr $ra` is `return`, `jal` is a call, delay slots are folded** into the preceding branch (with the slot's reads hoisted correctly: a bug in exactly this fold was caught by the differential test, see D.4).
5. **Prologue/epilogue pairs are elided**: `sw $s0, N($sp)` / `lw $s0, N($sp)` and the `$ra` save/restore are removed when the slot is only touched by that pair (26,117 pairs, 68,790 instructions removed in BOOT.BIN [M]); the native ABI saves callee-saved registers instead.
6. **Masked, unchecked arena memory:** `LW(a) = *(u32*)(g_ram + (a & 0x01FFFFFF))` (C.2). The mask is part of the design, not an option: it folds the cached/uncached/kernel mirrors and the scratchpad that BOOT.BIN uses at `0x00010000` onto the 32 MiB arena. The prototype also builds without the mask (`g_ram - 0x08000000 + a`); that variant is reported as the lower bound only. Constants are folded into addressing modes by GCC.
7. **`jalr` through a flat table** `g_fn_tbl[(target - 0x08804000) >> 2]` (2.6 MB if complete; `lean.h`, `CALL_IND`).
8. **Conformance checks, not trust.** Each function is checked for the ABI assumptions in 4 below and, if it violates one, it is *reported and skipped*, never silently translated.

**How guest registers stay correct as C locals (the ABI assumptions and how each is validated)**

| Assumption | Check in the prototype | If violated |
|---|---|---|
| Callee-saved `$s0-$s7`, `$fp`, `f20-f31`, `$sp` are restored by every callee | Writes to a callee-saved register that are not matched by a prologue/epilogue pair mark the function "clobbers callee-saved" (183 functions, 15,113 words; 4.5% of dynamic instructions) | needs the "spill-all" form (below) |
| Caller-saved registers are dead across calls | Reads of a caller-saved register after a call without a redefinition: "reads clobbered" (180 functions, 17,756 words; 1.2% dynamic) | spill-all |
| `$ra` only appears in `jal`/`jalr`/`jr $ra` and save/restore pairs | "reads $ra" (10 functions, 2,918 words) | spill-all or special form |
| Callee returns only in the registers its callers read | "returns v0,v1,f0" (1 function, 20 words) | widen signature |
| Control flow stays inside the function | "flow leaves function" (290 functions, 8,992 words): tail jumps | emit `return f_other(args)` (planned, not done) |
| Indirect-call targets take the uniform `(sp, a0..a3)` signature | `CALL_IND` casts to `u64(*)(u32,u32,u32,u32,u32)` | **open item**: address-taken functions with float/extra args need a generated thunk (about 100 lines, planned) |

Spill-all form (not implemented): the function keeps all 32 GPRs and FPRs in a local array, takes/returns them through a small `RegFile*`, and callers of it spill what they need. It is the escape hatch that makes the generator total instead of 69% dynamic coverage; its cost is about 3 to 4 ARM instructions per guest instruction [E].

**Not yet in the prototype (all needed for full coverage, by dynamic weight from the bring-up agent's PC histograms, D.5):**

* VFPU (`lv.q`, `sv.q`, `vmmul`, `vdot`, `vtfm`, ...) 5.6% of executed instructions are VFPU arithmetic/load/store/moves; statically only 928 words (0.14%): a few hand-written kernels. Lowering plan below.
* `ext`, `ins`, `seb`, `seh`, `wsbh`, `bitrev`, `rotr` (0.68% dynamic; 321 static `special3` words), `clz` (4 words), `lwl/lwr/swl/swr` (358 words), `break` (107 words), `max/min` (already done), `ll/sc` (none), `syscall` (none), `cache` (no-op).
* Jump tables: 173 `jr` through a non-`$ra` register, each preceded by an `lw` of the target (BOOT.BIN scan [M]). 151 functions (44,550 words) contain one or more (2.0% dynamic). Plan: recover `sltiu bound; sll 2; lui/addiu base; lw; jr` into a native `switch`, with the relocated code pointers of the table as the case set (`program_analysis.cpp:301-323` already collects relocated data code pointers); fall back to `goto *table[...]` (GCC computed goto) when the bound cannot be proved.
* Loop yield counters (B.2.4), blocking-HLE coroutine calls, interrupt delivery, hooks for the hot natives (below), and the indirect-call thunks.

**VFPU on NEON.** The VFPU register file is 128 floats arranged as 8 4x4 matrices; a column vector is 4 consecutive floats, so `lv.q/sv.q` and column ops map to `vld1.32 {q}`/`vst1.32` on a `float vfpu[128]` that lives in the guest context (L1-resident; 512 B). Row vectors are strided: use `vld4`/`vtrn` or load via a transposed view. `vadd/vsub/vmul` are one `vadd.f32 q` each. `vdot`/`vhdp` use `vmul` + `vpadd` (3 instructions). `vmmul.q` (4x4 by 4x4) is 16 `vmla` with lane operands after 8 loads. Prefixes (`vpfxs/t/d`: swizzle, abs, neg, constants, saturation, write-mask) are set by preceding instructions: the generator tracks the prefix state through straight-line code (as the framework helpers already do dynamically, `allegrex_context.hpp:213`) and either specialises the following VFPU instruction or calls the generic helper when the state is not statically known. Estimate [E]: 4 to 8 ARM instructions per VFPU instruction for the common forms, which at 5.6% dynamic share adds 0.25 to 0.45 ARM instructions per guest instruction. Size: about 600 to 800 lines of generator + 300 of runtime header, reusing the existing helper semantics as the oracle (they are in the same repository and are what the differential test compares against).

**Hooks for hot natives.** `0x08A2F228` is the game's `memcpy` written as a byte loop: 6.64% of all executed instructions in the bring-up run (`work/hot_game.txt`) and 4.3M instructions per decoded movie frame (`BRINGUP_LOG.md`); `0x08A2F47C` (13 instructions, 1.04%) is the game's `strcmp` (disassembled), another native-hook candidate. Two other tiny hot loops are not libc: `0x08A24A98` (1.59%) is a highest-set-bit scan over 9 bits and `0x08987248` (1.26%) a byte-sum over a buffer; GCC compiles both well as they are. In the lean form a hook is an alias in the function table and a direct-call target (`memcpy` with NEON, `strcmp`): removing these libc loops takes about 7 to 8% of dynamic instructions (6.64% for `memcpy` alone, 1.04% `strcmp`). **Caveat from the bring-up log:** with both libc hooks on, the end of `intro.pmf` hangs under the interpreter (cause not found, "likely a thread interleaving the faster loops expose"), so hooks are a risk item, not a free win.

### D.3 Micro-experiment: current style versus lean form on real BOOT.BIN functions

Six functions, chosen for shape (not for how well the lean form does): `experiments/pick_funcs.py`. "Current style" = the exact text of the real generator output for those words (sliced from `gen/generated_unit_*.cpp` by `slice_unit.py`, wrapped in the same `LOCAL_DISPATCH` prologue), compiled with the Vita compiler; "lean" = `lean_gen.py` output, same functions, same compiler and flags. ARM instruction counts are from `arm-vita-eabi-objdump -d` of each function symbol (`insncount.py`); bytes are the symbol size (including its literal pool).

| Function | Shape | Guest words (span / lean-reachable) | Current style, default: insns / B | Current + fast-path defines: insns / B | Lean, no mask (lower bound): insns / B | **Lean, mask (design point): insns / B** |
|---|---|---|---|---|---|---|
| `0x088C9C1C` | leaf, loads/stores | 36 / 32 | 437 / 1,172 | 412 / 1,116 | **49 / 152** | 45 / 140 |
| `0x08826DFC` | short loop | 24 / 16 | 199 / 536 | 199 / 536 | **35 / 96** | 38 / 120 |
| `0x08833434` | byte loop (case-fold copy) | 20 / 14 | 225 / 580 | 212 / 552 | **20 / 60** | 23 / 76 |
| `0x08805C6C` | calls (14 calls, saves) | 73 / 54 | 1,806 / 5,474 | 1,615 / 4,842 | **87 / 244** | 93 / 268 |
| `0x08833AAC` | float-heavy | 87 / 76 | 969 / 3,474 | 969 / 3,474 | **72 / 260** | 86 / 304 |
| `0x08829A54` | float + pointer walk | 72 / 68 | 961 / 2,936 | 930 / 2,866 | **95 / 340** | 127 / 448 |
| **Total** | | **312 / 260** | **4,597 / 14,172** | **4,337 / 13,386** | **358 / 1,152** | **412 / 1,356** |

[M] all cells. The 25-bit form `g_ram + (a & 0x01FFFFFF)` that C.2 recommends was measured on the same six functions afterwards: **411 instructions / 1,340 B**, the same as the 29-bit masked column (412 / 1,356). Ratios, unmasked (lower bound): the lean form uses **12.1x fewer ARM instructions** (4,337 vs 358) and **11.6x fewer bytes** (13,386 vs 1,152) than the current style with the fast-path defines; 12.8x and 12.3x against the default build. Ratios at the design point (masked, 412 / 1,356): **10.5x fewer instructions, 9.9x fewer bytes** than the fast-path-defines build (4,337 / 412 and 13,386 / 1,356). In plain per-word terms: current style 13.9 ARM instructions and 42.9 B per guest word of span (note: slices are 1.8x smaller per word than the 76.6 to 80.3 B/word of a whole real unit, because small standalone functions compile better than one 4,096-word function; the slice figure is the *kind* to the current model, so the true ratio on the whole program is larger than 12x); lean 1.38 ARM instructions and 4.43 B per reachable guest word unmasked, 1.58 and 5.22 masked. Readability check on the byte loop (`0x08833434`, 13 guest instructions in the loop): the lean form compiles to a 10-instruction loop with `ldrsb`, one `cmp.w lr,#25` / `it ls eorls` case fold, `strb.w [r2,#1]!` and one conditional branch; the current style needs 225 instructions for the same function.

**Native stack frames** [M]: over all 9,247 lean function symbols (the 9,229 functions plus 18 compiler-split `.part.0`/`.constprop.0` clones; unmasked build) the prologue allocation (`push`/`vpush` + `sub sp`) has median 20 B, mean 27.7 B, p90 64 B, p99 128 B, max 312 B (`f_089FC24C`); 5,611 functions allocate under 32 B. This is what bounds coroutine stack size (B.2.1), because guest stacks live in guest RAM.

**Whole-program measurement** [M], lean form, all 9,229 translatable functions (451,251 reachable guest words) in 18 translation units (`experiments/lean_all/`, `lean_all_measure.log`):

| Memory model | ARM instructions | `.text` bytes | per guest word | compile wall time, 1 core |
|---|---|---|---|---|
| no mask (lower bound; not correct for BOOT.BIN, see C.2) | 679,191 | 2,049,698 | 1.505 insns, 4.54 B | 502 s |
| **mask `& 0x1FFFFFFF` + bias (design point; the final 25-bit form costs the same, C.2)** | 768,047 | 2,337,676 | 1.702 insns, 5.18 B | 516 s |

Whole-program ratio against the measured baseline of the current style (76.6 to 80.3 B per word): **about 15x smaller at the design point (5.18 B per word; 17x unmasked)** for the covered subset, and **about 47x less compile CPU** (8.4 minutes against about 6.6 core-hours [E from unit 0010]).

**Extrapolation to the whole of BOOT.BIN** [E]. The measured part covers 9,229 of 10,276 functions and 451,251 + 68,790 (elided) of 661,557 words. The rest, 113,911 words in skipped functions, will cost more per word: jump-table and tail-jump functions (53.5 k words) about 4.5 B per word [same density as translated code], functions that need VFPU or other extra opcodes (17.7 k words) about 10 B/word, spill-all functions (42.7 k words) about 12 B/word. That adds about 0.9 MB. **Total lean `.text` about 3.3 MB at the design point (masked; 3.0 MB unmasked)**, plus about 0.2 MB of jump-table data and the jalr function table (2.6 MB flat, or 0.13 MB if built as a two-level table over only the 10,276 function starts). Against about 52 MB for the current style.

**Instructions and bytes per MIPS instruction, summary**

| Form | ARM instructions per guest word | bytes per guest word | source |
|---|---|---|---|
| Current generator, whole real unit (default / fast defines) | 26.2 / 25.0 | 80.3 / 76.6 | [M] |
| Current generator, small functions (slice) | 13.9 | 42.9 | [M] |
| Lean, no mask (lower bound), translated 68% subset | 1.505 | 4.54 | [M] |
| **Lean, masked (design point), translated 68% subset** | **1.702** | **5.18** | [M] |
| Lean, masked, dynamic, whole game incl. VFPU/NEON and spill-all (executed instructions) | 1.7 to 2.0 | n/a | [E] |

The dynamic figure is lower than the static one for the translated part because the 68,790 elided prologue/epilogue instructions execute but cost nothing, and higher for the VFPU/spill-all part. It drives the CPU budget in D.5.

### D.4 Correctness evidence for the lean form

Differential test (`experiments/diff/difftest.cpp`): the emitted lean C++ for 317 leaf functions is built natively on x86-64 next to PortableKit's own interpreter (`src/interpreter.cpp`); each trial builds a random 1 MiB arena (small ints, pointers into the arena, floats, noise), random arguments, the same RAM image on both sides, then compares the registers the lean signature returns and all of guest RAM except the stack. Result of the final run `diff/run_mask_postfix.log` (after fixing a delay-slot hazard in `jr $ra` that this test itself found): **4,430 of 4,430 compared trials equal, 317 of 317 functions all-equal, 0 mismatches**; trials where the interpreter faulted or exceeded its 40,000-instruction budget are counted separately and excluded. Additionally 102 trials where the interpreter completed but the lean code touched an address outside the arena (guest VRAM 27, scratchpad 2, other unmapped 73, RAM 0): a correct game does not do this, but it shows what unchecked mode does with a bad pointer (a data abort on Vita), see C.2.

Limits of the evidence: leaf functions only (callees are not linked), no VFPU/jump tables/spill-all yet, random rather than game-realistic inputs, x86-64 host (the semantics of the helpers `TRUNC_W`, `CVT_W`, `DIV_S` are portable C but ARM's `vcvt`, NaN canonicalisation and flush-to-zero behaviour must be re-tested on ARM, [U]).

### D.5 Dynamic coverage and the CPU budget

Source of dynamic data: the bring-up agent's executed-PC histograms from the interpreter (`/work/infected/stats/s18/pc_f*.csv`, one per 200 flips; mission phases f3600..f7600 = 4,200 flips, 8.13 G executed guest instructions) [M], analysed by `experiments/dyn_profile.py` with the current prototype.

* **1,936,076 guest instructions per flip** on average in those phases.
* Mix of executed instructions: ALU 37.6%, loads 21.1%, stores 11.9%, branch/jump/call 12.1%, FPU 8.4%, nops 2.0%, mul/div 0.2%, unsupported-by-prototype 6.7% (VFPU arithmetic 2.72%, VFPU load/store 2.43%, ext/ins/seb/seh 0.68%, VFPU moves/branches 0.48%, other VFPU 0.44%).
* **Prototype coverage by dynamic weight: 69.1%** of executed instructions are in functions the prototype translates. The rest: 21.2% in functions with an unsupported opcode (most of it ordinary instructions *inside* a function that also contains a VFPU/ext/lwl instruction), 4.5% clobbers-callee-saved, 2.0% jump-table, 1.4% tail jumps, 1.2% reads-clobbered, 0.3% returns, 0.2% reads-`$ra`. So full coverage requires VFPU lowering, ext/ins/lwl-lwr and the spill-all form; none is conceptually hard.
* The hottest function is `0x08935F54` (941 instructions, 11.7% of everything) which contains VFPU code; second `0x089118B0` (748 instructions, 6.8%); third the byte-loop `memcpy` (6.6%). The 52 hottest functions are half of the executed instructions.

**CPU budget** [E, not measured on hardware]. 1.94 M guest instructions per flip times 1.7 to 2.0 ARM instructions each (masked design point, with VFPU/NEON and spill-all functions included) is **3.3 to 3.9 M ARM instructions per flip**. A Cortex-A9 at 444 MHz executes game-like code at about 0.5 to 0.8 instructions per cycle once the 32 KB L1 caches, the shared 512 KB L2 and branch mispredictions are included (no Vita-specific measurement exists to cite; this is the typical range for A9 on integer, pointer-chasing code). That gives **about 9 to 18 ms of CPU per flip on the guest core, against 16.7 ms at 60 Hz** (3.3 M at 0.8 IPC is 9.3 ms; 3.9 M at 0.5 IPC is 17.6 ms; the IPC range is an assumption, [U]). Removing the byte-loop memcpy/strcmp (7 to 8%) pulls it to about 8.5 to 16.5 ms, and eliding the mask for `$sp`-based accesses (not done in the prototype) saves a further 3 to 5%. The unmasked lower bound (1.5 per instruction, 2.9 M) would be 8 to 16 ms, so the mask costs about 1 to 2 ms per flip. Verdict: 60 fps in average gameplay is plausible, in the heaviest scenes it will not be guaranteed; the game is time-step driven (`BRINGUP_LOG.md`: flips are paced by compute plus a 300 us delay), so a miss means a drop to 45 to 55 fps, not a slow-motion game. The guest core also pays for HLE (about 32 calls per flip in gameplay, negligible), the SAS render of `thread_sfx` (4 `__sceSasCore` calls per flip, F.1) and kernel overhead. The things that move this budget most, in order: IPC of the hot VFPU kernels (the 11.7% function), the I-cache footprint of the hot set (about 50 functions, a few hundred KB of ARM code, larger than the 32 KB L1I), the mask (+13% static, about 1 to 2 ms), spill-all functions, hooks.

**Why not a threaded JIT or a `ucontext` model:** the Vita forbids W^X JIT in normal processes (given), and the lean model already removes the dispatcher, so the remaining cost is the code itself.

### D.6 Where each change goes and how big it is

| Change | Where in the existing code | Size (estimate) |
|---|---|---|
| New C++ lean emitter: function discovery (reuse `analyze_program`, `decoder.cpp`), CFG with delay slots, interprocedural liveness, signature inference, conformance checks, native `switch` for jump tables, spill-all form, VFPU lowering, hooks, thunks | new `tools/lean_codegen.cpp`; replaces `emit_regular` (`codegen_main.cpp:88-890`), `emit_function_source` (`:1033-1281`) and `generate_auto` (`:1598-1821`); keeps the CLI, the profile and unit partitioning idea (partition by function groups, 18 TUs of about 25 k words compiled in 25 to 30 s each) | 3,000 to 4,500 lines C++ (prototype: 790 lines Python covering about 70% by static function count) |
| Lean runtime header (`MEMA`, loads/stores, FPU edge semantics, `DIV_S`, VFPU/NEON helpers, `CALL_IND`, yield counter, `hle_call`) | new `include/psprecomp/lean_abi.h`; replaces the use of `corpus_abi.hpp` for the Vita build | 400 to 600 lines (prototype `lean.h`: 40) |
| Import calls: marshal `jal` to an import stub into the HLE ABI (`AllegrexContext`): write `a0..a7`/`f12..` into a stack-local context, call `Runtime::invoke_import_cached(slot, ...)`, read `v0/v1/f0` back | new, plus `runtime.cpp:1276` unchanged; 234 import stubs, about 480 HLE functions untouched | 150 to 300 lines |
| Function table, hooks, address-taken thunks | generator output | 40 lines + generated tables |
| Coroutine kernel (B.2) | `host/kernel/kernel.cpp` (`block`, `schedule`, `switch_to`, interrupts), thread start path in `hle_threadman.cpp`, ARM `swap_context` | 300 to 500 lines + 40 lines asm |
| Remove from the Vita build | `Runtime::register_function`, `functions_`, `direct_*` tables, chained-call machinery, `AotFastView` (`runtime.cpp:430-481`, `corpus_abi.hpp:185-380`) | deletion |

**Static-site checks for the lean ABI inside the existing HLE.** HLE functions take `(Runtime&, AllegrexContext&)` and read arguments from `ctx.gpr[4..11]` and write results with `kernel().finish(ctx, v)`; that ABI is kept as the import marshalling target, so nothing in `host/hle` has to change for the new generator. The single exception is code that *returns into guest code* from inside HLE (callbacks, `call_guest`, interrupt handlers), which becomes a direct call of a lean function through the function table (B.2.3).

**Answers to the specific questions asked.**
* Function-granular emission: no today (unit-level, `single_copy_basic_blocks`), yes in the lean design.
* `PSPRECOMP_AOT_PRODUCTION_FASTPATHS` / `ASSUME_NO_WRITE_WATCH`: both exist, both default OFF, neither is a different code model: together −4.5% instructions and −3% bytes on unit 0010 [M]. Nothing depends on the write watch (C.1).
* `-fno-exceptions` compatibility of generated code: yes [M]; of the framework runtime: no (H, hazard 2).
* Blocking syscalls and per-thread host stacks: PortableKit has none and cannot (B.1); the lean design needs them (B.2.1-2).

---

## E. GE (graphics) pipeline and a Vita backend

Vita-side API facts in this section (GXM, shader toolchain, memory behaviour) are **[U]**: no Vita SDK headers beyond the compiler were used, nothing was run on hardware, and they are from knowledge of the public SDK and homebrew practice. Facts about PortableKit and about what Infected draws are **[M]** (read in the code, or taken from the bring-up agent's `stats/s18/ge_*.txt`, 38 phase files of 200 flips each).

### E.1 From `sceGeListEnQueue` to pixels

1. **Guest call.** `sceGeListEnQueue` / `sceGeListEnQueueHead` are one lambda `enqueue` (`host/hle/hle_media.cpp:726-738`, it allocates the list id and calls `run_ge_list` synchronously); `sceGeListUpdateStallAddr` (`:739`) moves the stall pointer and runs the list again; `sceGeDrawSync/ListSync` (`:749-760`) only retire finished lists. `sceGeSetCallback` (`:714`) stores signal/finish callbacks.
2. **Whole list walked inline, synchronously.** `run_ge_list` (`hle_media.cpp:169-211`) installs three sinks on the global `GeState` (signal, draw, transfer) and calls `GeState::execute(memory, pc, stall, finished)` (`host/gpu/ge_state.cpp:1351`). The walker reads 32-bit commands from guest memory until `END`/stall; `handle_command` (`:590`, a switch over the 256 command bytes) updates `registers_[256]` and the state structs (`TextureState`, `BlendState`, `DepthState`, `AlphaTestState`, `ViewportState`, `LightingState`, `FogState`, matrices, `bone_matrices_[96]`, `ge_state.hpp:19-176, 295-330`). `CALL/RET`, `BASE`, `OFFSET`, `ORIGIN` are handled in the walker. Addresses are resolved by `resolve_ge_address` (`ge_state.cpp:157`).
3. **IR boundary.** `PRIM` runs `draw_primitive` (`ge_state.cpp:994`): it decodes the vertex type (`vertex_format`, `ge_state.hpp:218-228`) and either (a) decodes vertices on the CPU into `std::vector<Vertex>` (`decode_vertices`, `ge_state.cpp:389`; handles s8/s16/f32 positions, tex, colour formats 565/5551/4444/8888, normals, weights, indices u8/u16, morph, and CPU skinning when `bone_matrices` is given), or (b) when `set_raw_vertices(true)` and the draw is a non-through, single-morph triangle draw with positions, sets `DrawCall::raw_vertices/raw_count/raw_stride/bone_matrices` to point at the guest's own bytes (`ge_state.cpp:1072`) for the renderer to decode on the GPU. `BEZIER/SPLINE` are tessellated in `draw_patch` (`:1204`). The result is a `DrawCall` (`ge_state.hpp:150-204`) handed to the draw sink: primitive type, vertices or raw bytes, `through` flag (already screen space), full texture/target/blend/depth/alpha-test/viewport/cull/lighting/fog state, `world/view/projection/texture_matrix`, and version counters for lighting/material. **No transform, no lighting, no clipping happens in `GeState`**: vertex positions stay in object space, matrices travel in the `DrawCall`.
4. **Block transfers** (`TRXKICK`) go to the transfer sink (`hle_media.cpp:138-150`), which first calls `renderer->read_back_framebuffer(source)` if the source is a framebuffer the renderer drew, then `memmove`s rows. Infected issues **zero** block transfers in all 38 phases [M, `ge_*.txt`], so this path is dead code for the port.
5. **Signals/finish** go through the signal sink (`hle_media.cpp:178-190`) which queues a guest interrupt callback; the `sceGeDrawSync` wait is trivial because everything is already done.
6. **Renderer.** `VulkanRenderer::submit(const DrawCall&, const GuestMemory&)` (`vulkan_renderer.cpp:5793-6685`, 890 lines) is where the backend begins: expansion to triangles/indices (strip/fan/sprite), merging consecutive draws, texture lookup (`texture_key` per list, `begin_display_list`, `:5787`; `decode_texture` or `snapshot_texture` + worker pool, `:4130-4196`), render-target management (`std::map<uint32_t, Target> targets`, `:1062-1090`, one image per guest framebuffer address, `copy` image for sampling a target as a texture), pipeline key and cache (`PipelineKey`, `:262-280`; blend/depth/cull/colour-mask/alpha-test/raw), uniform writes, `vkCmdDraw*`. Other entry points: `present(display_address, moment)` (`:7635`), `upload_frame` (`:5687`, movies), `write_back_frame` (`:6685`, copy the displayed target back to guest VRAM at 480x272 once per flip), `read_back_framebuffer` (`:6703`).
7. **What is Vulkan-specific** and must be replaced: all of `vulkan_renderer.cpp` (8,085 lines) except the pieces that are really state logic (target bookkeeping, texture-key caching policy, draw merging, the blend-factor reduction at `:290-330`), `host/gpu/shaders/ge.vert` (299 lines; transform, 4-light lighting, fog, 8-bone skinning, raw vertex decode), `ge.frag` (82 lines; texture function, specular add, fog, alpha test), the SDL window/pad code, the ImGui integration, frame interpolation, frame pacing, texture packs. **What is backend independent** and reusable as is: `GeState` (1,473 + 352 lines), `texture_decode.*` (529+34 lines), `vertex_format()`.

The data that the Vulkan renderer needs from outside `GeState` is small: a `GuestMemory&` for textures and raw vertex bytes, `portablekit::game()` (profile), settings, `perf::` counters.

### E.2 The exact interface a new backend must implement

`hle_media.cpp` refers to the renderer by type `gpu::VulkanRenderer` at about 25 sites (`:26, 127, 144, 165-209, 218-363, 397-402, 1109-1180`). The surface it uses:

| Call | Purpose | Notes for a GXM backend |
|---|---|---|
| `bool initialize(config, error)`, `shutdown()`, `available()` | device/window | create GXM context, shader patcher, display buffers (E.4) |
| `begin_frame()` (private use via `submit`), `begin_display_list()` | clear per-list texture key cache | start of every `sceGeListEnQueue` run |
| `bool gpu_decode()`, `check_gpu_decode()` | ask for raw vertices | return **true**/false: GXM reads native vertex formats, so the raw path is the natural one |
| `void submit(const DrawCall&, const GuestMemory&)` | the draw | the core of the backend |
| `void write_back_frame(GuestMemory&)` | copy the displayed frame into guest VRAM at 480x272; **called on every flip** (`hle_media.cpp:301`) | make it optional/off by default; see risk below |
| `void read_back_framebuffer(src, GuestMemory&)` | before GE block transfers | no-op for Infected (0 transfers) |
| `bool present(display_address, optional<time_point>)` | flip: show the target of `sceDisplaySetFrameBuf` | GXM `sceGxmDisplayQueueAddEntry`; paces to vsync |
| `void upload_frame(display_address, pixels, w, h, stride)` | movie/CPU-drawn frame | upload RGBA to a texture, draw full-screen quad into the display target |
| `pump_events()`, `pad()`, `sample_pad()`, `take_mouse_motion()` ... | input | replace by `sceCtrl` reading (about 150 lines) |
| `set_internal_scale`, fullscreen, aspect, texture pack, interpolation | settings | drop |
| `GeState::set_draw_sink / set_signal_sink / set_transfer_sink / set_raw_vertices` | wiring | unchanged |

Cheapest integration: keep the class name and the call sites, provide a `gpu::VulkanRenderer`-shaped class `GxmRenderer` with the subset above, and `#if` the rest of `hle_media.cpp` (display, ctrl, audio glue are mostly independent of the renderer type). An abstract `IRenderer` is cleaner but touches the same 25 sites.

### E.3 What Infected asks of the GE (measured on the interpreter run)

From `stats/s18/ge_f3600.txt, ge_f5000.txt, ge_f7600.txt` (gameplay, 200 flips each) and the menu phases [M]:

| Item | Gameplay value |
|---|---|
| Display list runs per flip | 3 (one `sceGeListEnQueue` and two `sceGeListUpdateStallAddr` per flip, each runs the walker; `sceGeDrawSync` once) |
| **Draws per flip** | **611, 635, 758** (menu: 16 to 36); 122 k to 152 k per 200 flips |
| Vertices per flip | 12.5 k to 20.3 k decoded; primitives 14 k to 25 k |
| Primitives | triangle strips 93% to 95%, triangles 4% to 7%, sprites 600 per phase (3 per flip) |
| Through-mode (2D) draws | 6% to 9% (8.7 k to 11.2 k per phase) |
| Indexed draws | 22% to 45% (u16) |
| Skinned draws | 2.6 k to 6.4 k per 200 flips (13 to 32 per flip), 1 to 8 bones, u8 weights; no morph |
| Lit draws | 38% to 66%; 1 to 3 lights enabled; fogged 43% to 80% |
| Textures | **CLUT4 (swizzled) 53% to 82%, CLUT8 (swizzled) 15% to 45%, 8888 linear (render-target copies) 1.6 k draws per 200 flips**; no 565/5551/4444 textures, no DXT; 186 to 197 unique textures per phase; sizes mostly 64x64, 32x64, 32x32, 128x64 |
| Texture function | TFX=0 (modulate), TCC=1, on 100% of textured draws |
| Blend | only four combinations: (src alpha, 1-src alpha); (2x src alpha, FIX 0/0); (src alpha, FIX white); (FIX white, FIX white); equation ADD only; 98% of draws blended |
| Alpha test | enabled on every draw: GREATER 0 on 89% to 93%, ALWAYS on the rest |
| Depth | test on 89% to 93% of draws, function GEQUAL on 99.6% of those; depth write on 43% to 79% |
| Targets | two 512-stride 8888 display buffers (`0x04000000`, `0x04088000`), depth `0x04110000`, plus three render-to-texture targets (`0x04154000` stride 480, `0x041D3800` stride 240, `0x041F3600` stride 120), textured back into the main passes (8 draws per flip sample a render target). These are the addresses the game sees today; with the EDRAM relocation of C.3 they shift by the same constant, and `resolve_ge_address` (`ge_state.cpp:157`) must map the new window back to EDRAM offsets, otherwise target identity (one image per guest address) breaks |
| Block transfers | 0 |
| Clear-mode draws | 0 (the game clears with its own draws) |
| Vertex types | 13 per phase of which about 8 carry the bulk (`0x000001FF`, `0x00001137`, `0x00001123`, `0x0001C3A1` skinned, `0x0080011E` through, ...); positions s8/s16/f32; texcoords u8/s16/f32; colours 8888 or 5551 or none; normals s8 |

Consequences: the **feature surface is narrow**, which makes a precompiled-variant approach viable (E.4); the **draw count is high** (about 36 k to 46 k draws per second at 60 fps), so per-draw CPU cost is the first-order problem, not shader complexity or fill; there is no CPU readback of VRAM visible in these statistics (the interpreter-side check `PSPRECOMP_TRACE_VRAM_READS` still has to be done under the interpreter, C.3).

### E.4 Outline of a GXM backend

*Context and surfaces*
* One `SceGxmContext` owned by the **GE worker thread on core 1** (B.2.6): `sceGxmCreateContext` with the default ring sizes (about 3 MB of ring buffers), a shader patcher, three 960x544 display buffers with a display-queue callback for vsync.
* One colour+depth target pair per guest framebuffer address as today (`targets` map): the two display buffers at 960x544 (exactly 2x the PSP's 480x272: HUD sprites land on 2x2 blocks), the three RTT targets (480, 240, 120 wide) at their native size (they are sampled back as textures; rendering them at 1x keeps the game's own blur/bloom chain meaning the same). Sampling a target as a texture after `sceGxmEndScene` needs no copy (the `copy` image of the Vulkan renderer is a Vulkan constraint) but needs a fragment-sync dependency [U].
* Depth: a 32-bit depth (or D24S8) surface per target; PSP depth is 16-bit with an inverted convention and GEQUAL is by far the dominant function: map the PSP viewport z scale/offset and depth range into the vertex shader's output z and use the matching GXM compare function (`GREATER_EQUAL`) [U on the exact clip-space z range of GXM; `ge.vert` already has a `view_z` push constant that does this for Vulkan].
* **4x MSAA at 960x544**: GXM implements it as a 2x2-supersampled surface, i.e. colour and depth are allocated at 1920x1088 and downscaled on resolve: 8.4 MB + 8.4 MB = **16.7 MB per MSAA target pair** [E from [U] layout], only for the two display targets (not for the RTT chain, which should stay at 1x, otherwise the blur passes change). Edges of alpha-tested geometry do not benefit; most of Infected's picture is alpha-tested sprites and trees, so MSAA buys little and costs fill and bandwidth: make it a setting, default off or 2x [E].

*Shaders* (variants, not an uber shader)
* GXM bakes **blend state and multisample mode into the fragment program** (`sceGxmShaderPatcherCreateFragmentProgram` takes a `SceGxmBlendInfo`), has **no constant blend colour**, no logic ops and (to my knowledge) no fixed-function alpha test. Infected's four blend combinations map to: (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) directly; (2x SRC_ALPHA, ZERO) as ONE/ZERO with the shader multiplying rgb by 2*alpha; (SRC_ALPHA, ONE) directly; (ONE, ONE) directly. FIX factors that are neither white nor black (the Vulkan renderer reduces them the same way, `vulkan_renderer.cpp:290-330`) do not occur in Infected; if a later game needs them, the shader must do the blend with the destination read back (no programmable blending in GXM that I know of [U]).
* Alpha test in the fragment shader with `discard`, as `ge.frag:50-76` does. On a tile-based deferred renderer (SGX543) `discard` defeats hidden-surface removal for those draws, and 89% of Infected's draws use GREATER 0. Reduction: when depth write is off and the blend equation makes alpha 0 invisible (SRC_ALPHA/ONE_MINUS_SRC_ALPHA, ADD), omit the discard (a pure optimisation, the output is identical); this keeps early-Z/HSR for the translucent majority [E].
* Vertex programs: one per (vertex type x lit/unlit x fog x skinned) combination actually used, created lazily by the shader patcher from the vertex attribute layout that `vertex_format()` yields. GXM attribute formats cover `U8N/S8N/U16N/S16N/F32` positions, texcoords and colours directly, so **the guest's vertex bytes are consumed in place** (stride, offsets from `VertexFormat`); the 5551 and 565 colours (about half of the lit geometry uses 5551) need unpacking in the shader from a `U16` attribute. Skinning: 8 bones x 3x4 as 24 `float4` uniforms and weights from a `U8N` attribute; lighting: the VS evaluates up to 3 lights (PSP lighting is per vertex) from a uniform block built from `LightingState`; fog per vertex. [E] 20 to 40 vertex programs and 8 to 16 fragment programs for Infected.
* **Precompiled GXP versus runtime compile.** The vitasdk toolchain compiles Cg to GXP through `libshacccg.suprx` (a Sony module dumped from firmware that the user has to install; widely used by vitaGL-based ports) at *run time* on the device, or offline with `psp2cgc` (Sony SDK, not redistributable). Recommendation [E/U]: write the shaders in Cg once, compile on first launch on the device with `libshacccg`, cache the resulting GXP files in `ux0:data/<app>/shaders/`, and from then on load only GXP; this needs the same user-installed module that vitaGL requires. An offline path would remove the dependency but I did not find a redistributable assembler (not verified).

*Vertex and uniform data.* Allocate three 4 to 8 MB write-combined rings in CDRAM/mapped memory per frame in flight (the Vulkan renderer reserves 3x32 MB + 12 MB for vertices and indices, `vulkan_renderer.cpp:85-99`, because of frame interpolation and a measured 16.1 MiB worst frame in other games; Infected's 20 k vertices per flip are about 0.5 MB at 24 B, so one region of 4 MB is ample). Per draw: `memcpy` the guest vertex bytes of the used index range (the Vulkan renderer already compacts sparse index ranges, `:5945-5965`), write an index list when the draw is indexed or a strip/fan that GXM draws natively as `TRIANGLE_STRIP/FAN`, fill the default uniform buffer (about 200 to 600 B: mvp, world-view for lighting, material, viewport, texture params), `sceGxmDraw`. Sprites (3 per flip) expand to two triangles.

*Textures.*
* **CLUT4/CLUT8 are native** in GXM (`P4`/`P8` ABGR formats with a palette pointer) [U], so the 94% of draws that use them need no RGBA expansion: unswizzle the PSP's 16-byte x 8-row swizzle into GXM's Morton order on the CPU at upload (table driven, one pass over 1 byte per 2 texels), expand the palette to 32-bit ABGR (CLUT format from `TextureState::clut_format`), upload once per texture key. `decode_texture`/`snapshot_texture` (`texture_decode.cpp`) already unswizzle and expand to RGBA8; the Vita variant needs a second output (indexed + palette), about 150 lines.
* DXT1/3/5 map to GXM's native BC1/2/3 formats (not used by Infected). 565/5551/4444 map to native 16-bit formats with a channel swizzle. 8888 is native.
* Cache: `texture_key` hashes the texel and palette bytes **once per display list per distinct texture state** (`vulkan_renderer.cpp:4130`, `texture_decode.hpp:33`). For Infected that is about 190 to 570 texture hashes per flip (each of the 3 runs clears the key cache), about 1 to 2 MB hashed per flip, 60 to 120 MB/s at 60 fps; at 300 to 600 MB/s for a 64-bit hash on a Cortex-A9 (a typical figure, not measured here) that is 10 to 40% of one core [E], on the GE core. If it is too much: hash only on first use and re-validate a rotating subset per frame (textures in a PSP game change rarely except RTT/movie buffers, which are known by address) [E].
* Capacity: 1,024 cached textures (`kMaxCachedTextures`, `:109`). At about 3 to 8 KB per P4/P8 texture (plus 1 KB palette) the whole working set is about 1 to 4 MB, trivial.

*Draw call cost: the real constraint.* 611 to 758 draws per flip are 37 k to 46 k draws/s at 60 fps. A GXM draw with a texture change costs several function calls into the driver library (`sceGxmSetFragmentTexture`, uniform reservation and fill, `sceGxmDraw`; program changes add more). [U] I would budget 6 to 12 us per draw on a 444 MHz A9 including the GE walk and vertex copy, i.e. **4 to 9 ms of the 16.7 ms on core 1** with the walk, decode, hash and submission serialised. Mitigations that exist in the Vulkan renderer and are portable: merging consecutive draws with identical state into one (the index-list merging noted at `vulkan_renderer.cpp:88-90`), sorting by state is not allowed (draw order matters), skipping redundant state sets. This number must be measured early with a stub renderer that only issues `sceGxmDraw` calls from a recorded `DrawCall` stream; it is the single most informative GPU-side experiment and can only be done on the device (or in Vita3K, whose CPU timings are not representative).

*What 960x544 with 4x MSAA changes.* Memory (16.7 MB per target pair, F/G), GPU bandwidth on resolve (the SGX resolves on tile writeback, so it is cheap relative to a desktop GPU [U]), the need to keep RTT passes at 1x, and nothing about the GE command stream: 960x544 is an exact 2x of the PSP resolution so scissor, viewport and through-mode vertex coordinates are multiplied by 2; the Vulkan renderer's internal-scale logic (`set_internal_scale`, `resize_targets`, `:5306-5366`) already does this and can be reduced to the constant 2.

*Expensive on Vita, ranked.* (1) per-draw CPU cost (above); (2) fragment shader `discard` on 89% of draws, losing HSR (mitigation above); (3) 4x MSAA fill/bandwidth if enabled; (4) texture key hashing per list; (5) render-to-texture passes: three RTT scenes plus the main scene plus HUD per flip means 4 to 5 `sceGxmBeginScene/EndScene` pairs per frame with synchronisation between the RTT write and the sampling pass; (6) per-vertex lighting and 8-bone skinning on the SGX vertex unit for 20 k vertices per frame is cheap (the GPU is not the bottleneck at this vertex count) [E]; (7) CPU readback of a render target is slow and is avoided only if the game never reads VRAM with the CPU: `write_back_frame` must be turned off in the Vita backend unless the VRAM-read probe finds a reader (C.3).

### E.5 Size and what can be tested on the desktop first

* New code: `GxmRenderer` about 4,000 to 6,000 lines (submit/state 1,500; shader set and variant management 700 plus 600 lines of Cg; texture cache and upload 700; targets and display 600; rings, init, present 500; glue 300), against 8,085 lines for the Vulkan renderer that also carries interpolation, texture packs, UI and replay.
* Desktop-first: (a) a **"GXM-shaped" reference backend on the existing Vulkan renderer's data flow**: indexed P4/P8 textures sampled in the shader, per-vertex-type variants fed with raw guest bytes, ONE/ZERO blend reductions with shader premultiply, discard policy; compare images against the current Vulkan output frame by frame (the bring-up harness has scripted input and frame hashing); this validates the *translation* choices, not GXM; (b) run the compiled GXM program under **Vita3K** on the desktop for smoke tests of the API usage (it implements GXM through its own translation layer; its accuracy for MSAA, `discard` performance and timing is not something to rely on) [U]; (c) the draw-stream recorder (record `DrawCall`s and texture bytes from a desktop run) so that the GXM backend can be driven on the device without the guest CPU running at all: the right way to measure per-draw cost and the GPU frame time before the lean recompiler is finished.

---

## F. Audio, ATRAC, SAS, movies, save data, input

What Infected imports (`/work/infected/tools/imports.tsv`, 234 stubs) decides the scope [M]: `sceAudio` 9 functions, `sceSasCore` 18, `sceAtrac3plus` 7, `sceMpeg` 25 (the ring-buffer API, no `scePsmf*`), `sceUtility` 14 (savedata 4, message dialog 4, netconf 4, system params 2), `sceCtrl` 3, `sceDisplay` 5, `sceUmdUser` 7, `IoFileMgrForUser` 15, `sceNet*`/`sceAdhoc*`/`sceWlanDrv` about 80 (multiplayer; ad hoc is not in scope for a first Vita port).

### F.1 Output path and SAS

* **Where.** `sceAudioChReserve/OutputBlocking/OutputPannedBlocking...` in `host/hle/hle_media.cpp:865-925` (`audio_output`, `:767-824`); `AudioSink` (`host/audio/audio_sink.cpp`, 350 lines) is a ring of monotonic frame indices filled by `mix()` and drained by an SDL audio callback under a `std::mutex` (`audio_sink.cpp:96,186,255,300`). `sceAudioOutputPannedBlocking` copies the guest buffer, converts to stereo s16, calls `AudioSink::mix`, then **blocks the guest thread for the buffer's duration in virtual time** (`kernel().delay_current(ctx, previous_end - now, frames)`, `hle_media.cpp:818-823`), so audio pacing is by the emulated clock, not by the device.
* **SAS.** `host/audio/sas_core.cpp` (615 lines) renders up to 32 VAG/PCM/noise voices with linear interpolation, ADSR (packed and explicit), at 44.1 kHz; `SasCore::render(memory, out, frames)` (`:533`) is pure integer C++ that reads sample data from guest memory (`advance_source`/`decode_block`) and has no external dependency. `__sceSasCore` renders one grain (default 256 frames) **synchronously into a guest buffer** (`sas_render`, `hle_media.cpp:826-858`; registered at `:1097`), which the guest then hands to `sceAudioOutput...`. Reverb is not modelled (sends accepted and ignored).
* **What Infected does** (`stats/s18/calls_per_frame_*.txt`): thread `thread_sfx` per flip: `__sceSasCore` x3 to 4, `__sceSasGetEndFlag` x4, `sceAudioOutputPannedBlocking` x1; thread `AT3_Stream`: `sceAtracDecodeData` x1 (music/voice) and `sceAudioOutputPannedBlocking` x1.
* **Cost.** The inner loop is about 40 to 50 ARM instructions per voice-sample (envelope step, interpolation, two multiplies for the pan, source advance) [E from reading the loop, not measured]. 3 to 4 grains of 256 frames per flip with 8 to 20 active voices (not measured; the SAS statistics counter `trace().voices` exists under `<prefix>_TRACE_SAS`) is **0.35 to 1.0 M ARM instructions per flip, i.e. 10 to 25% of the guest-core budget (D.5)**, and it runs on the guest core because `__sceSasCore` is synchronous.
* **Cheapest correct Vita implementation.** (1) Port as is: `sas_core.cpp` compiles with the Vita compiler [M, `-fsyntax-only` clean]. (2) Replace `AudioSink` by `sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, 256 or 512 grain, 44100, STEREO)` and a dedicated **audio thread on core 2** that pulls from the ring and calls `sceAudioOutOutput` (blocking) in a loop; the HLE side only writes into the ring and computes `queued_until_us` as today, so no HLE semantics change; about 150 lines replacing the SDL callback (no `std::mutex` needed: SPSC ring of frame indices with atomics). (3) Offload SAS if profiling demands: render grain N+1 on core 2 while the guest consumes grain N, i.e. `__sceSasCore` returns the previous grain's samples (one grain, 5.8 ms, extra latency; voice parameters take effect one grain later; end flags are polled by the game only to recycle voices). Risk: sample data in guest RAM that the guest overwrites while core 2 reads it (streamed voices): start with (1) and a NEON mix of the accumulate loop plus an envelope that is stepped every 16 samples while in sustain [E], which is a 3 to 5x saving without changing semantics.

### F.2 ATRAC3/3+

* **What the game plays.** All 75 `.at3` files on the disc (45 music tracks, 30 voice-overs, 72.6 MB) are **ATRAC3plus**: I read the RIFF `fmt ` chunk of every file straight from the ISO: all 75 are `WAVE_FORMAT_EXTENSIBLE` (0xFFFE) with the ATRAC3plus sub-format GUID (`bfaa23e9-58cb-7144-a119-fffa01e4ce62`), 2 channels, 44,100 Hz, block align 280 bytes (about 48 kbps) [M]. The 18 `.pmf` movies (243 MB) carry ATRAC3plus audio too (`PsmfDemuxer`: private stream 1 with an 8-byte header per frame).
* **API.** `sceAtracSetDataAndGetID`, `DecodeData`, `GetRemainFrame`, `GetStreamDataInfo`, `AddStreamData`, `GetInternalErrorInfo`, `ReleaseAtracID` (`host/hle/hle_atrac.cpp`, 919 lines; one `AtracDecoder` per id). The HLE keeps the PSP-visible state machine (buffer, loop points from the `smpl` chunk, remaining frames, sample offsets) and calls `AtracDecoder::decode(frame_bytes, int16_out)` once per 2,048-sample frame (`audio/atrac_decoder.hpp:29-46`: one frame in, interleaved s16 stereo out). The decoder is the only FFmpeg touch point (`atrac_decoder.cpp`, 144 lines, `AV_CODEC_ID_ATRAC3P`).
* **Vita hardware does not decode ATRAC3/3+ [G]**, so there are two ways:
  1. **A small bundled software decoder behind `AtracDecoder`.** The cleanest source is FFmpeg's `atrac3plus` decoder (`libavcodec/atrac3plus*.c`, `atrac.c`, tables; LGPL-2.1+), which means carrying about 3,000 to 4,000 lines plus its tables (about 40 KB) and a small MDCT/FFT, or a minimal FFmpeg build (`--disable-everything --enable-decoder=atrac3plus`) built with the vitasdk toolchain [U that this builds cleanly for `arm-vita-eabi`]. Cost [E]: ATRAC3plus stereo at 44.1 kHz costs a few tens of MHz on an A9 (not measured), 5 to 10% of one core per stream, two streams at most (music + voice-over), on core 2. The synchronous call pattern (one `sceAtracDecodeData` per about 46 ms of audio) can be turned into decode-ahead later. Licensing: LGPL needs the user to be able to relink; for an open homebrew project that is the usual situation, but it is a policy decision for the maintainer.
  2. **Offline conversion** (to AT9, AAC or MP3, which the hardware decodes) while keeping the `sceAtrac` API. It does not work without a software ATRAC3plus decoder anyway (the input is ATRAC3plus), so it would run once on the user's PC or on the Vita at install; the Vita-side runtime would then need an AT9 container handler instead of a decoder, plus a rewrite of the ISO content (the game reads the files itself and hands bytes to `sceAtracSetData...`, so a converted file must keep the 280-byte frame structure it looks for). More moving parts for a 5% CPU saving. **Not recommended** for a first version.
* **Recommended:** (1), with `AtracDecoder` kept as the seam so that a different decoder (hardware AT9 path, an optimised one) can replace it later. Verify on desktop first: decode all 75 files with the bundled decoder and compare against the FFmpeg output sample for sample (it is the same code, so exact).

### F.3 Movies (PMF)

* **What the game does.** `sceMpegRingbufferConstruct/Put/AvailableSize`, `sceMpegGetAvcAu/GetAtracAu`, `sceMpegAvcDecode`, `sceMpegAtracDecode`, `sceMpegAvcDecodeStop/Flush` (25 imports); the game's `readThread` pushes packs into the ring buffer (`sceMpegRingbufferPut` about 166 calls per 16.7 ms during playback) and its own threads pull access units and call decode. `sceMpegAvcDecode` writes the decoded picture **into a guest buffer** in a GE pixel format (modes 0 to 3, `hle_mpeg.cpp:464-490` `store_pixels`, with BT.601 YCbCr to RGB done per pixel in C++, `picture_row_to_rgba`, `:441`); the game then displays it itself (`sceDisplaySetFrameBuf`, or a textured quad), which the renderer sees as `upload_frame` when it is a CPU-drawn frame (`hle_media.cpp:287-306`).
* **Where.** `host/hle/hle_mpeg.cpp` (753 lines) = sceMpeg HLE (portable); `host/movie/psmf_demuxer.*` (178+80, MPEG-PS pack splitter, **portable**), `psmf_header.*`; `host/movie/avc_decoder.cpp` (131 lines, FFmpeg H.264).
* **Vita implementation.** Keep the HLE and the demuxer. Replace `AvcDecoder` with the hardware decoder (`sceVideodecInitLibrary/ CreateDecoder / Decode`, SceVideodec/SceAvcdec; takes Annex B access units, returns YUV 4:2:0 in a CDRAM buffer [U on exact API and on the module being loadable from a homebrew]), or with a small FFmpeg H.264 build: 480x272 at 29.97 fps baseline/main profile is about 5 to 15 MHz-equivalent on a NEON-enabled A9 [E]. Convert YCbCr to the requested guest format with a NEON loop (480x272 = 130 k pixels per frame, about 0.5 M instructions per frame, 15 M per second [E]). Run decode and conversion on core 2 by making `sceMpegAvcDecode` a kernel wait that the decode thread completes (`WaitType::Host`-style wait exists, `kernel.hpp`, `kernel.cpp:389-405`), so the guest threads are not stalled and the movie's other threads (`readThread`) keep running.
* **Offline transcode alternative:** convert the 18 PMFs to a hardware-friendly container at install time and play them with `sceAvPlayer`. Rejected: the game drives the ring buffer itself (its threads, its timing), so replacing the player means emulating that control flow anyway; and PMF audio is ATRAC3plus again.
* **Known hazard.** The bring-up log reports that with the libc `memcpy/memset` hooks the end of `intro.pmf` hangs under the interpreter (a queue entry that no thread pushes); the hooks were left off (`BRINGUP_LOG.md`). The movie player in the game (not the HLE) is sensitive to thread interleaving; expect the coroutine kernel to expose or hide this, test both.
* **CPU** [E]: the game's own byte-loop `memcpy` costs 4.3 M instructions per decoded movie frame (`BRINGUP_LOG.md`), i.e. 130 M/s at 30 fps in the lean form (about 190 M ARM instructions per second, 40% of a core) unless hooked; with a NEON `memcpy` hook it disappears. This is the strongest single argument for the hook.

### F.4 Save data

* `sceUtilitySavedataInitStart/Update/GetStatus/ShutdownStart` (`host/hle/hle_savedata.cpp`, 540 lines) implement the save dialog state machine and read/write `host/save_data/savedata_store.cpp` (238 lines; PARAM.SFO via `param_sfo.cpp` 154 lines; file list; optional PGD encryption in `crypto/pgd.cpp` + `savedata_crypto.cpp` 222 + `aes128.cpp` 19). Everything is `std::filesystem` plus byte buffers: **portable as is** (syntax check clean except a `strnlen` that needs `-std=gnu++20`, H). Infected calls it from the `main` thread once per flip in `sceUtilitySavedataGetStatus` polling.
* Vita: save under `ux0:data/Infected/savedata/<GAMEDIR>/` using the existing store; encryption is only needed to import saves from a real PSP (skip in v1). The message-dialog and netconf utilities need a 100-line text shim (draw with the GE or with GXM's debug font), not a port of `host/ui`.

### F.5 Input

* `sceCtrlPeekBufferPositive` (Infected's only reader; `hle_media.cpp:609`) writes a `SceCtrlData` (buttons, analog) from `sample_ctrl()`. On Vita use `sceCtrlPeekBufferPositive2` and map the Vita button bits to the PSP bits (the PSP has no L2/R2/R3/L3 and no second stick; the HD-release second-stick bytes the framework fills, `vulkan_renderer.hpp:25-33`, are irrelevant). The game polls the CROSS **release edge** to confirm (`BRINGUP_LOG.md`): keep edge semantics, not level, when remapping (the PSP's confirm button on Western releases is CROSS; Vita has the same layout). Touch/rear touch: not needed. About 100 lines.

---

## G. Footprint on Vita

Given: 256 to 365 MB of main RAM available to the application, 128 MB CDRAM (GPU-local), 32 KB L1 per core, 512 KB shared L2 [G]. All entries below are [E] unless marked [M]; nothing has been run on a Vita.

| Item | Size | Basis |
|---|---|---|
| **Guest arena (RAM, with EDRAM and scratchpad inside it)** | **32 MiB**, one allocation (today: 32 MiB RAM + 2 MiB EDRAM + 16 KiB scratchpad in two vectors) | profile `guest_ram_bytes = 32 MiB` (`infected_profile.cpp:110`), `guest_memory.cpp:88-118`; the 25-bit mask folds the scratchpad to arena offset `0x10000` and EDRAM is relocated to the free block `0x08200000..0x083FFFFF` (C.2, C.3 option 1) [M] |
| Same, if VRAM and scratchpad had to keep their true addresses (C.3 option 2) | about 128 MiB of address range, 34 MiB of content | not viable without `mmap`, not recommended |
| Generated `.text` (lean, complete, masked design point) | about 3.3 MB (3.0 MB unmasked) | D.3 extrapolation (measured 2.34 MB masked / 2.05 MB unmasked for 68% of the words) |
| Jump-table data + hook table | about 0.2 MB | 173 tables [M], 4 B per entry |
| `jalr` function table (flat, one pointer per guest word) | 2.65 MB (or 0.13 MB as a two-level table over the 10,276 function starts) | `0x2860D4 / 4` words x 4 B |
| Native coroutine stacks | about 0.7 MB (5 threads x 128 KB) | B.2.1; frames median 20 B, p99 128 B [M] |
| Framework heap (HLE table ~480 `std::function`, NID names 568 entries / 26 KB, kernel objects, ISO directory of 253 files, GE state, `DrawCall` scratch) | 3 to 5 MB | read from code; no per-guest-address structures remain after D |
| Texture cache (native P4/P8 + CLUT), 190 to 200 unique per scene, 1,024 cap | 1 to 2 MB typical, 8 MB cap | E.3, E.4 |
| Texture decode scratch (unswizzle, palette expansion) | 1 MB | |
| Vertex / index / uniform rings (GXM) | 3 frames x 2 to 4 MB = 6 to 12 MB | 20 k vertices per flip = about 0.5 MB; the Vulkan renderer reserves 108 MiB + 12 MiB (`vulkan_renderer.cpp:85-99`) because of interpolation and other games |
| GXM context rings, shader patcher, programs | 3 to 6 MB | default ring sizes |
| **Render targets, 960x544, no MSAA**: 3 display colour (3 x 2.09 MB) + 1 shared depth (2.09 MB) | 8.4 MB | |
| Render-to-texture chain (480x272, 240x136, 120x68 colour + depth) | 1.4 MB | `0x04154000/0x041D3800/0x041F3600` (E.3) |
| **With 4x MSAA on the display targets**: colour and depth at 1920x1088 | +14.6 MB (16.7 MB for the MSAA pair, minus the 2.1 MB single depth) | GXM layout [U] |
| Audio: sink ring (1 s stereo s16), SAS staging, 2 ATRAC3plus decoders (state + tables) | 0.5 MB | |
| Movies: PSMF demux buffers, decoded picture(s), hardware decoder buffers in CDRAM | 3 to 6 MB | |
| ISO read buffering | 1 MB | |
| **Total main RAM** | **about 57 to 80 MiB** | sum of the rows above, render targets excluded |
| **Total CDRAM / GPU-mapped** | **about 15 to 45 MB** (render targets 10 or 25, textures 2 to 8, rings if placed there) | |

Memory is not the constraint on a 256 MB budget. The *current* model would be a different story in the same budget: about 52 MB of `.text`, plus about 65 MB of per-address structures below, plus the same 80 MB as above, i.e. about 200 MB of the 256 MB without any headroom.

**Per-guest-address side tables in the current PortableKit (all to be removed in the lean model)** [M from code, sizes E on a 32-bit target]:

| Table | Where | Entries | Size on 32-bit |
|---|---|---|---|
| `Runtime::functions_` `unordered_map<u32, FunctionEntry{fn, std::string name}>` | `runtime.hpp:189`, filled by `register_function` (`runtime.cpp:430`) | **661,557** (one per guest word: `unique_registered_block_entries`) | **about 55 to 60 MB**: node (next 4 + key 4 + fn 4 + `std::string` 24, 8 B allocator overhead) is about 48 B, and every entry's name (`"recomp_unit_recomp_unit_0010"`, 28 characters, emitted per word at `gen/generated_unit_0010.cpp:19290-`) exceeds the 15-character SSO buffer so it is also a separate heap block of about 40 B; plus 4 B per bucket |
| `direct_functions_` and `direct_chainable_` (`vector<fn*>`, one per word, window grown in 1 MiB chunks) | `runtime.hpp:192-195`, `runtime.cpp:440-470` | 2 x about 0.75 M | 5 to 6 MB |
| `kEntryIds_*` (`uint16` per word, per unit) | generated, `codegen_main.cpp:1073-1103` | 4,096 x 162 | 1.3 MB |
| Generated registration code (`register_recomp_unit_N`: a call with a string name per word, about 20 B of code each) | `gen/generated_unit_*.cpp` | 661,557 | about 13 MB of `.text` |
| `generated_units_`, `generated_unit_disabled_` | `runtime.hpp` | per unit | small |
| `HleRegistrar::nids_by_name_` (`map<pair<string,string>,u32>`) | `hle_common.cpp:30-33` | 568 | about 60 KB |
| GE: `Target::guest_words` (one word per 256 B of each target), `list_texture_keys` | `vulkan_renderer.cpp:1062-1090` | thousands | under 100 KB |

So the existing registry costs about 65 MB of heap and 13 MB of code before a single instruction of the game runs [E]; the lean model replaces all of it by one 2.65 MB (or 0.13 MB) table.

**CPU-cache footprint** (not memory, but it decides speed): the executed hot set is about 50 functions for half of all instructions (`work/hot_game.txt`), which in lean form is a few hundred KB of ARM code, above the 32 KB L1I and below the 512 KB L2; data side, the guest RAM working set per frame is much larger than L1D. Keep the generated code compiled at `-O2`, not `-O3`; prefer `-Os` for cold files only after measuring [E].

---

## H. 32-bit / newlib hazards

Method [M]: (1) `arm-vita-eabi-g++ -std=gnu++20 -fsyntax-only -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -Iinclude -I. -Ihost -Ithird_party` over 46 translation units: all of `src/*.cpp` that the runtime needs (10 files) and 36 files of `host/` that are not UI, adhoc, installer, renderer or platform glue (kernel, ge_state, texture_decode, sas_core, audio_sink, hle_* except adhoc/net_offline, movie, save_data, system, main, profile). Logs: `experiments/syntax/*.log`. FFmpeg-guarded files (`atrac_decoder.cpp`, `avc_decoder.cpp`) compile in their no-FFmpeg configuration. (2) A link test of the libstdc++ facilities named in the task (`experiments/hazard/h1.cpp`, `h1.elf`). Nothing was run.

**Result: 44 of 46 compile clean at first attempt; the other two (`host/hle/hle_system.cpp:109`, `host/save_data/savedata_store.cpp:167`) fail only because `strnlen` is not declared under strict `-std=c++20` (newlib hides POSIX names); both are clean with `-std=gnu++20`.** With `-fno-exceptions` the core fails at the first `throw` (`src/guest_memory.cpp:108`), see 2 below. The link test (`std::thread`, `std::mutex`, `std::condition_variable::wait_for`, `std::atomic<uint64_t>`, `std::chrono::steady_clock`, `std::this_thread::sleep_for`, `std::filesystem::exists`, `std::map<u32, std::function>`, `printf`, a 64-bit division) **links** with `-lpthread`; without it `std::thread` fails to link.

| # | Hazard | Where (file:line) | Finding and action |
|---|---|---|---|
| 1 | `std::atomic<uint64_t/int64_t>` | `host/kernel/stall_watchdog.cpp:42-50` (watchdog, not needed), `host/fonts/game_font.cpp:330` | Both outside the port's needs. On ARMv7 8-byte atomics use `ldrexd/strexd` and are lock-free on a Cortex-A9; links [M]. No action. |
| 2 | **C++ exceptions** | 98 `throw` sites outside UI/adhoc (`psprecomp::Error` in `guest_memory.cpp` x7, `runtime.cpp` x7, `extra_instructions.cpp`, `elf32.cpp`, `sha256.cpp`, `nid_registry.cpp`, `program_analysis.cpp`, `hle_common.cpp`, `hle_extensions.cpp`, `system.cpp`, `iso_image.cpp`, `main.cpp` x8); `catch` at `runtime.cpp:218, 670, 757, 1064`, `hle_media.cpp:203` | **Error paths only, no exceptions as control flow on any hot path** (the checked guest accessors throw on an invalid address; `run_ge_list` catches a malformed display list, `hle_media.cpp:203`). Build `src/` and `host/` with `-fexceptions` (costs `.ARM.extab/.exidx` for those TUs, a few percent of their size), the generated code with `-fno-exceptions -fno-rtti` [M: compiles unchanged]. Put one `try/catch (const Error&)` in the import marshalling layer so that no exception ever unwinds through lean frames (they carry no unwind info). Alternative: turn `Error` into `stop()` + abort; invalid guest pointers then crash on a data abort anyway. |
| 3 | `size_t` is 32 bits | guest sizes are `uint32_t` throughout; `raw_pointer(addr, size)`; ISO offsets `std::streamoff` (64-bit) at `iso_image.cpp:78` | No narrowing hazard found in the core path: guest addresses and sizes are 32-bit by definition. `std::vector<uint8_t>` of 590 MB (the ISO) is never built (streamed). Large-file seek: newlib `off_t` width on Vita is [U]; the ISO is 590 MB, under 2 GiB. |
| 4 | 64-bit division and modulo | `kernel.cpp` (microsecond clock, `real_time_of`), `hle_media.cpp:822` (`frames * 1'000'000u / rate`), perf counters | Software `__aeabi_uldivmod` (tens to a hundred cycles); only on per-HLE-call and per-frame paths, never per guest instruction or per vertex. **Cortex-A9 has no hardware integer divide** [known ARMv7-A property, [U] for this chip]: guest `div/divu` (515 static words) become `__aeabi_idiv/uidiv` calls, 20 to 40 cycles each; dynamic count not measured, and compilers mostly strength-reduce constants. |
| 5 | `std::thread`, `std::mutex`, `std::condition_variable` | `host/audio/audio_sink.cpp:96,186,255,300` (SDL callback ring), `stall_watchdog.cpp:108` (detached thread), `save_transfer.cpp`, `savedata_crypto.cpp`, texture decode pool and prewarm thread in the Vulkan renderer (to be replaced) | Link and presumably work over vitasdk's pthreads [U at run time]. **Use native `sceKernelCreateThread` with a CPU affinity mask for the three workers** (core 0 guest, core 1 GE, core 2 audio/IO/decode): `std::thread` offers no affinity. Replace the SDL callback ring by an SPSC ring. Drop the watchdog. |
| 6 | `std::filesystem` | 30+ files, including `runtime.hpp`, `nid_registry.hpp`, `elf32.hpp`, `sha256.hpp`, `hle_io.cpp`, `savedata_store.cpp`, `iso_image.cpp` | Compiles and links (`exists` on `"ux0:data"` [M]). Whether directory iteration, `create_directories`, `rename` and `file_size` behave on `ux0:` paths is [U]; paths such as `ux0:data/x` have no root-name semantics in `std::filesystem` on a POSIX-like target but are passed through to newlib. Keep for the portable modules, put all Vita-specific I/O (`sceIoOpen`, `sceIoDread`) behind a 100-line shim used by `hle_io.cpp` and `iso_image.cpp`. |
| 7 | `std::chrono` | `kernel.cpp` (`steady_clock`, `sleep_until`), `hle_system.cpp`, `fast_loading.cpp`, `load_trace.cpp`, `frame_stats` | Links [M]. Resolution and cost of `steady_clock::now()` on Vita newlib is [U]; replace the kernel's pacing by `sceKernelGetSystemTimeWide` and GXM vsync (B.2.5). |
| 8 | `mmap`, `VirtualAlloc`, signals | only `host/fonts/game_font.cpp:145` (not needed) and `host/main.cpp:304-305` (SIGINT) | None in the core. Guest memory today is a `std::vector<uint8_t>` (`guest_memory.hpp:228-229`): on Vita allocate the 32 MiB arena (RAM, with EDRAM and scratchpad inside it, C.2-C.3) with `sceKernelAllocMemBlock` (page aligned, non-moving) and set `sceLibcHeapSize` high enough for the framework heap (about 8 to 16 MB; the default is small) [U]. |
| 9 | `__int128` | none [M, `grep`] | |
| 10 | Large static tables | the lean jalr table (2.65 MB const, `.rodata`), jump tables, `kEntryIds` (gone) | Fine for the eboot; keep the tables `const` so they stay in read-only pages, no static constructors. |
| 11 | `std::function`, `std::map`, `std::string` in hot paths | per HLE call: `import_bindings_[slot]` indexed lookup and one `std::function` call (`runtime.cpp:1276-1297`), no allocation; **12 sites in `hle_mpeg/psmfplayer/atrac` build a `std::string` argument per call even with tracing off** (`finish_traced(ctx, name, result, "packets=" + std::to_string(n), ...)`, e.g. `hle_mpeg.cpp:199`; SSO-sized so no heap, about 100 cycles); per draw: `std::map<TextureKeyInput, ListTexture> list_texture_keys` (`vulkan_renderer.cpp:1639`, one tree lookup per draw, nodes allocated per list), `GeState::DrawSink` is a `std::function` (one indirect call, no copy) | Replace the `std::map` by a small open-addressing table in the GXM backend; wrap the 12 `std::string` temporaries in `if (trace)`; otherwise fine at 32 calls per flip (gameplay) and 1,000 per flip (worst movie phase, F.3). |
| 12 | iostreams | 351 `std::cout/cerr` uses in 32 files (logging); `std::ostringstream` inside `finish_traced` only when tracing | Linking `<iostream>` pulls `ios_base::Init` and code (100 to 200 KB) and `cout` to the Vita console is slow; route logging through one `portablekit::log()` that writes to a file ring. |
| 13 | `getenv`-driven switches (`portablekit::env`) in hot constructors | many `static const bool x = env("...")` | Evaluated once per site; each later use is a guard-variable check. Harmless. On Vita there is no environment: replace `env()` by a settings lookup (one function). |
| 14 | Vulkan/SDL/ImGui/FFmpeg | `vulkan_renderer`, `audio_sink` (SDL), `host/ui`, `host/platform`, `apps/`, texture packs | Not compiled for Vita; the renderer and sink are replaced (A, E, F). |
| 15 | Alignment | `AllegrexContext` `vfpu[128]` (512 B) | Align to 16 for `vld1`/`vst1` with `:128` hints only if sure; the lean runtime uses unaligned-safe forms (C.4). |

**Overall verdict on portability of the C++:** high. The framework was written for 64-bit Linux/Windows/Android with C++20 and no platform assumptions in the parts that matter; nothing needs a rewrite for 32-bit reasons. The work is in the execution model (coroutines, no unwinding), the code generator and the renderer, not in porting hazards.

---

## I. Recommendation

### I.1 The choice

**Option (2): build a slim Vita runtime from selected PortableKit components, with a new lean code generator and a new GXM backend.** Do not port `host/` as it stands (option 1).

Why not (1), port `host/` mostly intact with a GXM renderer and a Vita platform layer:

* The part that cannot be kept is the execution core, not the periphery. The generated-code ABI (`CorpusRuntime`/`AotFastView`/`invoke_chained_*`) produces about 52 MB of `.text` for BOOT.BIN on the Vita compiler [M] and registers every guest word in a hash map with a heap-allocated name (about 65 MB of heap on 32-bit, G) [E]; it makes the kernel's blocking model depend on unwinding native frames (B.1); and it cannot run 60 fps on a 444 MHz A9 (25 ARM instructions per guest instruction, D.3). "Intact" would mean keeping exactly those parts.
* About 32 k of the 66 k framework lines are desktop-only (UI, installer, ad hoc, Android, texture packs, interpolation, 83 k lines of third-party code outside the count); about 12 k must be rewritten anyway (A).

Why (2) and not a from-scratch runtime (3): 22 k lines of the framework are reusable as is and they are exactly the expensive-to-get-right parts: the GE state machine (1.8 k, validated on this game's command stream by the bring-up agent), texture decode, SAS, the kernel's data model and wait semantics (2.8 k), 480 HLE functions through a clean `AllegrexContext` ABI (10 k), PSMF demux, save data, ISO reader, the Allegrex interpreter as a correctness oracle. All of it compiles with the Vita compiler today (H).

**What stays, what is new**

| Keep (compile for Vita, light edits) | New |
|---|---|
| `psprecomp_core`: decoder, ELF/PRX loader (install-time), NID registry, interpreter (debug/oracle), `AllegrexContext` | **Lean generator** (`tools/lean_codegen.cpp`) and lean runtime header |
| `GeState` + `DrawCall` IR, `texture_decode`, `vertex_format` | **GXM renderer**, shader set, texture/target/display management |
| `SasCore`, `AtracDecoder` seam, `PsmfDemuxer`, sceMpeg HLE | Vita audio sink thread, ATRAC3plus decoder, hardware H.264 glue |
| Kernel data model (threads, semaphores, event flags, mailboxes, timers, callbacks) | **Coroutine execution** (ARM `swap_context`), interrupts as nested calls, loop yields |
| HLE bodies (`hle_threadman`, `hle_sysmem`, `hle_system`, `hle_utility`, `hle_io`, `hle_savedata`, `hle_media` minus renderer glue, `hle_mpeg`, `hle_atrac`) | Import marshalling for lean code, native hooks (`memcpy`, `strcmp`) |
| Save data store, ISO reader, PARAM.SFO | Vita shell: eboot, memory arena, `sceIo` shim, input, timing, logging |

### I.2 Facts that drive the choice

* **Threading** (B): single host thread runs all guest threads; blocking is "save context and unwind to the dispatcher"; no per-thread host stacks. The lean generator needs real native stacks for guest threads: one coroutine per guest thread, `block()` becomes "suspend and resume inside the call", 24 call sites, 300 to 500 lines.
* **Memory** (C): one flat 32 MiB arena, unchecked access through the 25-bit mask `host = g_ram + (a & 0x01FFFFFF)`; no feature depends on write watches or the slow path except CPU access to VRAM/scratchpad. The mask is **mandatory**, not an optimisation: BOOT.BIN's texture swizzle routines use the PSP scratchpad at its absolute address `0x00010000` (`0x089A5480..0x089A5710`) and 15 sites build the uncached alias; the mask folds both onto the arena (scratchpad lands at arena offset `0x10000`, a free kernel-partition page). It costs +13% ARM instructions and bytes [M]. EDRAM is relocated into the free block `0x08200000..0x083FFFFF` via `sceGeEdramGetAddr` (BOOT.BIN imports it, one NID, five constants to change in the framework), because `0x04000000` would fold onto arena offset 0.
* **Generator** (D): the lean form (masked, the design point) is 10.5x fewer ARM instructions and 9.9x fewer bytes than today's model on six real functions (12.1x and 11.6x unmasked) [M], 1.70 ARM instructions per guest instruction and 5.2 B per guest word on 68% of the program (1.50 and 4.5 unmasked) [M], about 3.3 MB of `.text` for the whole game against 52 MB [E]; 8.4 minutes of compile CPU against about 6.6 hours [M/E]; differentially tested against the interpreter, 4,430 of 4,430 trials equal [M]. 69% dynamic coverage today; VFPU, `ext/ins/lwl/lwr`, jump tables and a spill-all form are required to reach 100% and none is conceptually hard.
* **GPU** (E): `GeState` yields a backend-independent IR; Infected's use of the GE is narrow (CLUT4/8 textures, modulate, 4 blend combinations, GEQUAL depth, up to 8 bones); the problem is **600 to 760 draws per flip**, not shader complexity.
* **Budget** (D.5, F.1): guest core 9 to 18 ms of 16.7 ms [E, IPC assumed], plus SAS 10 to 25% unless optimised; GE core 4 to 9 ms [E/U]; audio/decode core light.

### I.3 Target architecture on the Vita

| Core | Thread(s) | Work |
|---|---|---|
| 0 | `guest` (one host thread, N coroutines) | lean generated code, kernel scheduler, all HLE, SAS render, ATRAC3plus decode on first version |
| 1 | `ge` | GE list walker (`GeState::execute`), vertex copy, texture key + upload, GXM submission, present |
| 2 | `audio`, `io`, `decode` | `sceAudioOut` loop, ISO/file reads, movie demux + H.264 decode + YCbCr conversion, ATRAC3plus decode (moved here when profiling asks) |
| 3 | spare | OS / driver |

`sceGeListEnQueue` becomes a handoff: the guest thread publishes (list, stall) to an SPSC queue and continues; `sceGeDrawSync/ListSync` block the guest coroutine until the `ge` thread signals completion (a `WaitType::Host` wait already exists, `kernel.cpp:389-405`). A `GE_SYNC=1` mode keeps today's synchronous behaviour for debugging.

### I.4 Ordered work breakdown

Line counts are new or changed lines of C++/Cg/asm, tests included where noted. "Desktop" = can be built and tested on the x86-64 Linux box that already runs the bring-up harness.

| Step | Work | Lines | Desktop? |
|---|---|---|---|
| 0 | **Decisive experiments with the existing interpreter and Vulkan renderer**: (a) address-class histogram of every guest load/store over a mission (which non-RAM windows are used: scratchpad, uncached alias, VRAM; proves the relocation); (b) CPU-side VRAM-read probe (`PSPRECOMP_TRACE_VRAM_READS` equivalent in the interpreter, about 10 lines) to decide `write_back_frame`; (c) active SAS voice count and HLE call rates per flip; (d) record a replayable `DrawCall` + texture stream; (e) extract the ATRAC3plus decoder and compare against FFmpeg on all 75 files | 300 | yes |
| 1 | **Lean generator, production**: port `lean_gen.py` to C++ on `analyze_program`/`decoder`; add VFPU on NEON with prefix tracking, `ext/ins/seb/seh/wsbh/bitrev/rotr/clz`, `lwl/lwr/swl/swr`, jump tables as native `switch`, tail jumps, spill-all, indirect-call thunks, `break`, loop yield counters, hooks; lean runtime header; function and whole-program differential tests against the interpreter (leaf test exists) and a lock-step trace diff at every HLE boundary | 3,500 to 5,000 + 1,500 tests | **yes** (x86-64 builds the same C++; the masked arena works on a 64-bit host) |
| 2 | **Coroutine kernel**: `swap_context` (x86-64 asm and ARM asm), `block/delay/wait` as suspend/resume, interrupts and callbacks as nested native calls, loop yields feeding the starvation hook, `call_guest` as a direct call, GE worker handoff | 800 to 1,300 | **yes** (run the lean game on desktop with the existing Vulkan renderer: this is the full-system dress rehearsal) |
| 3 | **Vita shell**: eboot, arena via `sceKernelAllocMemBlock`, `sceIo` shim for ISO/saves, `sceCtrl` mapping, native threads with affinity, timing and vsync pacing, logging, boot with no GPU (frames discarded) | 1,500 to 2,000 | partly (shim interfaces yes, device calls no) |
| 4 | **GXM renderer**: first as a replayer of the step-0 recorded stream (measures per-draw cost and GPU time before the CPU side exists), then wired to `GeState` | 4,000 to 6,000 + 600 Cg | reference semantics yes (a GXM-shaped path on the Vulkan renderer), API no (Vita3K for smoke tests only) |
| 5 | **Audio**: `sceAudioOut` thread and ring, SAS as is, ATRAC3plus decoder behind `AtracDecoder`; **movies**: hardware H.264 behind `AvcDecoder` + NEON YCbCr conversion | 800 to 1,200 + imported decoder | decoder yes, device glue no |
| 6 | **Performance pass** against the budget: NEON mix in SAS, native `memcpy/strcmp` hooks, `$sp`-based accesses without mask, draw merging, texture hash policy, MSAA decision, `-O2` versus `-Os` per file | 500 to 1,000 | partly |

Total new or changed: about **14 to 20 k lines** (plus an imported ATRAC3plus decoder), against 66 k in the framework. Order rationale: steps 0 to 2 remove the largest unknown (CPU budget and correctness of the generated model) without any Vita hardware, and step 4's replayer attacks the second largest (draw cost) as early as the shell exists.

A staged fallback to get something running on the device early: lean code for the 69% of the game the prototype already covers plus the existing interpreter for the rest, accepted as slow in menus (about 0.6 M guest instructions per flip) and unusable in missions; it needs a register-file sync at each interpreter/lean transition (the spill-all form gives exactly that).

### I.5 Top risks (ranked)

1. **Guest-core CPU budget.** 3.3 to 3.9 M ARM instructions per flip (masked, 1.7 to 2.0 per guest instruction) at 0.5 to 0.8 IPC is 9 to 18 ms of 16.7 ms [E; neither the IPC nor any timing has been measured on a Vita]; SAS adds 10 to 25% and the game's byte-loop `memcpy` another 7% unless hooked. A miss means 45 to 55 fps in heavy scenes, not a crash. Mitigation: hooks, NEON SAS, mask elision for `$sp` accesses, measuring on the device with a stubbed GE as soon as step 3 exists. Cheapest early check: run the lean `.text` of the 52 hottest functions in a loop on a Vita (or any Cortex-A9) to measure IPC.
2. **Lean generator coverage and correctness** beyond leaf functions: VFPU lowering with prefixes, spill-all, jump tables, indirect-call ABI thunks, and the delay-slot class of bug (one was caught by the differential test). Mitigation: interpreter oracle, differential tests per function and lock-step per HLE call, mask-mode builds that trap bad pointers on desktop.
3. **GXM draw cost and shader toolchain.** 37 k to 46 k draws per second with per-draw state; `discard` on 89% of draws defeats HSR; GXP compilation needs the user-installed `libshacccg.suprx` or an offline path I could not confirm [U]. Mitigation: draw merging, discard policy (E.4), early replayer benchmark.
4. **Execution-model change** (coroutines, nested interrupts, no unwinding): deadlocks and races the old model hid; the one known sensitivity is the `intro.pmf` hang that appears with faster `memcpy` (BRINGUP_LOG). Mitigation: desktop dress rehearsal (step 2), keep `GE_SYNC` and a "single-step coroutine" debug mode, deterministic tests with scripted input.
5. **Unchecked memory, scratchpad and VRAM layout.** A stray guest pointer is a data abort. The scratchpad lands at arena offset `0x10000` (guest `0x08010000`), which must stay unused by the framework and the game's own heap; about 140 other `lui rX, 1` sites were not classified, and the 18 `lui 0x0400` sites might be "is this VRAM" tests that break when EDRAM moves (then use the `0x0C200000` variant of C.3). Mitigation: step-0 histogram and VRAM probe on desktop before any Vita work; a checked build (bounds trap) on desktop; a true-address model is not available as a fallback (C.3 option 2), per-site patching of the guest image is.
6. **No hardware ATRAC3/3+ and movies.** All 75 audio files and the movies' audio are ATRAC3plus; a software decoder (LGPL if FFmpeg's) costs CPU on core 2 and a licensing decision; the hardware H.264 module's availability to homebrew is [U]. Mitigation: software H.264 fallback (480x272), measure.
7. Texture hashing per list (10 to 40% of the GE core, E.4); `write_back_frame` per flip if the VRAM probe finds a CPU reader; frame pacing against a game that is not vblank paced (needs display-queue backpressure so it cannot run above 60 fps).
8. Vita-side unknowns that need a device: `std::filesystem`/`chrono` behaviour, `sceLibcHeapSize`, memory block types for guest RAM and CDRAM write-combining, cache coherency of mapped GPU memory.

### I.6 What can be built and tested on the desktop first

1. The lean generator and its differential harness (leaf-function harness exists; extend to whole-program lock-step against the interpreter at import boundaries): all of step 1.
2. The coroutine kernel with x86-64 context switching, running the lean game under the existing Vulkan renderer, SAS, FFmpeg movies and FFmpeg ATRAC: all of step 2; this reproduces the 60 fps CPU load question on x86 only as correctness, not speed.
3. The address histogram, VRAM probe, SAS voice counts, call rates, recorded `DrawCall` stream (step 0).
4. A GXM-shaped path through the current Vulkan renderer (native P4/P8, per-vertex-type variants fed by raw guest bytes, blend reductions, discard policy) with image comparison against today's output.
5. The ATRAC3plus decoder bit-exactness check on all 75 files.
6. The shell's interfaces (`sceIo` shim, input mapping, audio ring) with desktop implementations behind the same headers.

What cannot be validated without a Vita: CPU speed and IPC, GXM behaviour and per-draw cost, memory-block behaviour, hardware decoders, `libshacccg`, thermal and battery behaviour.

---

## Appendix. Experiments, reproduction, and what was not verified

### Experiment index (`/work/assess/experiments/`)

| Path | What it is |
|---|---|
| `lean_gen.py`, `lean.h` | The lean generator prototype (about 790 lines) and its runtime header (about 45 lines; `LEAN_MASK25` is the recommended memory function, `LEAN_MASK` the 29-bit form, no define is the unmasked lower bound). Reads the relocated image `/work/infected/work/image.bin`. |
| `slice_unit.py`, `pick_funcs.py`, `slices/` | Slices the real generator output (`gen/generated_unit_*.cpp`) for six chosen functions and compiles them in "current style". |
| `lean_six/`, `lean_six_emit.log`, `lean_six_measure.log` | Same six functions in lean form: unmasked 358 insns / 1,152 B, masked 412 / 1,356, 25-bit masked 411 / 1,340. |
| `lean_all/`, `lean_all_emit.log`, `lean_all_measure.log` | Whole-program lean emission, 18 translation units, 9,229 functions, 451,251 reachable words, masked and unmasked builds. |
| `vita/compile_unit.sh`, `secsum.py` | Baseline: the real unit 0010 compiled at `-O2`, `-O2` with the two fast-path defines, and `-Os`. |
| `gen/`, `gen_run.log` | Output of PortableKit's own `psp_recomp` for BOOT.BIN (162 units). |
| `diff/difftest.cpp`, `run_mask_postfix.log` | Differential test of lean code against `src/interpreter.cpp` on x86-64. |
| `dyn_profile.py`, `dyn_unsup.py`, `dyn_unsup_out.log` | Dynamic coverage and instruction mix from the bring-up agent's PC histograms. |
| `insncount.py`, `lean_measure.py` | Instruction and symbol-size counting on Vita objects. |
| `syntax/` (46 logs), `hazard/h1.cpp` | `-fsyntax-only` over the runtime-relevant translation units; link test of the libstdc++ facilities. |
| `at3probe/`, `iso_listing.txt` | ISO inventory (75 `.at3` files, all ATRAC3plus), format probe. |
| `funcs/*.dis`, `/work/assess/tools/mipsstat.py` | Disassembly of the chosen functions; static opcode statistics. |

The experiment directory contains disassembly and generated code derived from BOOT.BIN, which is private game data; it should not leave the machine. The assessment itself quotes only short snippets.

### Reproduction (one core)

```
export PATH=/work/vitasdk-root/vitasdk/bin:$PATH
cd /work/assess/experiments
bash vita/compile_unit.sh 0010 base                                   # baseline unit, one core
bash vita/compile_unit.sh 0010 fast -DPSPRECOMP_AOT_PRODUCTION_FASTPATHS -DPSPRECOMP_AOT_ASSUME_NO_WRITE_WATCH
python3 lean_gen.py stats                                             # analysis statistics
python3 lean_gen.py emit lean_all                                     # whole-program emission, 18 TUs
python3 lean_measure.py lean_all nomask                               # unmasked lower bound
python3 lean_measure.py lean_all mask -DLEAN_MASK                     # 29-bit masked
python3 lean_measure.py lean_six mask25 -DLEAN_MASK25                 # 25-bit masked (recommended), six functions
python3 insncount.py lean_six/lean_00_mask25.o '_Z\d+f_'              # instruction count
```

The whole-program builds take about 8.5 minutes each on one core. `lean_gen.py` options (`--funcs`, `--sample`, `--maxfn`, `--nelide`) are documented at the head of the file. `experiments/gen/` was produced by `psp_recomp` (`gen_run.log`).

### What was not verified (all [U])

1. **Anything on a Vita**: IPC and timing of the lean code, `std::filesystem`/`chrono` behaviour on `ux0:`, `sceLibcHeapSize`, memory block types and cache coherency for guest RAM and GPU memory, availability of the hardware H.264 decoder to homebrew, `libshacccg.suprx` or an offline GXP compiler, GXM behaviour with alpha-test `discard` and 4x MSAA, unaligned access behaviour (`SCTLR.A`).
2. **VFPU on NEON, jump tables, spill-all functions, `lwl/lwr` and `ext/ins`**: designed, not yet emitted. They account for the 31% of dynamic instructions the prototype does not cover (D.5).
3. **ARM floating-point edge semantics** (NaN, flush-to-zero, `vcvt` rounding) of the lean helpers; the differential test ran on x86-64.
4. **The address-class histogram** of guest loads and stores, whether the 18 `lui 0x0400` sites are "is VRAM" tests, whether CPU code reads VRAM, and what the roughly 140 `lui rX, 1` sites do (C.2, C.3). These are step 0 of I.4 and need the existing interpreter on the desktop.
5. **SAS voice counts** in missions (F.1), **ATRAC3plus decoder bit-exactness** (F.2), the recorded `DrawCall` stream (E.5).
6. **Why `intro.pmf` hangs with faster libc loops** under the interpreter (bring-up log): a sensitivity of the cooperative model that the coroutine kernel must be tested against.
