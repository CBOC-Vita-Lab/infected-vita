# vitaGL notes for the Infected PS Vita port

Everything here was read from the vitaGL source (commit `dca4b9d143290d78ec043131a19be36c78cdc7b5` for sections 1-13; the port has been on
`9bf3e3f4e55efd1f4a191b179144f7f9a98cd925` since 30 Oct 2026, see section 14 for what changed), from the vitasdk headers/libs in `$VITASDK`, and from the Vita3K source. Nothing could be run
(no Vita, no ARM emulator on the build machine), so every statement about **runtime behaviour on hardware** is marked
**UNVERIFIED** and is the subject of a selftest line (`T<n>`) that will confirm or refute it. File and line references are
to `third_party/vitaGL/source/` (git-ignored, re-created by `scripts/build-vitagl.sh`).

Contents: 1 builds and flags - 2 init - 3 memory (`_newlib_heap_size_user`, pools, SFO) - 4 threading - 5 swap and pacing -
6 GLSL translator - 7 shader cache - 8 textures - 9 FBOs - 10 state support - 11 draw path - 12 SceVideodec - 13 open questions.

## 0. What the port must know in one screen

* Link `libvitaGL.a` (release). Use `libvitaGL_debug.a` for bring-up. `libvitaGL_store.a` only if depth must survive an FBO round
  trip (see 10). `libvitaGL_fast.a` is for measurement only until T8 shows it is worth the hazards.
* **One thread owns GL.** Call `vglInit*`, every `gl*` and `vglSwapBuffers` from the same thread (core 1 in the architecture).
  Only the debug build enforces it; the release build corrupts state silently.
* Runtime GLSL needs `ur0:data/libshacccg.suprx` **and** taiHEN (the SceShaccCgExt patch uses `taiInjectData`; taiHEN is part of
  HENkaku/ensō, kubridge is not involved). Without the module, only shaders already in the shader cache work.
* Cheapest draw: static or ring **VBO**, attribute pointers set once, `glDrawArrays(first = k * verts)`; **no** client arrays,
  **no** `glBufferSubData` after the buffer has been drawn this frame, no redundant `glUseProgram`/blend changes. Reasons in 11.
* `glGetUniformLocation` returns a *pointer-derived* number; only `-1` means "missing". Query arrays as `"u_pal"`, never `"u_pal[0]"`.
* No `glBlendColor`, no `GL_CONSTANT_*` blend factors: PSP "fix" blending needs two passes or a shader. `glClear` ignores
  `glColorMask`. Details in 10.
* GLSL variables must not be called `s t st ts ss tt sampler matrix mod mix atan fract not lessThan`... (6).

## 1. Builds and flags

`scripts/build-vitagl.sh <variant>` copies the source tree to `build/vitagl-<variant>/`, runs `make` with
`CC="arm-vita-eabi-gcc -std=gnu17 -I$VITA_SYSROOT/include"` (GCC 15 defaults to gnu23 which breaks the K&R-style `()` prototypes in
vitaGL) and installs the library into the private sysroot (`sysroot/lib/`, never the shared SDK). Each library has a `.flags` file
that lists exactly what it was built with.

COMMON = `NO_SPLASHSCREEN=1 HAVE_SHADER_CACHE=1 HAVE_VERTEX_LAYOUT_CACHE=1 NO_TEX_COMBINER=1`

| Variant | Library (bytes) | Flags on top of COMMON | Use |
| --- | --- | --- | --- |
| release | `libvitaGL.a` (6,418,202) | `NO_DEBUG=1 INDICES_SPEEDHACK=1 SAMPLERS_SPEEDHACK=1` | shipping default. `NO_DEBUG` drops most GL error checks (`SKIP_ERROR_HANDLING`): invalid calls are no longer reported by `glGetError` |
| debug | `libvitaGL_debug.a` (6,831,756) | `LOG_ERRORS=1 HAVE_SHARK_LOG=1 HAVE_PROFILING=1 DEBUG_THREAD_SAFENESS=1 DEBUG_GLSL_TRANSLATOR=1`, compiled with `-DsceClibPrintf=vgl_hook_printf` | bring-up. Error checks stay on, shader compile logs (`glGetShaderInfoLog`) exist, every translated shader is dumped, per-30-frame profiling (draw time, GPU stall, layout-cache hits), wrong-thread GL calls are logged and ignored. All vitaGL messages end up in `log.txt` via `vgl_hook_printf` (platform layer, any message length) |
| fast | `libvitaGL_fast.a` (6,348,044) | `NO_DEBUG=1 DRAW_SPEEDHACK=1 SAMPLERS_SPEEDHACK=1 BUFFERS_SPEEDHACK=1 USE_SCRATCH_MEMORY=1` | **hazardous**, see below |
| store | `libvitaGL_store.a` (6,418,036) | `NO_DEBUG=1 INDICES_SPEEDHACK=1 SAMPLERS_SPEEDHACK=1 STORE_DEPTH_STENCIL=1` | depth/stencil are loaded and stored at every scene boundary, so they survive FBO switches and read-backs (costs one depth+stencil load/store per scene) |
| profile | `libvitaGL_profile.a` (6,555,522) | `NO_DEBUG=1 INDICES_SPEEDHACK=1 SAMPLERS_SPEEDHACK=1 HAVE_PROFILING=1 LOG_ERRORS=1`, compiled with `-DsceClibPrintf=vgl_hook_printf` | the release library plus vitaGL's own per-30-frame profile (time in draw calls, GPU stalls, vertex-layout cache hits and misses) in `log.txt`; for measuring on the console, not for shipping |

Why these COMMON flags: `NO_SPLASHSCREEN` removes the boot splash thread and the swap-ignoring period; `HAVE_SHADER_CACHE` is
the shader cache of 7; `HAVE_VERTEX_LAYOUT_CACHE` caches the per-draw vertex program lookup (11, 4 entries, `custom_shaders.c:398`);
`NO_TEX_COMBINER` removes `GL_COMBINE` code that only the fixed-function path uses (the port is shader-only).

**fast** details (verified in source):
* `DRAW_SPEEDHACK`: no packing analysis, no client-array copy: client arrays must already be GPU-mapped memory (a heap pointer
  makes the GPU fault) and the vertex-layout cache is compiled out. T8 skips the client-array variants in this build.
* `BUFFERS_SPEEDHACK`: `glBufferSubData` writes in place. Without it, updating a buffer that was drawn in the last 4 frames
  allocates a *whole new buffer*, copies the old content plus the patch and defers freeing the old one (`buffers.c:480`). With it the
  CPU can overwrite data the GPU is still reading (display buffers = 3 frames in flight): a ring must be at least that deep.
* `USE_SCRATCH_MEMORY`: `GL_STREAM_DRAW`/`GL_DYNAMIC_DRAW` buffers live in the per-frame circular pool and are overwritten when that
  pool slot is reused, i.e. after `display_buffers` frames. A persistent dynamic ring must not use scratch:
  `vglSetupScratchMemory(dynamic, stream)`; `plat::GlParams::scratch_for_dynamic` (default **false**) / `scratch_for_stream` (default
  true) set it in `gl_init` (vitaGL's own default is true/true). It is a no-op in the other libraries.
* `SAMPLERS_SPEEDHACK`: no null checks on sampler units.

**Speed options.** Each option was read in the patched source (`third_party/vitaGL/source`, commit `dca4b9d` plus our patches 0003 and 0004; unchanged by the move to `9bf3e3f`, section 14)
and checked against the port's own GL calls: only 16-bit `glDrawElements` from a scratch buffer, `glDrawArrays` for blits, an overlay and a
platform composite from client arrays, one `sampler2D` (unit 0) or none per program, `GL_TRIANGLES` and `GL_LINES`, no `glBufferSubData`,
no `glOrtho`/`glFrustum`, a `glReadPixels` only for screenshots. **None of this has run on the console**; the two enabled options are
compiled into `libvitaGL.a`, `libvitaGL_store.a` and `libvitaGL_profile.a` and were checked by reading, not by a run.

| Option | Decision | Why |
| --- | --- | --- |
| `INDICES_SPEEDHACK` | **on** (release, store, profile) | Drops the per-draw loop that writes each stream's `indexSource` and the 32-bit-to-16-bit downgrade. Every VAO is reset with `INDEX_16BIT` (`buffers.c:80`, `reset_vao` at init), the port draws 16-bit indices only and never instanced. |
| `SAMPLERS_SPEEDHACK` | **on** (release, store, profile) | Drops the `if (p->frag_texunits[i])` test per texture unit. Every program has one sampler at resource index 0 (`u_texture`) or none, and none has a vertex texture, so no slot below `max_frag_texunit_idx` is empty. (The fixed-function overlay does not go through this code.) |
| `INDICES_DRAW_SPEEDHACK` | off | Only changes the copy of client-array indices into mapped memory; the scratch path is already copyless, so there is nothing to gain, and the renderer's heap fallback (scratch memory exhausted in a frame) would hand a heap pointer to the GPU. |
| `PRIMITIVES_SPEEDHACK` | off | Leaves out restoring the polygon mode after a line or point draw. The renderer does emit `GL_LINES` (PSP line primitives), and the next triangle draw would then be drawn as lines. Gain: one compare per draw. |
| `MATH_SPEEDHACK` | off | Only changes `glOrtho`/`glFrustum` (they assume an identity matrix); the port never calls them. No effect. |
| `NO_TILE_CLIPPER` | off | Moves scissor clipping from the CPU to the GPU. The port scissors about 6 times a frame, so the CPU gain is negligible, and the GPU is the limit at the heavy settings. |
| `SHARED_RENDERTARGETS` | off | Shares one render target between up to 8 small FBOs and creates every FBO wider than 256 px with `scenesPerFrame` 1 whatever `vglSetupRenderTargetScenesNum` says, which defeats the scenes-per-frame setting (`gl_rt_scenes`, below). It saves memory only. |
| `CIRCULAR_POOL_SPEEDHACK` | off | One undivided pool instead of one per display buffer; the scratch memory the batches live in comes from this pool, and a wrap while the GPU still reads would corrupt a frame silently. The saving is a few instructions per frame. |
| `DRAW_SPEEDHACK` | off | Passes client arrays straight to the GPU (they must be mapped memory) and compiles out the vertex-layout cache. The platform composite and the overlay draw from static or heap arrays. Stays in the `fast` measurement variant only. |
| `BUFFERS_SPEEDHACK` | off | Only changes `glBufferSubData`, which the port never calls; in place updates would let the CPU overwrite data the GPU is reading. |
| `TEXTURE_UPLOADS_SPEEDHACK` | off | Gives `sceGxmTransferCopy` the caller's pointer instead of a copy in mapped memory; the HD loader's buffers are ordinary heap memory. Uploads are about 0 ms a frame in play. |
| `READBACKS_SPEEDHACK` | off | Skips the scene end before `glReadPixels`; the port reads back only for screenshots and the optional display store. No gain. |

**Scenes per frame (not a build flag):** `vglSetupRenderTargetScenesNum(display, fbo)` (vitaGL, August 2026, in our base) sets `scenesPerFrame` for the display target and for each FBO's
target; the default is 1/1. In a traced mission frame (PC replay) the guest's four render targets are begun in the order A, B, C, D, C, B, A, so three of the four are begun twice
in one frame. The port now asks for 4/4 (`gl_rt_scenes`, `gl_rt_scenes_fbo`; 0 = vitaGL's default). What going over the declared number costs, and what the larger value costs in memory, are both unknown.
Read it on the console with `gl_timing=60`: the `[gl-time]` line's "first draw after a switch" against a run with `gl_rt_scenes=0`.

**Other switches left off, on purpose**: `HAVE_UNFLIPPED_FBOS` (the default keeps FBO textures in GL orientation, so samplers need no V flip),
`HAVE_GLSL_TEXTURE_SIZE`, `HAVE_GLSL_UBOS` (experimental), `HAVE_FFP_SHADER_SUPPORT`, `HAVE_CUSTOM_HEAP`, `HAVE_TEXTURE_CACHE`,
`SAFE_UNIFORMS` (see 6, array uniform names), `SAFE_DRAW`, `PHYCONT_ON_DEMAND`, `HAVE_PTHREAD` (the GC thread uses
`sceKernelCreateThread`), `DISABLE_W_CLAMPING`. Rebuild with the corresponding `make` variable if a test points at one.

Companion libraries in the sysroot: `libvitashark.a` (vitaShaRK `df24065`), `libSceShaccCgExt.a` (SceShaccCgExt `1021923`),
`libtaihen_stub.a` (taiHEN prebuilt vdpm package), `libmathneon.a` (math-neon `0faab81`). Links against: SceGxm, SceGxmInternalForVsh (via
vitaGL, as in every vitaGL app), SceShaccCg, SceDisplay, SceCtrl, SceAudio, ScePower, SceCommonDialog, SceSysmodule, SceAppMgr,
SceSharedFb, SceNet, SceDmacmgr, SceRtc... **No kubridge import appears in the ELF** (`readelf` on the selftest: the `.vitalink.fstubs.*`
list is all system modules plus `taihen`).

## 2. Init call and parameters

`plat::gl_init(GlParams)` (`platform/vita/gl_vita.cpp`) performs, in this order (all "before init" calls are required to be before
`vglInit*`):

```
vglSetShaderCachePath("ux0:data/infected/<dir>")        // optional, see 7
vglSetVDMBufferSize / VertexBufferSize / FragmentBufferSize / USSEBufferSize / ParamBufferSize   // only when GlParams asks (defaults: sceGxm 128 KiB / 2 MiB / 512 KiB / 16 KiB / 16 MiB)
vglUseCachedMem(GL_FALSE)                                // uncached RAM pool; cached needs explicit cache maintenance
vglSetupScratchMemory(false, true)                       // no-op except libvitaGL_fast.a
vglSetCircularPoolSize(16 MiB)                           // per-frame transient data, divided by display_buffers; vitaGL default 32 MiB
vglSetDisplayBufferCount(3)
vglSetupGarbageCollector(0x10000108, SCE_KERNEL_CPU_MASK_USER_2)    // default: priority 0x10000100, affinity 0 (any core)
vglSetupRuntimeShaderCompiler(SHARK_OPT_DEFAULT, 0, 0, 0)           // O2, no fastmath/fastprecision/fastint
vglSetDisplayCallback(cb)                                // only if the selftest overlay is installed
vglInitWithCustomThreshold(legacy_pool=0, 960, 544, ram_thr, cdram_thr, phycont_thr, cdlg_thr, SCE_GXM_MULTISAMPLE_NONE)
vglWaitVblankStart(GL_TRUE)
```

* Resolution **960x544**, MSAA **off** by default (`GlParams::msaa` 0/2/4, selftest key `gl.msaa`). 4x MSAA multiplies colour and
  depth memory by 4 (`gxm.c:489`) and the tile bandwidth; the PSP content is texture-bound, so start without it. UNVERIFIED: cost
  of 2x on this content; T6/T8 take `gl.msaa=2` for a comparison run.
* Return value of `vglInit*` = "display size had to fall back to the maximum" (`res_fallback`), **not** an error flag. There is no
  failure return and no `vglEnd()`; `plat::gl_shutdown` only marks GL as down.
* The `...Threshold` init computes `pool = free_at_init - threshold`, `0` if `threshold >= free`. `gl_init` converts "pool size I
  want" into thresholds: `GlParams` values and defaults are

| Pool (vitaGL) | Memory type | Default pool | Why |
| --- | --- | --- | --- |
| RAM (`VGL_MEM_RAM`) | `USER_RW_UNCACHE` | **24 MiB** (`ram_pool_mib`) | GPU-visible overflow: vertex/stream data, textures that do not fit CDRAM. Capped to 200 MiB (`0xC800000`) when uncached (`mem_utils.c:420`) |
| CDRAM (`VGL_MEM_VRAM`) | `USER_CDRAM_RW` | **all free** (`cdram_pool_mib = -1`, `cdram_keep_mib = 0`) | textures, render targets, depth, colour buffers; only the GPU can use CDRAM anyway |
| PHYCONT | `USER_MAIN_PHYCONT_NC_RW` | **0** | left free for SceVideodec/SceAvcdec (12), which needs physically contiguous buffers |
| CDLG | `USER_MAIN_CDIALOG_NC_RW` | **0** (`cdlg_thr = 0x8C6000`) | the port shows no system dialogs; `vglSwapBuffers(GL_FALSE)` is used |
| circular pool | carved from the above | 16 MiB | T8 logs the peak use; raise if `LOG_ERRORS` reports an overrun |
| legacy (immediate mode) | - | 0 | the port never calls `glBegin` |

* **vitaGL's own CPU-side bookkeeping (`vglMalloc`) comes from the *newlib heap first*** (`vgl.c:603`: external, then RAM, PHYCONT,
  CDLG, VRAM). Programs, uniform tables, texture slot objects and shader copies count against `_newlib_heap_size_user` (3). Allocation
  order for GPU data: "for CPU" (stream/dynamic buffers, transient data) RAM -> PHYCONT -> CDLG -> VRAM -> newlib; "for GPU" (static
  buffers, textures, surfaces) VRAM -> RAM -> PHYCONT -> CDLG -> newlib (`gpu_utils.c:92`). `vglUseExtraMem(GL_TRUE)` (default) lets
  GPU allocations fall back to newlib memory once the pools are full.
* Static GL limits worth knowing: textures 16384 objects, 4096x4096 max, 16 texture units, 16 vertex attributes, 256 framebuffer objects,
  `MAX_CUSTOM_SHADERS` 2048, `MAX_CUSTOM_PROGRAMS` 1024, `glDrawArrays` at most `0xC000` (49152) vertices per call, 128 occlusion queries.
* First run: `vglInit` calls `sceIoMkdir` for the shader cache tree (up to 2 x 256 directories, see 7) and for the hard-coded
  `ux0:data/shader_cache/v31/{v,f}` fixed-function cache directories even though the port never uses the fixed-function path. The
  tree creation cost is part of the "vitaGL up in N ms" log line of the selftest (`T6 gl-init`).

## 3. Heap sizing: `_newlib_heap_size_user`, `sceLibcHeapSize`, `param.sfo`

* vitasdk newlib allocates its whole heap at start-up in `_init_vita_heap` with `sceKernelAllocMemBlock(..., 0x0C20D060 =
  SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, size)`. `size` is the weak symbol `_newlib_heap_size_user`, **default 128 MiB** (verified by
  disassembling `libc.a`: `mov r2, #0x8000000`; also stated in the vitasdk sample `newlib_heapsize_ctrl`). If the block cannot be
  allocated the heap is empty and `malloc` returns NULL.
* The selftest defines `unsigned int _newlib_heap_size_user = 144 * 1024 * 1024;` (`selftest/st_harness.cpp`) because the port needs
  about 130 MiB of its own heap (66 MiB guest arena + ~64 MiB of everything else) **plus** vitaGL's bookkeeping. T2 allocates the 66 MiB
  arena and a 96 MiB block, touches every page and logs `sceKernelGetFreeMemorySize` (user/cdram/phycont) before and after.
* `sceLibcHeapSize`: this is a Sony-libc symbol. The vitasdk newlib in this SDK does not reference it (no occurrence in `libc.a` or
  `crt0.o`), so it is not defined by the port. UNVERIFIED that nothing else on the system reads it.
* `sceUserMainThread{Name,Priority,StackSize,CpuAffinityMask}` are exported by the selftest as `extern const` (C++ `const` has
  internal linkage; the symbols were absent from the first ELF - checked with `nm`): priority `0x10000100`, 1 MiB stack, core 0.
* **Memory budget** (retail memory mode, `ATTRIBUTE2 = 0`): the OS shows a *user* budget of 256 MiB in Vita3K's emulation
  (`SceSysmem.cpp:174`: 256 MiB user, 112 MiB CDRAM, 26 MiB phycont); real value UNVERIFIED - T2 prints it. Plan: newlib 144 MiB + vitaGL
  RAM pool 24 MiB + ~7 MiB BSS (FFmpeg `ff_tx_tab_*` float tables are 4 MiB of it) + stacks = about 180 MiB.
* **`param.sfo` `ATTRIBUTE2 = 12`** (`vita-mksfoex -d ATTRIBUTE2=12`, set by `-DVITA_ATTRIBUTE2=12`, `vita/build.sh --attr2 N`) requests the
  extended memory mode: bits `0x0C` select +29 MiB (4), +77 MiB (8) or +109 MiB (12) of user memory (the numbers Vita3K implements).
  **UNVERIFIED on hardware**: whether firmware 3.74 honours it for a HENkaku-installed app, whether it changes `size_user`, and
  whether it costs anything (e.g. a smaller CDRAM). The package also exists with `--attr2 0` for the comparison/fallback run (**note**: `vita-mksfoex`'s own default is `ATTRIBUTE2 = 12`, verified by running it and parsing the SFO, so the build always writes the value explicitly; leaving the flag out does NOT give retail memory). The SFO
  also carries `ATTRIBUTE=0x8000` (vita-mksfoex default for `gd`), `PARENTAL_LEVEL=1`, `CATEGORY=gd`.

## 4. Threading rules

1. **Single GL thread.** `THREAD_SAFE()` (`shared.h:88`, debug build only) compares a per-thread flag stored at `TLS-1984` that
   `vglInitWithCustomSizes` sets on the calling thread; other threads get a log line and the call is dropped. In the release build
   there is no check and sceGxm's context is not thread-safe: two threads in GL = corrupted command lists.
   (UNVERIFIED: that `TLS-1984` is unused by anything else for threads that never called init.)
   The selftest runs init and every GL test on one worker thread pinned to core 1 (`gl.thread=1`, default); `gl.thread=0` runs GL on the
   main thread for comparison.
2. Calls that are safe from other threads: none of the `gl*`/`vgl*` ones. `vglMalloc`/`vglFree` are used by the GC thread internally.
   Resources shared with other cores must go through the guest/GL queue defined in the architecture.
3. vitaGL's own **GC thread** ("vitaGL Garbage Collector") is created at init: stack 64 KiB, priority `0x10000100 + 8`, **core 2** with
   our settings. `vglSwapBuffers` takes the semaphore `gc_mutex[1]` (initial count 4 = `FRAME_PURGE_FREQ`) and signals `gc_mutex[0]`, so it
   only blocks if the GC thread is **4 swaps behind**. Starving core 2 (audio, decoders) at a higher priority than the GC thread will
   eventually stall swaps. T4/T5 log the thread priorities; T6/T8 swap times show the stall.
4. The display queue callback (`display_queue_callback`, `gxm.c:246`) runs on an sceGxm internal thread: it calls the optional
   `vglSetDisplayCallback` hook, `sceDisplaySetFrameBuf` and `sceDisplayWaitVblankStartMulti`. Keep an overlay hook cheap.
5. Never call GL from a signal-like context, never from the audio thread. `plat::gl_swap()` is only called by the GL thread.
6. **The C++ runtime must be linked as multi-threaded.** libgcc and libstdc++ (`gthr-posix.h`) decide whether a program has threads by
   a *weak* reference to `pthread_cancel`, and call `pthread_once`, `pthread_cond_wait` and friends through weak references too. A
   static link pulls nothing in for a weak reference, so before the October build every one of them was 0: `__gthread_active_p()`
   said "no threads", `std::mutex` locked nothing, `shared_ptr` counts were not atomic, `thread_local` (emulated TLS on this
   toolchain, `--disable-tls`) was ONE variable shared by every thread, and a call to an undefined weak function linked as a call
   to nothing (the first guarded static then dereferenced a mutex `pthread_once` never made). The split renderer's
   "am I the GL thread" `thread_local` was therefore true on the walker, which drew everything itself: Split was one thread on
   another core. `app/CMakeLists.txt` forces the pthread functions in with `-u` and a post-link step fails the build if any
   `pthread_*` symbol is still weak and undefined (`nm infected | grep ' w pthread_'`). The startup line
   `costs: clock read ... | event signal ... | std::mutex ... | thread_local ...` measures what these primitives cost on the console.

## 5. Swap and vblank pacing

* `vglSwapBuffers(GL_FALSE)` = `vgl_framecount++`, resets the next circular-pool slot, `sceGxmEndScene`, then
  `sceGxmDisplayQueueAddEntry(front_sync, back_sync, cb)`, then the GC hand-off. It returns once the frame is **queued**, not
  displayed. The CPU waits for a free display buffer inside the next scene begin / `AddEntry`, i.e. when the GPU is 3 frames behind
  (`gl.buffers=3`; `gl.buffers=2` = double buffering, lower latency, no CPU run-ahead).
* The vblank wait is done on the **display queue thread**: `sceDisplayWaitVblankStartMulti(vsync_interval)` after
  `sceDisplaySetFrameBuf(..., NEXTFRAME)`. `vglWaitVblankStart(true)` sets interval 1 (60 Hz, the PSP ran at 59.94 Hz), `false` = 0 (no
  wait, may tear). Interval 2 (a hard 30 fps lock) is available through `eglSwapInterval(display, 2)` (`egl.c:283`, sets the same variable);
  not wrapped by the platform layer yet - add `gl_set_swap_interval(n)` if the port wants it.
* Because pacing happens off the GL thread, "swap time" in T8 with `t8.vsync=0` measures queue back-pressure (GPU-bound frames), and
  with `t8.vsync=1` it shows the vblank beat. The harness reports submit time and swap time separately.
* No frame skipping is built in: if the GL thread is slower than 16.7 ms the display repeats frames (interval 1) and the time
  quantises to 16.7 / 33.3 ms.

## 6. GLSL ES 1.00 through the runtime translator

`glCompileShader`/`glLinkProgram` convert GLSL text into Sony Cg text (`utils/glsl_utils.c:1293`), then compile it with libshacccg
(vitaShaRK). **The port should treat the translator as a text-mangling preprocessor with a short list of known gaps**.

Pipeline: `#version` line and `#extension` lines are commented out (first `#version` only) -> a C-style preprocessor runs
(`#define/#if/#ifdef`; `GL_ES` and `VITAGL` are predefined) -> `precision ...;` statements are blanked -> `gl_FragData[0]` becomes
`gl_FragColor` -> a ~100-line header of `#define`s and helper functions is prepended -> `*` operators are rewritten into `vglMul()`
calls -> globals get `static`.

**Default semantic mode is `VGL_MODE_POSTPONED`**: `glCompileShader` does nothing for GLSL sources (and `GL_COMPILE_STATUS` always
reports `GL_TRUE`); translation and compilation happen in **`glLinkProgram`** where both shaders of the pair are known. So
**check `GL_LINK_STATUS`**, and `glGetProgramiv(GL_INFO_LOG_LENGTH)` is always 0. Compile messages exist only in the debug library
(`glGetShaderInfoLog` + `log.txt`). A varying that the fragment shader reads but the vertex shader never writes is an error.

**Unsupported or hazardous constructs (verified in source; the T6 probes `glsl_probes()` compile each one and report OK/FAIL):**

| Construct | Behaviour |
| --- | --- |
| Identifiers `s t st ts ss tt sampler matrix Matrix mod mix atan fract not lessThan lessThanEqual inversesqrt dFdx dFdy texture2D samplerCube vgl centroid lowp mediump highp vec2..4 ivec2..4 bvec2..4 mat2..4 matrixCompMult` | they are `#define`d by the header: using one as a variable, struct member or function name breaks the shader (a variable called `t` or `s` is the classic casualty). `matrix`, `Matrix`, `sampler`, `texture` are renamed consistently (also in `glGetUniformLocation`), the rest are not |
| `*=` | **not** rewritten (`glsl_utils.c:1139` "FIXME"). `mat4 *= mat4` or `vec *= mat` fails/miscomputes; scalar and component-wise `vecN *= vecN` are fine because Cg's own `*=` matches |
| `*` between matrices / matrix and vector | rewritten by a *textual* scan for the operand extents (parentheses, `[]`, `, = < > ! ? : + -` as delimiters). Works for simple operands, can be wrong for complex ones such as `f(x) * M * v`. Safest: `#define MUL(a,b) vglMul(a,b)` under `#ifdef VITAGL`, plain `((a)*(b))` elsewhere. The rewriter skips explicit `vglMul(...)` calls |
| Varyings | mapped to `TEXCOORD0..9` and `COLOR0..1` (`MAX_CG_TEXCOORD_ID 10`, `MAX_CG_COLOR_ID 2`); T6 `shader_varying_limit()` probes the real maximum. `varying` in the fragment shader must match the vertex shader by name |
| `gl_FragData[n]` | only `[0]` (rewritten); no MRT |
| `gl_FragCoord`, `gl_FrontFacing`, `gl_PointCoord`, `gl_PointSize`, `gl_FragDepth` | supported by name (`WPOS`, `FACE`, `SPRITECOORD`, `PSIZE`, `DEPTH`) |
| `discard` | native in Cg; T6 `discard` checks an alpha-test shader |
| `dFdx/dFdy` | map to `ddx/ddy`; the `#extension` line is dropped silently |
| `texture2DProj`, `texture2DLod`, `texture2DProjLod`, `textureCube[Lod]`, bias argument | provided; `textureSize` is not (needs `HAVE_GLSL_TEXTURE_SIZE`, experimental) |
| integer/bool vector comparisons `lessThan/equal/...` | provided; they return `int` vectors, not `bvec` - mixing them with `any()/all()` on bool vectors is UNVERIFIED |
| Uniform arrays | `glGetUniformLocation("u_pal")` works; `"u_pal[0]"` returns -1 unless the library is built with `SAFE_UNIFORMS=1` (without it the name is passed to `sceGxmProgramFindParameterByName` verbatim). Upload the whole array with `glUniform4fv(loc, count, ...)` |
| Uniform location values | `-(GLint)(uniform*)`: a pointer, typically a large positive number for newlib addresses (>= 0x80000000), but **not guaranteed non-negative**. Test only `== -1` (the selftest logs the raw values, T6 `uniform-locations`) |
| Uniform buffers (`HAVE_GLSL_UBOS`), `textureSize`, `GL_FIXED` attributes (`HAVE_FIXED_ATTRIBUTES`) | off in all four builds |
| Constant arrays / structs / loops with non-constant bounds / `out`/`inout` parameters | passed through to the Cg compiler; T6 probes `for loop, constant bound`, `uniform array dynamic index`, `function with out param`, `const array / struct` |
| `float` precision | `vglUseLowPrecision(GL_TRUE)` would turn float into half (`glsl_precision_hdr`); off |

`glReleaseShaderCompiler()` unloads libshacccg (`shark_end`: `sceShaccCgReleaseCompiler`, `sceShaccCgExtDisableExtensions`, `sceKernelStopUnloadModule`) and frees its memory; the next
compile reloads it. Call it after the warm-up if memory is tight (UNVERIFIED: how much libshacccg holds. vitaGL loads it inside `vglInit` (`init_gxm` -> `start_shader_compiler`), so the `GL: init ... free before` and `vitaGL up ... system free after` lines of `gl_init` bracket the load, and T6 `shader-compiler-memory` brackets the first compiles).

Compile cost: a runtime compile is a **hitch of tens to hundreds of ms** per shader pair (UNVERIFIED, T6 `shaders-cold` measures compile
and link in ms; `shaders-warm` re-links the same sources from the cache). The port must never compile on the GL thread during
gameplay: ship the cache (7) and warm every program during loading.

## 7. Shader cache

* Enabled by `HAVE_SHADER_CACHE=1` (all four libraries). `vglSetShaderCachePath(root)` (before init; default
  `ux0:data/shader_cache/<TITLE_ID>` where the title id comes from `sceAppMgrAppParamGetString`) sets the root; vitaGL then uses
  `<root>/v3/{v,f}/<XX>/<H>.gxp` with `v3 = SHADER_CACHE_MAGIC` (`shared.h`), `H = XXH3_64bits(raw source text)` printed with `%016llX`
  (**zero padded to 16 digits**) and `XX = H >> 56` printed `%02X`. (At `dca4b9d`, the 1.0 build, it was `v2` and `%llX` without padding;
  the move to `9bf3e3f` changed both. A 1.0 cache is converted with `scripts/shader-cache-to-v3.sh`, section 14.)
* **The key is the raw GLSL string passed to `glShaderSource` (byte-exact, including any preamble the port prepends) and nothing else.**
  Changing a comment changes the key; changing the compiler options or vitaGL's translator does not (bump `v2` by rebuilding with a new vitaGL
  if its format changes).
* Content per file: matrix-uniform indices, the semantic binding map (`save_bindings = GL_TRUE` in POSTPONED mode), optional sampler
  info, then the compiled GXP. Reading a cached shader needs **no libshacccg**.
* Hazard (UNVERIFIED, from reading `custom_shaders.c:2381`): the binding map in a cached vertex shader was resolved against the
  fragment shader it was first linked with. If the same vertex source is linked later with a fragment shader with a different set of varyings,
  the cached vertex GXP is reused with the old map. When one source participates in several pairs, keep their varying interface identical or
  give each pair its own source text (a unique comment line is enough, since the key is the raw text). `glsl_translator_set_process` handles the
  mixed case (one shader cached, the other not) by adopting the cached shader's map.
* **Pre-shipping** (the first run on a console is slow, shaders compile at link time):
  1. Run the finished port (or the T6/T8 shader set) once on a real Vita with `libshacccg.suprx` installed and `shader_cache_dir` set.
     Every link writes `<H>.gxp` files. Pull `ux0:data/infected/shader_cache/` by FTP (README).
  2. Put the files under `app0:shader_cache/v3/{v,f}/XX/*.gxp` in the VPK (game-derived shaders are generated by the port itself, not from
     game data).
  3. At start, before `vglInit`, `plat::dir_copy_tree` copies the shipped files into `ux0:data/infected/shader_cache/` skipping existing ones.
     Since `9bf3e3f` nothing else is needed: `vglInit` makes `v3`, `v` and `f` only when it can create `v3` itself, and `compile_shader()`
     makes the `XX` folder of a file it is about to write (at `dca4b9d` `vglInit` made all 512 `XX` folders if it could create `v/00`, and a
     partly shipped tree needed them made first).
  4. Keep the GLSL strings byte-identical between the warm-up run and the shipped build (build the preamble once, in one place).
* The selftest uses its own root `selftest_shader_cache` (`ux0:data/infected/selftest_shader_cache`) and salts every source with the time of
  day for the cold run, then compiles the same text again to show the warm path.
* Vita3K has no libshacccg, so GLSL cannot be built there: selftest T6/T7/T8 report `LIMIT ... libshacccg.suprx is missing` instead of failing.

## 8. Textures

Verified in `textures.c` (`gpu_alloc_texture` family):

* Uncompressed: `GL_RGBA/RGBA8` (`UNSIGNED_BYTE`, `4_4_4_4`, `5_5_5_1`, `1_5_5_5_REV`), `GL_RGB` (`UNSIGNED_BYTE`, `5_6_5`), `GL_BGRA`,
  `GL_LUMINANCE`, `GL_ALPHA`, `GL_LUMINANCE_ALPHA`, `GL_R8`, `GL_RG8`, `GL_RGBA16F` (half float); max 4096x4096; NPOT is accepted (rows aligned to 8
  texels); swizzled upload through `sceGxmTransferCopy` when the size allows (<= 2048 high), CPU swizzle otherwise.
* **Paletted**: `GL_PALETTE4_{RGB8,RGBA8,RGBA4,R5_G6_B5,RGB5_A1}_OES` and `GL_PALETTE8_*_OES` through **`glCompressedTexImage2D`**, standard OES
  layout (palette entries first, then the indices; P4 high nibble first - vitaGL swaps nibbles into GXM order, `gpu_utils.c:507`). The data lands in a P4/P8 GXM texture with a
  16/256-entry **ABGR8888** palette (always 4 bytes per entry, `gpu_alloc_paletted_texture`) bound to the texture object (`vglSetTexPalette`).
  Mip chains use *negative* `level` as in the OES spec (`-level` is passed through). Only `GL_TEXTURE_2D`. A different CLUT for the same
  index data needs either a second texture object, `vglOverloadTexPaletteDataPointer`, or a shader lookup. T6 `paletted-p8`/`paletted-p4` compare the
  result with the same image uploaded as RGBA8. UNVERIFIED: filtering behaviour of paletted textures (palette-then-filter vs filter-then-palette) -
  T6 uses NEAREST for the comparison; check bilinear separately before relying on it.
* Compressed: DXT1/3/5 (`GL_COMPRESSED_RGB[A]_S3TC_*`; also sRGB variants), PVRTC 2/4 bpp v1/v2, RGTC1/2 (BC4/BC5), ETC1 (hardware), ETC2-EAC and ATC
  (CPU-decoded, optionally re-encoded at runtime with `recompress_non_native`). PSP DXT1/3/5 data maps directly to GXM `UBC1/2/3`.
* Planar YUV textures through the `VGL_YUV420P_NV12_BT601`/`VGL_YVU420P_NV21_BT601` pseudo-formats (`gpu_alloc_planar_texture`, no mipmaps) - the
  cheap way to show decoded H.264 frames without a CPU colour conversion (UNVERIFIED, not exercised).
* Cube maps work for level 0 only. 1D textures need `UNPURE_TEXFORMATS`. Mip levels beyond the base follow the usual `glGenerateMipmap` or per-level uploads.
* Texture memory is "for GPU" memory: CDRAM first (`gpu_utils.c:104`). Uploads of <= 2048-high power-of-two images use a temp copy in the circular pool (a
  `vgl_fast_memcpy` + `sceGxmTransferCopy`); `TEXTURE_UPLOADS_SPEEDHACK` would skip the copy (unsafe, not enabled).
* **Compressed mip chains (our patch 0004, `vglReserveCompressedMips`, on in the port):** each level of a `glCompressedTexImage2D` chain
  is swizzled by an asynchronous `sceGxmTransferCopy`, and every level after 0 used to `vgl_realloc` the texture's buffer, which may
  move it while the GPU still writes the level before. The HD texture uploader therefore called `sceGxmTransferFinish()` after every
  level, and that waits for whatever the GPU is doing: 10 to 15 ms per HD texture in play, one hitch per new texture on screen. With
  the patch, level 0 reserves the whole chain and the other levels are written in place; the realloc that can remain waits for the
  transfers itself. Measured on the console: the GPU waits of an HD upload went from 10-15 ms to 0.
* **HD replacements are read off the GL thread** (`HdConfig::async_load`, `hd_async=0` turns it off): a blob read from the memory
  card cost 5 ms for the smallest record and up to 60 ms for a 1024x512 one, on the GL thread. A loader thread reads it while the
  texture is drawn with its original, and the GL thread uploads the replacement at the texture's next draw (3-4 ms for a 512x512
  DXT chain). The pack's index is kept in RAM whole (80 KiB) and the pack file is unbuffered, so a blob is one seek and one read.

## 9. Framebuffer objects

* `glFramebufferTexture2D` accepts **`GL_COLOR_ATTACHMENT0` and `GL_TEXTURE_2D` only**; `level` is ignored (`shared.h:1346`); the FBO takes the size and format of the
  texture. Depth/stencil come from `glFramebufferRenderbuffer` (`GL_DEPTH_COMPONENT16/24/32/32F`, `GL_DEPTH24_STENCIL8`, `GL_DEPTH32F_STENCIL8`,
  `GL_STENCIL_INDEX8`), but the *format is only used to decide about stencil*: at the first scene begin on that FBO vitaGL allocates a **32-bit float depth
  surface (plus an 8-bit stencil surface if the renderbuffer was a stencil format) for every FBO, attached or not** (`gxm.c:641`; 480x272: about 0.5 MiB
  depth + 0.13 MiB stencil), and a dedicated `SceGxmRenderTarget` (`setup_render_target`; `SHARED_RENDERTARGETS` is off). Both are re-created when the attached texture changes size and freed
  through the 4-frame purge lists (`FRAME_PURGE_FREQ`). Creating many short-lived FBOs is therefore expensive; keep them.
* Limits: `BUFFERS_NUM 256` objects, 128 render targets per frame purge list, 4096x4096, one scene per FBO per frame by default (`vglSetupRenderTargetScenesNum(display, fbo)` raises it).
  `glCheckFramebufferStatus` only reports "complete" when a colour texture is attached; it does not validate formats. T7 creates a 480x272 RGBA8 target with both
  a 16-bit depth and a D24S8 renderbuffer, reports the creation time and sweeps count/size limits (`T7 fbo-limits`).
* **Orientation**: the default build flips FBO rendering so that the texture is in GL orientation (`misc.c:334`), the display is not flipped (that is the usual GXM
  top-left origin handled by the negative viewport scale). `HAVE_UNFLIPPED_FBOS=1` would drop the flip. T7 `fbo-sample` checks the orientation of a sampled FBO and reports a `LIMIT` if it
  arrives V-flipped.
* `glReadPixels` from the default framebuffer or an FBO is a GPU transfer only for RGBA8/RGB/RGB565/R8 (`vglReadPixels` with a GPU-mapped destination is the fast path);
  every read-back ends the current scene (`sceGxmEndScene` + finish) and is a pipeline stall. A scene break throws away depth/stencil unless the `store` library is used.
* MSAA FBOs use `SCE_GXM_COLOR_SURFACE_SCALE_MSAA_DOWNSCALE` when the display uses MSAA.

## 10. Blend colour, discard, stencil, colour mask, clear, scissor

| Feature | State in this vitaGL | Port action |
| --- | --- | --- |
| `glBlendColor`, `GL_CONSTANT_COLOR/ALPHA` and their ONE_MINUS forms | **not implemented** (no `glBlendColor` in `blending.c`; the enumerators are not mapped, source has no occurrence) | emulate PSP "fix" blend factors with a shader uniform (`gl_FragColor *= u_fix` / `+ u_fix`) or two passes. T6 `blend-color` tests the emulation and reports a `LIMIT` for the missing API |
| Blend factors/equations | ZERO, ONE, SRC/DST colour and alpha with ONE_MINUS forms, SRC_ALPHA_SATURATE; `glBlendEquation` ADD, SUBTRACT, REVERSE_SUBTRACT, MIN, MAX | straight mapping of the PSP modes (besides FIX) |
| `discard` | works in shaders (native Cg) | alpha test = `discard` in the fragment shader; no fixed-function alpha test with shaders |
| Stencil | the default framebuffer has **8-bit stencil** (`DF32M_S8`, `GL_STENCIL_BITS` = 8); full `glStencilFunc/Op/Mask`; FBO stencil needs `GL_DEPTH24_STENCIL8` | usable; T6 `stencil` writes with REPLACE and tests EQUAL |
| Depth | 32-bit float depth on the default framebuffer (`GL_DEPTH_BITS` = 32), D16 renderbuffers allowed for FBOs | `glDepthRangef`, `glPolygonOffset` supported |
| `glColorMask` | honoured by draws (becomes the GXM blend mask) | note: a `glClear` **ignores it** (clear is a full-screen quad drawn with a pre-built program, `misc.c:643`); T6 `color-mask-clear` reports it - clear with a masked quad instead |
| `glClear` cost | a full-screen draw (honours scissor and the depth/stencil masks), not a tile clear | clear once per scene, at the start; avoid clears that the first full-screen draw makes redundant |
| Depth/stencil across a scene break (`glReadPixels`, FBO switch) | **lost** unless built with `STORE_DEPTH_STENCIL=1` | T6 `depth-across-scene-break` and T7 `depth-across-fbo-switch` decide whether the `store` library is needed |
| Scissor | `GL_SCISSOR_TEST` through a tile clip region (`NO_TILE_CLIPPER` not set) plus mask-update program | fine |
| Alpha-to-coverage, line smoothing, `GL_POINT_SPRITE` | outside the PSP feature set | not used |

## 11. Draw path: client arrays vs VBOs, what is cheapest and why

Measured by **T8**; the reasoning below is from `draw.c` and `custom_shaders.c:1057` (`_glDrawArrays_CustomShadersIMPL`).

Per draw call with a program bound the library does: rebuild the fragment program if blend/state changed (`setup_frag_program`), loop
over the program's sampler units setting `sceGxmSetFragmentTexture` (+ per-sampler overrides), then the vertex part, `upload_uniforms()`
(copies the dirty uniforms into the GXM default uniform buffer from the ring, `sceGxmReserve*DefaultUniformBuffer`), then
`sceGxmDraw` of 16-bit indices. `glDrawArrays` always draws from a constant ascending index buffer (`default_idx_ptr`, up to 49152 vertices; quads and line
strips use pre-built index tables) so there is no index processing for `DrawArrays`.

**Vertex data:**
* **Client arrays** (no VBO bound): every draw does `gpu_alloc_mapped_temp(count * stride)` from the circular pool and `vgl_fast_memcpy`s the vertices into it (per attribute
  stream when the attributes are not interleaved in one buffer); the GPU then reads that copy. Cost = alloc + memcpy of the vertices on the **CPU in the submission path**,
  plus `glVertexAttribPointer` changes that bump the layout-cache version (below). 700 draws x ~28 vertices x 20 bytes is ~390 KiB/frame, ~12 MiB/s of copying at 30 fps;
  affordable in bytes, but the per-call overhead dominates.
* **VBO**, interleaved, bound once: the draw passes `vbo->ptr + first * stride` straight to `sceGxmSetVertexStream`; no copy. `target_vbo->last_frame` is
  stamped (cheap).
* **Vertex-layout cache** (`HAVE_VERTEX_LAYOUT_CACHE`): 4 entries keyed by (program, VAO, program layout version, VAO layout version). A hit skips building the
  `SceGxmVertexAttribute/Stream` arrays and looking up the vertex program (patcher) - the most expensive part of a draw. The VAO version is bumped by
  `glEnable/DisableVertexAttribArray`, `glVertexAttribDivisor`, and by `glVertexAttribPointer` **only if the offset, VBO, format, component count or stride
  actually changed** (`custom_shaders.c:3299`). So: **set the attribute pointers once per buffer + program, then vary `first`** (the pointer offset is computed from
  `first`, not part of the key). Re-pointing the attributes for each draw (a different byte offset into a ring) misses the cache every time; client arrays miss
  every time too (the pointer changes). T8 measures "vbo-ring-first" against "vbo-ring-repoint".
* The cache is 4 entries per (program, VAO) pair. With 2 programs alternating every ~50 draws, entries stay valid. A game with 20 programs and one VAO will thrash it: use one VAO per
  (program, buffer) combination or batch by program.

**Buffer updates:**
* `glBufferData` re-allocates (the old block is queued on the purge list when drawn in the last 4 frames); STREAM/DYNAMIC usage selects "for CPU" memory (RAM pool first),
  STATIC selects "for GPU" memory (CDRAM first - fast for the GPU, **slow to write from the CPU**).
* `glBufferSubData` on a buffer **drawn in the last 4 frames** allocates a new buffer of the *full* size, copies the old data around the patch and queues the old block for
  deletion; the new buffer is marked unused, so later patches before the next draw are plain memcpys. Rule: **write the whole frame's vertex data into the ring in one go
  before the first draw that reads it** (one `glBufferSubData` over the whole region, or build in CPU memory and upload once), never interleave `SubData` and draws on the
  same buffer. Use 3-4 ring buffers rotated per frame (T8 allocates 4) so the buffer being written was last drawn >= 3 frames ago.
* `glBufferData(GL_STREAM_DRAW)` per frame (orphaning, T8 `vbo-stream-orphan`) is the alternative; allocation cost vs. one new full-size buffer per frame is what T8 compares.

**Uniforms:** `glUniform*` only stores into a per-program shadow buffer and sets a **global** dirty flag per stage (`dirty_shader_vert_unifs`/`dirty_shader_frag_unifs`). At the next
draw `upload_uniforms()` reserves a new GXM default uniform buffer (`sceGxmReserve*DefaultUniformBuffer`, taken from the sceGxm ring) and `memcpy`s the **whole** default uniform buffer of
that stage (`unif_buf_size`, i.e. all uniforms of the program, not just the changed one); the flags are set again at every swap. Cost per updated draw = one ring reservation + a
memcpy proportional to the program's total uniform size, so keep per-draw uniforms small and arrays out of programs that change per draw. T8 `uniform-cost` gives the number. Cheaper:
per-draw data in a vertex attribute or in a 1-row texture. Heavy per-draw uniform traffic also eats the sceGxm vertex/fragment ring buffers (`GlParams::vertex_ring_kib/fragment_ring_kib`,
`gl.vertex_kib/gl.fragment_kib` in the selftest).

**State changes:** each `glBlendFunc`/`glBlendEquation`/`glColorMask`/`glEnable(GL_BLEND)` marks the fragment program dirty (`change_blend_mask`, `blend_state`), and the next draw
re-patches/looks up a fragment program (`sceGxmShaderPatcherCreateFragmentProgram` is cached by the library, but the lookup runs per change). T8 `blend-churn-cost` toggles the
blend function every 8 draws. Sort draws by (program, blend state, texture) before submitting. `glUseProgram` with the same program is cheap but not free; skip it.

**Textures:** a bind is a pointer store; the per-draw cost is `sceGxmSetFragmentTexture` for each sampler of the program. Switch textures per draw only when needed.

**Redundant GXM state (our patch 0003, `vglSkipRedundantState`, on in the port: `gl_vgl_skip_redundant=0` turns it off):** every custom-shader draw sets the
vertex program, the fragment program, each sampler's texture and every vertex stream again, whether or not they changed. With the patch on, those
calls are left out when they would set what the previous custom-shader draw set. The record of what is set is dropped whenever anything else
touches that state (`vgl_gxm_state_epoch`: clears, blits, the scissor test's stencil draws, fixed-function and legacy draws) and at every scene
begun or ended. In Infected's mission frames most consecutive draws share program and stream layout and differ only in texture. On the
console (16 Oct, mission 1 with the bot) vitaGL's draw calls took 4.2 ms a frame with it against 4.6-7.4 ms without, the frame rate within the
run-to-run spread, and the pictures were right; Vita3K draws the same pictures with it on.

**Cheapest path, in order:** (1) one interleaved VBO per frame region, 20 bytes/vertex, filled once before drawing, attribute pointers set once, `first` varies; (2) program/blend/state sorted
runs; (3) uniform updates batched into attributes or per-run; (4) strips of ~28 vertices cost the same call overhead as longer ones, so merge strips with degenerate triangles
(index 0xFFFF restart is **not** used by `glDrawArrays`; use `glDrawElements` with an index buffer in a VBO) when the draw count is the bottleneck. **Expected** numbers (UNVERIFIED, T8 prints them): a few microseconds per draw for the
VBO path, noticeably more for client arrays; at 700 draws/frame the CPU time of the submission on core 1 is the budget to watch.

## 12. SceVideodec / SceAvcdec hardware H.264 (not exercised)

From `psp2/videodec.h` (vitasdk, `-lSceVideodec_stub`, `SCE_SYSMODULE_AVCDEC` must be loaded with `sceSysmoduleLoadModule`):

* `sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC = 0x1001, &SceVideodecQueryInitInfoHwAvcdec{size=0x14, horizontal, vertical, numOfRefFrames, numOfStreams=1})`.
* `sceAvcdecQueryDecoderMemSize(type, &SceAvcdecQueryDecoderInfo{horizontal, vertical, numOfRefFrames}, &info)` gives `frameMemSize`; the buffer for
  `sceAvcdecCreateDecoder(type, &SceAvcdecCtrl{handle, frameBuf{pBuf,size}}, &query)` **must be physically contiguous** (`SCE_AVCDEC_ERROR_NOT_PHY_CONTINUOUS_MEMORY 0x80620007`): allocate it with
  `sceKernelAllocMemBlock(..., SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, ...)` - the reason the vitaGL phycont pool is 0 by default.
* `sceAvcdecDecode(&ctrl, &SceAvcdecAu{pts, dts (init 0xFFFFFFFF), es{buf,size}}, &SceAvcdecArrayPicture{numOfElm, pPicture})` takes one access unit of Annex-B H.264 and writes `SceAvcdecFrame`s. Output
  formats: `RGBA8888`, `RGBA565`, `RGBA5551`, `YUV420_RASTER`, `YUV420_PACKED_RASTER` (`SceAvcdecPixelFormat`); with an RGBA format the decoder does the colour conversion
  (`SceAvcdecFrameOptionRGBA{alpha, cscCoefficient}`).
* Pros: no CPU use, any profile the hardware accepts, direct RGBA/YUV output usable as a texture (YUV through vitaGL's planar formats). Cons: needs contiguous memory, an extra system module, limited
  sizes and profiles (the library reports `SCE_AVCDEC_ERROR_UNSUPPORT_IMAGE_SIZE`), and Vita3K's support is partial.
  **Usability for the UMD cutscenes is UNVERIFIED** (no hardware here). T9 measures the software path (libavcodec h264 + NEON, single thread): per-frame time of a 480x272 baseline stream and CRC
  correctness; if T9 shows well under budget at the real bitrate, software decoding on core 2 is the simpler route and avoids the phycont dependency.
* Hardware audio decoders on the Vita (`SceAudiodec`) cover AT9, MP3, AAC and CELP; **ATRAC3/ATRAC3plus need software decoding** (libavcodec in the sysroot) - `psp2/audiodec.h` has no ATRAC3 type.

## 13. Open questions that the first hardware run answers

| Question | Where it shows up |
| --- | --- |
| Does the shader compiler load from `ur0:data/libshacccg.suprx` and how long does a pair take? | T6 `gl-init` log, `shaders-cold`/`shaders-warm` |
| Do `ATTRIBUTE2=12`, `_newlib_heap_size_user=144 MiB` and the 66 MiB arena + 96 MiB block fit? | T2, `platform init` memory lines in `log.txt` |
| Real `size_user/cdram/phycont` free values | T2 and `GL: init ...` line |
| Draw cost: client arrays vs VBO ring vs repoint vs orphan, uniform cost, blend churn, FBO round trip | T8 |
| Is depth preserved across a scene break / FBO round trip (needs `store`)? | T6 `depth-across-scene-break`, T7 `depth-across-fbo-switch` |
| Does the swap stall on the GC thread? Is `sceKernelGetThreadInfo` priority of GC sane? | T8 `swap` column, `vitaGL Garbage Collector` thread in `log.txt` |
| Uniform locations, translator gaps, varying limit | T6 `glsl_probes`, `uniform-locations` |
| Paletted texture filtering | extra manual check; T6 only compares NEAREST |

## 14. The move from `dca4b9d` to `9bf3e3f` (30 Oct 2026) and our patches

vitaGL pin: `9bf3e3f4e55efd1f4a191b179144f7f9a98cd925` ("Avoid setting dirty flags when program doesn't change in glUseProgram.", 30 Oct 2026).
`fetch-third-party.sh` applies `scripts/patches/vitagl-0001..0004` on top of it. Sections 1-13 above were read at `dca4b9d`; where this
section says otherwise, this section wins. Nothing here has run on the console yet.

### Our patches on the new base

| Patch | What it does | On `9bf3e3f` |
| --- | --- | --- |
| 0001 `circular-pool-align` | `vgl_reserve_data_pool` rounds every reservation up to 16 bytes | applies unchanged; upstream did not touch the function |
| 0002 `texsubimage-no-readback` | `_glTexSubImage2D` skips reading the old texels back when the update replaces a whole single-level texture | applies unchanged |
| 0003 `skip-redundant-state` | `vglSkipRedundantState`: custom-shader draws leave out GXM program/texture/stream calls that repeat the draw before | applies; hunk offsets moved (+5 to +10 lines in `custom_shaders.c`, `vitaGL.h`) and **one semantic change**, below |
| 0004 `reserve-compressed-mips` | `vglReserveCompressedMips`: level 0 of a compressed texture reserves the whole mip chain | applies; the `vitaGL.h` hunk moved from line 1161 to 1171 (upstream added the splashscreen types above it) |
| 0005 `no-cache-for-failed-compile` | `if (!s->prog) return;` before the shader is serialised | **dropped**: upstream `76f43e6` is the same two lines in the same place (checked in the built tree) |

**Patch 0003 and the new `glUseProgram`.** Upstream `9bf3e3f` makes `glUseProgram(p)` return at once when `p` is already current. Before, it
marked the vertex and fragment uniform sets dirty (`dirty_shader_vert_unifs`, `dirty_shader_frag_unifs`), which made the next draw copy
the program's uniform block into a fresh default uniform buffer. Those two flags are global, not per program, and the buffer reserved is the
GXM context's *current* default uniform buffer. Every other path that sets a GXM program also reserves its own default uniform buffer behind the
custom-shader draws' back: `glClear` (`misc.c`), the framebuffer blit (`framebuffers.c`), the scissor-test mask draw (`tests.c`), the fixed
function pipeline (`ffp.c`) and the legacy draws (`draw.c`). Patch 0003 already lists exactly those paths (plus every scene begun or ended)
in `vgl_gxm_state_touched()`, so the macro now also sets both dirty flags. The effect: after any of them the next custom-shader draw uploads
its uniforms again, with or without a `glUseProgram` in between, which is what a repeated `glUseProgram(same)` used to guarantee. The cost is
one uniform block copy after a clear, a blit, a scissor mask draw or a scene change; the gain is that nothing relies on the caller's
`glUseProgram`. Nothing is done twice: the patch's record concerns GXM programs, textures and streams; upstream's early return concerns the
uniform flags, and the macro is the only place they meet. `gl_vgl_skip_redundant=0` still turns the skipping off; the uniform flags are set
in either case (that part is a correction, not an optimisation, and costs nothing on the custom-draw path).
The port's renderer calls `glUseProgram` only on a program change (its own `cache.program` check), so for Infected the early return changes
nothing by itself; the dirty marking above matters for the clear/blit/mask paths, which the renderer does use (`glClear` with a scissor,
the platform composite, the post passes).

### Upstream commits since `dca4b9d`, read against the port's GL calls

The port draws with GLSL ES programs only (no fixed function, no `glMatrixMode`, no display lists, no `glUniform4iv`, no 8-bit indices):
16-bit `glDrawElements` from scratch-memory buffer objects (`vglBufferData`), `glDrawArrays` from client arrays for blits and the overlay,
`glUniformMatrix4fv(.., 1, GL_FALSE, ..)` for the transform and world matrices, `glUniform4fv` (count 1, and count 4 for the 16-float
uniforms that are not matrices), `glUniform2fv` count 1, `glUniform1i` for the sampler, up to about ten uniforms a draw after the renderer's
own change test.

| Commit | Change | Effect on Infected |
| --- | --- | --- |
| `9bf3e3f` | `glUseProgram` returns when the program is unchanged | none by itself (the renderer already filters); see the 0003 merge above. Helps the overlay and any caller that repeats the call |
| `36219c3` | `vgl_fill_uniform_data`: for `count == 1`, 1-4 components, element 0 and F32/S32 source of the same type as the shader's, a straight `memcpy` of 4-16 bytes into the vertex and fragment uniform buffers | **helps**: every `glUniform4fv(..,1,..)`, `glUniform2fv` and `glUniform1f/1i` skips `vglSetUniformData` (its switch on the type and its call to `sceClibMemcpy`). Same bytes as before for the same type (checked against the branches of `vglSetUniformData`); other types, arrays and `offs != 0` take the old path |
| `6f56e3f` | `vglSetUniformData`: sizes 1-16, 24 and 48 bytes use inline fixed-size copies (`vgl_uniform_copy`) | **helps** a little: the 16-byte vec4 and 8-byte vec2 copies, the 24-byte case. The 64-byte matrices and the 16-float vec4 arrays still go to `sceClibMemcpy` |
| `37620de` | `glUniform4iv` used `count 1` where it passed `count` | none: the port never calls `glUniform4iv` (only `glUniform1i` and `glUniform{2,4}fv`, `glUniformMatrix4fv`) |
| `19bff53` | `glUniformMatrix{2,3,4}fv` with `transpose` and `count <= 4` use a stack buffer instead of the circular pool | none: the port passes `GL_FALSE` |
| `9a14d17` | the 8-bit to 16-bit index copy in `glDrawElements` is a restrict loop | none: only `GL_UNSIGNED_SHORT` indices are drawn |
| `3f7fd52` | fixed-function colour array reused across the attributes that map to it | none (no fixed function) |
| `3cb7872` | cheaper `rotate`/`translate`/`scale`, `matrix4x4_normal_matrix` | none (matrix stack and fixed function only) |
| `20f0123` | `vglForceAlloc(size, for_gpu)` (was `(size)`), new `vglForceAllocAligned` | **API change**; the port does not call either (grep of the port, PortableKit and the self-test: no use) |
| `76f43e6` | no cache write when a shader failed to compile | same as our dropped patch 0005 |
| `c2b3b8a`, `94c75ce` | custom boot splashscreen API (`vglSetupCustomSplashscreen`, `vglSetCustomSplashscreenHashColor`, `vglSplashCallback`) | none: every library variant is built with `NO_SPLASHSCREEN=1` (`-DSKIP_SPLASHSCREEN`), which compiles `splashscreen.c` down to the two no-op setters (the built `splashscreen.o` is 16 bytes of text, no splash data, no `splashscreen_thread`); the port never calls the setters, and there is no runtime default that shows a splash |
| `7635e3c`, `a85cb8d` | shader cache: `SHADER_CACHE_MAGIC` 3 (folder `v3`), file names `%016llX.gxp`, the `XX` folders made when a file is written instead of 512 at init, `v`/`f` made only when `v3` is new | **changes the cache layout the VPK ships**: see below |
| `1a3b247` | display-list build fix | none (display lists off) |

**Behaviour risks.** (1) The uniform fast path writes `size * 4` bytes with `__builtin_memcpy` where `vglSetUniformData` copied
`componentCount * 4`: the same bytes. It is taken only when the shader's uniform has the type of the source (F32 or S32). The port never calls
`vglUseLowPrecision`, so the translator keeps `float` (and therefore every `vec4`/`vec2`/`mat4` uniform) in 32 bits whatever the
`precision` lines say (section 6) and the fast path applies to all of the port's uniforms; if a uniform were half, it would take the converting
path as before. The fast path can only skip work, never change a value. (2) `glUniform4iv` is the only call whose result changes in meaning (arrays); not used here. (3) The unpadded-to-padded file name means a
1.0 cache is *invisible* to the new library: the new build looks in `shader_cache/v3` and compiles anything it does not find (needs
`libshacccg.suprx`; about 40-90 ms each at first draw, or 1.5 s for the whole shipped list at start with the program list). Ship a converted
cache. (4) vitaGL's FFP shader cache (`ux0:data/shader_cache/v<FFP_SHADER_CACHE_MAGIC>`) is not used by the port.

**Shader cache conversion.** `scripts/shader-cache-to-v3.sh SRC DST` copies `SRC/v2/{v,f}/XX/<hash>.gxp` to `DST/v3/{v,f}/XX/<hash padded to 16>.gxp`.
The serialised shader (`serialize_shader`/`unserialize_shader`) did not change between the two commits, only the folder and file names did. The
1.0 release cache has 70 files, 10 of them with a 15-digit name that the new library would not find. Use the result as `build.sh --shader-cache DST`.
A device that ran 1.0 keeps `ux0:data/infected/shader_cache/v2` and gets `v3` next to it; the two do not disturb each other.

### Comparing old and new on the console

Everything but patch 0003 is compiled in, so the A side is the 1.0 `eboot.bin` (vitaGL `dca4b9d`). Steps are in `LOG.md` (30 Oct 2026 entry).
