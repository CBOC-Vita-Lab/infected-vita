# Architecture and contracts

How the Vita port is put together, and the rules the parts agree on. This is the working contract the port was written to; it is kept as it was,
so it uses the words of the development (workstreams, the recompiler's "lean" form) and describes the target as well as what exists:
[`STATUS.md`](STATUS.md) says what has actually run, and [`BUILDING.md`](BUILDING.md) how the pieces are made.

Target: the US version (ULUS-10054, 1.02) on a PS Vita at 960x544. The game's own rate is 30 fps (60 was the first goal and is out of reach: see `STATUS.md`).

## Where things are

| Path | What |
|---|---|
| `recomp/` | the recompiler (`lean_gen.py`, `leangen/`), the profile of the game (`infected_profile.json`), the ABI headers its output includes, and its tests |
| `port/` | the game-specific part linked into PortableKit: the profile, the gameplay tuning, the bot |
| `vita/` | the Vita application (`app/`), the platform layer (`platform/`), the library build scripts (`scripts/`), the LiveArea assets, the release tooling |
| `patches/portablekit/` | the changes to [PortableKit](https://github.com/TeamGDB/PortableKit) (the runtime this builds on); `scripts/get-portablekit.sh` applies them |
| `tools/` | the texture-pack and HD-movie builders, the bot and the console test drivers, small tools (`tools/README.md`) |
| `docs/history/` | the working logs of the development, as written at the time |

## Known facts about the game (measured under PortableKit's interpreter)

- Boots to gameplay on PortableKit HLE: 228 of 234 imports implemented, 130 called on the way to mission 1.
- ~1.94 M guest instructions per flip in gameplay, ~0.5 M in menus. 611-758 draws per flip in gameplay.
- Not vblank paced: per frame `sceKernelGetSystemTime` + `sceKernelDelayThread(300us)` + `sceDisplaySetFrameBuf(sync=0)`.
  The game measures elapsed time, so it runs at correct speed at any frame rate.
- GE: transformed vertices, up to 8 bones (u8 weights), CLUT4/CLUT8 swizzled textures, 8888 render targets
  (480/240/120 wide) sampled back as textures, fog, 1-3 lights, alpha test on every draw, 4 blend combinations.
  No block transfers, no bezier/spline, no morph, no DXT.
- Audio: sceSasCore voices (thread_sfx), ATRAC3plus music via sceAtrac (AT3_Stream), `sceAudioOutputPannedBlocking`.
- Movies: 18 PMF via sceMpeg ring buffer (H.264 + ATRAC3plus).
- Uses scratchpad `0x00010000` (texture swizzle routines) and uncached aliases.
- Hot code: VFPU function `0x08935F54` (11.7 %), `0x089118B0` (6.8 %), byte-loop memcpy `0x08A2F228` (6.6 %), strcmp `0x08A2F47C`.

## Target architecture

```
core 0: guest executor  - lean recompiled game code + HLE kernel, guest threads as coroutines
core 1: GE thread       - walks display lists (GeState) -> DrawCall -> GL renderer (vitaGL on Vita)
core 2: audio/decoders  - audio out pump, ATRAC3plus and H.264 decode
```

That was the plan. As built, the renderer is split in two: the display-list walk and vertex decoding run on core 2 and the GL calls on core 1
(`STATUS.md`, "Where a mission frame goes"). Audio output and decoding cost a few milliseconds a frame (the `audio=` and `decode=` fields of the `[stat]` log line).

Phase order: everything first runs single-threaded and synchronous on desktop (Linux x86-64, headless),
then the GE and audio work moves to their own threads, then the Vita build.

### 1. Guest memory (all code must agree)

One flat arena, `uint8_t* g_ram`, size `0x04200000` (66 MiB), 1 MiB aligned.

```
host(a) = g_ram + (a & 0x05FFFFFF)        // keeps bits 26 and 24..0 ; one ARM `bic #0xFA000000`
```

| Guest range | Arena offset | Notes |
|---|---|---|
| `0x08000000..0x09FFFFFF` (and `0x48..`, `0x88..` aliases) | `0x00000000..0x01FFFFFF` | user RAM starts at `0x08800000` |
| `0x00010000..0x00013FFF` scratchpad | `0x00010000..` | lands in the unused kernel area |
| `0x04000000..0x041FFFFF` VRAM (and `0x44..` alias) | `0x04000000..0x041FFFFF` | |
| arena `0x02000000..0x03FFFFFF` | unused hole | |

No bounds checks, no write watches in generated code. PortableKit's `GuestMemory` must be backed by this
same arena (RAM, VRAM and scratchpad views into it) so HLE, GeState and generated code see the same bytes.

### 2. CPU state

`psprecomp::AllegrexContext` is kept as the one register-file type. There is ONE live instance, `g_cpu`
(== `Runtime::cpu()`); the kernel copies it out/in on a guest thread switch as PortableKit already does.

Lean generated code keeps guest registers in C locals. It touches `g_cpu` only for:
- VFPU registers and control (`g_cpu.vfpu[128]`, `vfpu_ctrl`) - always in memory;
- import calls (arguments/results marshalled through `g_cpu.gpr/fpr`);
- regfile-form calls (below).

### 3. Calling conventions in generated code

- **Typed form** (default): `RET f_ADDR(u32 sp, u32 a0, ..., float f12, ...)`, signature from interprocedural
  liveness, registers as locals, `jal` = C call, `jr $ra` = `return`.
- **Regfile form** (escape hatch, must make the generator total): `void r_ADDR()` operating on `g_cpu.gpr/fpr/hi/lo`.
  Used for functions that break the ABI assumptions. A typed caller stores what the callee may read into `g_cpu`
  and reloads what it may have written.
- **Entry thunks**: for EVERY function, `void t_ADDR()` = regfile-convention entry (loads args from `g_cpu`, calls the
  typed function, stores `v0/v1/f0` back). Exposed as a flat table:

```c
extern "C" void (*const g_lean_entry[])();     // index (pc - 0x08804000) >> 2 ; nullptr = not a known function start
extern "C" const uint32_t g_lean_entry_count;
```

- **Indirect calls** (`jalr`) and all runtime-to-guest calls go through:

```c
extern "C" void pk_guest_call(uint32_t pc);    // regfile convention on g_cpu. Runtime provides it:
                                               // g_lean_entry[idx] if present, else the interpreter runs the callee to its return.
```

- **Imports**: `jal` to an import stub -> marshal `a0..a7`, `sp`, `f12..f15` into `g_cpu`, then

```c
extern "C" void pk_hle_call(uint32_t stub_pc); // runtime runs the HLE function; may switch coroutines before returning
```
  then read `v0/v1/f0` back from `g_cpu`.
- **Untranslated direct targets**: `pk_guest_call(pc)` (interpreter fallback). The corpus is allowed to be partial
  during bring-up; it must be total before the Vita build.
- Generated code is compiled `-fno-exceptions -fno-rtti`. The runtime never unwinds through generated frames.
  Fatal conditions call `pk_fatal(const char*, uint32_t pc)` (noreturn).
- `break` traps -> `pk_fatal`. Division semantics, float->int conversion and NaN behaviour follow the interpreter.

### 4. Kernel execution model (replaces PortableKit's unwind-to-dispatcher model)

- Each guest thread is a coroutine with its own host stack (default 256 KiB, guest stacks stay in guest RAM).
  `co_switch` is hand-written per architecture (x86-64 SysV, ARMv7 AAPCS incl. VFP d8-d15); no ucontext.
- A blocking HLE call saves/loads `g_cpu` exactly as today, then switches coroutine. It returns normally in the
  blocked thread when that thread is rescheduled. Nothing unwinds.
- Thread start: a new coroutine whose entry calls `pk_guest_call(entry)` and then the thread-exit path.
- Callbacks, interrupt handlers (GE signal/finish, vblank sub-interrupts, alarms, vtimers), mpeg ring-buffer
  callbacks: delivered at HLE boundaries as nested `pk_guest_call` on the current coroutine, with `g_cpu` saved and
  restored around the handler.
- The interpreter runs on the same model (it is just another way to execute a guest function), so the kernel
  can be validated with zero lean code, and any function can be flipped between lean and interpreted at run time
  (env list) for bisection.
- Time: `Kernel::now_us()` has two sources: `virtual` (PortableKit's deterministic clock, default for tests) and
  `realtime` (host monotonic clock; idle sleeps on a host event with timeout). Vita uses realtime.
- No preemption inside generated code. If a guest loop needs it, the function is listed in a profile yield list
  and the generator emits `PK_YIELD()` on its back edges.

### 5. Renderer

`GeState` and the `DrawCall` IR stay as they are. The backend is behind an abstract `gpu::Renderer` interface
(`initialize`, `shutdown`, `begin_display_list`, `submit(DrawCall, GuestMemory)`, `present(addr)`,
`upload_frame`, `read_back_framebuffer`), with:
- `GlRenderer`: OpenGL ES 2.0 feature level, the same source for desktop (EGL, headless llvmpipe) and Vita (vitaGL).
  Internal resolution 2x (960x544) for the display targets, off-screen targets at native size x2 too unless that
  breaks the effect.
- A frame recorder/replayer (guest RAM + VRAM snapshot + the list runs of one flip) so the renderer can be
  developed and regression-tested without running the game.

### 6. Platform layer

Small interface per platform (`desktop_headless`, `vita`): window/GL context, present + vsync, pad, audio out,
file root, thread affinity, clock. No SDL, no Vulkan, no ImGui in the Vita build.

## Rules for every workstream

- Game-derived data (ISO, images, generated code, dumps, logs with game data) stays under `/work/infected/`
  or your own `/work/ws/<name>/private/`. Never commit it.
- Progress over ceremony: no evidence receipts. One meaningful check per milestone. But never fake a result: a stub
  that returns a plausible value is listed as a stub.
- Keep a short factual log in a `LOG.md` of the workspace.
- Commit your work on a named branch in your own clone and report repo path + branch + commit.

### 7. Port settings

All enhancements are user-switchable from an in-game overlay menu, grouped as Graphics and Performance, and persisted.

- File: `settings.txt` in the port data root (`ux0:data/infected/` on Vita, `$INFECTED_DATA_DIR` on desktop), `key=value` lines.
  Unknown keys are kept. Environment/`config.txt` (developer switches) override it.
- Overlay: opened by holding SELECT+START for half a second (and, on Vita, by tapping the top-right corner of the touch screen).
  While open the guest is paused (guest threads not scheduled, guest clock frozen), pad input goes to the overlay only.
  D-pad up/down selects, left/right changes a value, L/R switches page, CIRCLE or START closes and saves.
  Each row shows a cost hint (None / Low / Medium / High) and "restart needed" where true.
- One C++ header `host/settings/port_settings.hpp` owns the key list, defaults, allowed values, labels and cost hints;
  renderer, runtime and overlay all read from it. No other place hard-codes a key string.

| Page | Key | Values (default first) | Notes |
|---|---|---|---|
| Graphics | `gfx.hd_textures` | `auto`, `off`, `2x`, `4x` (`auto` = the 2x pack if installed, else the 4x pack) | texture pack, section below |
| Graphics | `gfx.texture_filter` | `smooth`, `original` | 3D draws only; HUD/2D keeps the game's own filter |
| Graphics | `gfx.mipmaps` | `on`, `off` | generated at upload or taken from the pack |
| Graphics | `gfx.antialiasing` | `off`, `fxaa`, `msaa2`, `msaa4` | MSAA = restart needed |
| Graphics | `gfx.pixel_lighting` | `off`, `on` | per-fragment lighting |
| Graphics | `gfx.effects_scale` | `1`, `2` | off-screen glow/blur targets |
| Graphics | `gfx.movie_filter` | `sharp`, `bilinear` | scaling of 480x272 movies |
| Graphics | `gfx.hd_movies` | `off`, `on` | 960x544 replacement movies from `movies_hd/` (docs/HD_MOVIES.md) |
| Performance | `perf.frame_cap` | `60`, `30`, `off` | |
| Performance | `perf.show_fps` | `off`, `on` | fps + frame time + draw count |
| Performance | `perf.resolution` | `960x544`, `720x408` | restart needed; 720x408 draws the display at 1.5x and enlarges it |
| Performance | `perf.threaded_renderer` | `on`, `off`, `split` | restart needed; split = list walker and GL thread on two cores |
| Performance | `perf.cpu_clock` | `444`, `500` | Vita only |
| Performance | `perf.enemy_density` | `100`, `125`, `150`, `200` | percent, gameplay patch (`port/host/infected_tuning.cpp`, docs/GAMEPLAY_TUNING.md); applies at the next mission load |
| Performance | `perf.view_distance` | `100`, `150`, `200` | percent, gameplay patch, live; the GPU cost is not measured |

**Texture pack.** `texpack_2x.bin` / `texpack_4x.bin` in the data root. Little-endian:
header `"IFTP"`, `u32 version=1`, `u32 count`, `u32 scale`; then `count` index records sorted by key:
`u64 key, u64 offset, u32 size, u16 width, u16 height, u8 format (0=RGBA8, 1=DXT1, 2=DXT5), u8 mip_count, u16 reserved`;
then the blobs (mip chain, largest first). `key` is the renderer's replacement key: a 64-bit hash of the
decoded-source identity (texel bytes as the GE sees them, the CLUT entries actually addressed, format, width, height,
swizzle flag), computed by ONE function `gpu::texture_replacement_key(...)` that both the renderer and the pack tools call.

**Texture pack, as implemented (texture-pack workstream).** The key is XXH3-64 chained over a 16-byte header (`"IFK"`, key
version 1, pixel format, palette format or 0xFF, swizzle flag, width, height, buffer width when it differs from the width), the
stored texel bytes (`stride * height`, or the DXT blocks) and, for CLUT textures, the palette entries in index order (entry
`((i >> shift) & mask) | (start << 4)` for every index `i` the format can produce: 16 for CLUT4, 256 for CLUT8), as stored. It
does not depend on the guest address, on where in CLUT memory the palette sits, or on the host. Cost: one pass over the texel
bytes and the palette, once per texture cache entry when it is first uploaded or found changed, never per draw (about 1.2 us
for a 64x64 CLUT4 texture, measured in `ge_replay --stats`; render-target copies, format 8888, are never keyed). The pack
index stays on disk except for the sorted keys: 8 bytes per texture in RAM, 28 bytes per record in the file.

Palette-swapped textures: the game rewrites some palettes at run time (about 32 of the 256 entries of the characters'
textures, taken from small palette textures), so a CLUT texture can have a palette no disc stream contains. The pack
therefore also holds, for characters and for every image with more than one palette on disc, a **recolour record**: flag bit 0
of `reserved` is set, the key is the *image key* (`gpu::texture_image_key`: the same hash with header magic `"IFI"`, the texel
bytes and no palette), and the blob is `u16 palette_entries, u16 encoding (1 = PackBits), u32 encoded_bytes`, the palette the
picture was upscaled with (RGBA8), the PackBits-encoded index map (the palette index each output pixel belongs to), and
level 0 of the picture in the record's format. When the full key misses, the renderer looks up the image key and rebuilds the
picture as `pixel + new_palette[index] - old_palette[index]` per channel (RGBA8 mip chain built at upload). A texture whose
palette the game rewrites again and again (the minimap) keeps its original: a palette slot (the texel memory and the palette memory it is drawn
from) is rebuilt for at most 3 changes in a row, each within 24 frames of the one before; the next change makes it an animation, and it then
stays on the original, which the renderer's own palette path handles, until its palette has not changed for 600 frames. Characters share texels
but not palette memory, so a crowd of one model is not an animation.

**Pack policy (Vita).** The Vita's `gfx.hd_textures=4x` reads the MIXED pack (4x world and effects, 2x characters and UI; 102 MB on the card, 26-37 MB
of texture memory for a whole level), installed under the name `texpack_4x.bin`; the format is unchanged. The full 4x pack (261 MB, ~90 MB for what
mission 1 draws) does not fit and is for the desktop. `auto` picks the 2x pack when it is installed (the safe one) and the 4x pack only when it is
the only one. The replacement budget per mode is 32 MB (2x) and 48 MB (4x); config.txt / environment keys `hd_budget_mb` (every mode),
`hd_budget_2x_mb`, `hd_budget_4x_mb` change it.

Offline tools: `tools/texpack/` (extract.py, upscale.py, pack.py; `tools/texkey` in PortableKit computes keys with the same code
as the renderer). `INFECTED_TEX_DUMP=<dir>` (and `INFECTED_TEX_DUMP_RAW=1`) writes every distinct texture the renderer sees as
`<key>.png` + `<key>.json` (+ `<key>.raw`); `INFECTED_HD_TEXTURES=off|2x|4x` and `INFECTED_HD_BUDGET_MB` override the setting.

