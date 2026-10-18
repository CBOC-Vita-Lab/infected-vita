# The OpenGL ES 2.0 renderer (and what vitaGL makes of it)

`host/gpu/gl/` is a second implementation of `gpu::Renderer` (the first is the Vulkan one). It turns the
`DrawCall` list that `GeState` produces into pictures with OpenGL ES 2.0 and GLSL ES 1.00 only, from one
source for two targets:

* the desktop (Mesa, any ES 2.0 driver) through EGL, headless if need be, which is how it is developed and
  checked, and
* the PlayStation Vita through [vitaGL](https://github.com/Rinnegatamante/vitaGL) (the sources read for this
  note are commit `dca4b9d`).

Nothing in it is specific to a game. This document is the contract with vitaGL: what the renderer asks of GL,
what vitaGL does with each of those requests, and what has to differ on the Vita. Everything below that says
"vitaGL" was checked against its source, not against a Vita; **nothing here has run on a Vita**.

## Layout

| File | What it is |
| --- | --- |
| `host/gpu/renderer.hpp` | `gpu::Renderer`, the abstract interface `hle_media.cpp` draws through. Vulkan keeps compiling behind it. |
| `host/gpu/gl/gl_renderer.{hpp,cpp}` | `GlRenderer`: targets, draw batching, state caching, blending, texture cache, presenting. |
| `host/gpu/gl/gl_shaders.{hpp,cpp}` | The GLSL ES 1.00 sources, assembled from a six-flag program key. |
| `host/gpu/gl/gl_platform.h`, `gl_platform_egl.cpp` | The one place that differs per platform: how a context and a pixel-less surface are made. A Vita build supplies its own (`vglInit`). |
| `host/gpu/ge_dump.{hpp,cpp}` | The frame recorder and its reader. |
| `tools/ge_replay/ge_replay.cpp` | Replays recorded frames into the renderer: PNGs, statistics, timing. No game, no guest code. |

Build switches: `-DPORTABLEKIT_GL_RENDERER=ON`, `-DPORTABLEKIT_GL_INCLUDE_DIR=<dir with GLES2/ and EGL/ headers>`
and `-DPORTABLEKIT_GL_LIBRARIES=<libEGL;libGLESv2>`. The GL entry points are linked directly (no loader), which
is also what vitaGL wants. At run time `<PREFIX>_RENDERER=gl` selects it, `<PREFIX>_GL_<SETTING>=<value>` changes a
setting (the names are `gl_setting_names()` in `gl_renderer.hpp`), and `<PREFIX>_SCREENSHOT_DIR` captures what the game showed.

## Design in one page

* **Window rows are PSP rows.** The GE's y axis points down, so the vertex shader flips clip y (`u_flip`) and a
  target's row 0 is the top of the picture. Reading a target back needs no flip, and a PSP framebuffer
  sampled as a texture is the right way up without a second flip. The viewport comes from the GE's
  scale/offset and region offset; negative scales are folded into `u_flip`; the depth range comes from
  `z_offset ± z_scale`, so the PSP's reversed range (Infected: scale -32768, offset 32767, test GEQUAL) works
  with `glDepthRangef` and `glDepthFunc` alone.
* **One framebuffer object per guest framebuffer address.** Each has its own 16-bit depth renderbuffer. A
  draw whose texture lies in a target the GPU drew (same address range, same row length, a format of that
  pixel size, guest memory under it unchanged since) samples the target's colour texture in place; a draw that
  samples the target it is drawing into gets a copy first (`glCopyTexSubImage2D`, counted as `feedback_copies`).
* **A draw is a vertex run plus a `DrawState`**: every GL state, uniform and texture it needs, as plain 32-bit
  words. A draw whose state is memcmp-equal to the batch in flight is appended to it; anything else flushes the
  batch. Applying a batch goes through caches that skip every GL call whose result is already in effect.
* **Programs are keyed by six flags** (through, light, fog, texture, alpha test, rect clamp); everything else
  a draw varies in (texture function, alpha function and reference, blend mode, lights, fog, matrices) is a
  uniform. Eleven programs are enough for Infected from the logos to a mission.
* **Blending without `glBlendColor`.** `GL_CONSTANT_COLOR` factors are used only where the driver has them and
  the setting `no_blend_color` is off; otherwise the fragment shader pre-multiplies (`u_blend.x`: 1 doubles the
  source alpha, 2 doubles and complements, 3 multiplies by a fixed colour, 4 a fixed colour against its
  complement). The two paths produce identical pictures here (checked with `--set no_blend_color=1` on the
  whole corpus).
* **Pixel masks, stencil, colour test.** The GE's pixel mask becomes `glColorMask` (a mask that protects all of
  RGB and alpha is how the game clears depth only: a full-screen sprite with `MASKRGB=FFFFFF`,
  `MASKALPHA=FF`). Stencil is in the IR but not emulated (see "Not done").
* **Texture cache** (below) and **cost counters** (`GlStats`, printed by `ge_replay --stats`).

## Settings that matter on a Vita

`display_scale` (2 = 960x544), `offscreen_scale` (1), `no_blend_color` (on), `depth_bits` (16), `writeback` (0),
`texture_check_interval` (30), `texture_budget_mb`, `merge`, `rect_clamp`. Defaults are chosen for the Vita;
the replayer's `--set KEY=VALUE` and `<PREFIX>_GL_<KEY>` change them.

* **Offscreen scale: 1.** The smaller targets Infected renders into and reads back (480, 240 and 120 pixels
  wide: the glow and bloom chain) are drawn at the PSP's own size and magnified when sampled. Against 2x the
  difference over the corpus is RMSE 0.6-1.3 % of the glow frames and +17 % replay time, so 1x is the default;
  `offscreen_scale=2` is the setting if a game's offscreen targets are sharp on screen.
* **Write-back: off.** Presenting does not read the display back into guest memory. The read-back is a
  `glReadPixels` of the whole display plus a conversion pass: a pipeline stall on the Vita and about 2 million
  instructions per frame. A GE block transfer whose source is a framebuffer still reads that framebuffer back on
  demand (`read_back_framebuffer`), which is the case a game copying the last frame needs. A game whose CPU
  reads the display itself sets `writeback=1` (every frame) or N.

## vitaGL readiness

### Entry points

Every GL function the renderer calls (65) is in `vitaGL.h` except `glBlendColor`, which vitaGL does not have;
the renderer only calls it when the driver reports it and `no_blend_color` is off, and the Vita build runs with
`no_blend_color=1`. `GL_EXT_blend_minmax` equations are used only if the driver has them; vitaGL's
`glBlendEquation` takes `GL_MIN`/`GL_MAX` (blending.c) and `glBlendEquationSeparate` exists.

### Feature table

| Feature | What the renderer does | vitaGL (source) | What to know |
| --- | --- | --- | --- |
| Vertex attributes | Four attributes from one 40-byte struct in a client array: `vec4` float position, `vec3` float normal, `vec2` float texcoord, colour as `GL_UNSIGNED_BYTE` normalised x4. | `glVertexAttribPointer` accepts float, half, fixed, (u)short, (u)byte, normalised or not (custom_shaders.c:3239). | Attributes must be bound by `glBindAttribLocation` before link; the renderer does. A smaller vertex (fixed-point position, 16-bit texcoords) is possible but costs nothing to leave: about 1 MB of vertex data per heavy frame. |
| Draw calls | `glDrawElements` with `GL_UNSIGNED_SHORT` indices from client memory, at most 65535 vertices per batch (the batch flushes before it overflows). `glDrawArrays` only for lines/points when unindexed. | `MAX_IDX_NUMBER` is 0xC000 for `glDrawArrays` (shared.h:62); indexed draws are not limited by it. | Keep `glDrawElements` for everything; the renderer already does. |
| Shader compile | `glShaderSource`/`glCompileShader`/`glLinkProgram` at first use of a program key. | vitaGL translates GLSL to Cg and compiles it at run time with `libshark` (`vglSetupRuntimeShaderCompiler`), strips `precision` statements (glsl_utils.c:1343), supports `glShaderBinary` with precompiled GXP and, built with `HAVE_SHADER_CACHE=1`, a cache under `vglSetShaderCachePath`. | Run-time compilation is slow enough on the Vita that vitaGL has a cache for it (unmeasured here: Mesa takes 41 ms for all 11). With 11 variants, **compile them on a loading screen, or ship the cache / precompiled GXPs**. The list of variants is below. |
| Preamble collisions | Identifiers are chosen to avoid vitaGL's `#define`s. | The translator header (glsl_translator_hdr.h) defines `s`, `t`, `st`, `ts`, `ss`, `tt`, `matrix`, `Matrix`, `sampler`, `mod`, `fract`, `mix`, `atan`, `texture2D`, `lowp`... as macros. | The sources avoid all of those, and avoid names that are Cg/HLSL library functions (`distance`, `clip`, `function` were renamed). Keep it so when adding shader code. |
| Varyings | Five: `v_texcoord`, `v_color`, `v_rect`, `v_specular`, `v_fog` (not all in every program). | Varyings are mapped to TEXCOORDn (glsl_utils.c, `FOUT`/`FIN`). | vitaGL binds ten (`MAX_CG_TEXCOORD_ID`, shared.h:800); five are used. |
| Uniforms | `vec4`, `vec2`, `mat4` and `vec4[4]` arrays; one `sampler2D`. `glUniform*` through locations from `glGetUniformLocation`. | Uniform arrays and matrices are supported. A location is a pointer-like cookie, not a small index. | The renderer tests locations `!= -1`, never `>= 0` (a cookie can look negative), and skips uniforms the compiler removed. A uniform's dirty tracking avoids 6400 of 6600 `glUniform` calls per frame. |
| Lights | Per-vertex lighting in the vertex shader: 4 lights, `for` loops with constant bounds, arrays indexed by the loop counter. | Loop bounds are constants, so the compiler may unroll them. | The lit variants are the heaviest vertex programs; if the Vita compiler rejects one (indexing a uniform array by a loop counter is the likely cause), the lights can be unrolled, the GLSL being regular. Unverified on hardware. |
| Fragment `discard` | The alpha test is `if (a fails) discard;` in programs with the alpha-test flag only. | Translated to Cg `discard`. | A fragment shader that can discard costs the GPU its early depth rejection on tile-based hardware: the alpha-test flag is on only for draws that have an alpha test (the renderer does that). |
| Blend | `glBlendFuncSeparate` with `SRC/DST_COLOR`, `ONE_MINUS_*`, `SRC/DST_ALPHA`, `ZERO`, `ONE` and equation `FUNC_ADD`/`SUBTRACT`/`REVERSE_SUBTRACT`. | All of those are supported (blending.c:123-300); there is no constant colour. | The shader path covers the "fixed colour" factors. The PSP's `2*DST_ALPHA` factors are approximated by `DST_ALPHA` (counted as `approximated blends`; zero in the corpus). |
| Depth | `glDepthFunc` all eight functions, `glDepthMask`, `glDepthRangef`, 16-bit depth renderbuffers. Reversed range via `glDepthRangef(near > far)`. | `glDepthRangef` and all functions supported. vitaGL's depth buffer is `DF32M` (32-bit float, gxm.c:513) whatever the renderbuffer format asked for. | `glDepthRangef(near, far)` becomes a viewport z port and scale of `(far+near)/2` and `(far-near)/2` (misc.c:359), so a reversed range is a negative scale; not run on hardware. If it misbehaves, fold the reversal into the vertex shader's z (`z' = 1 - z`) and mirror the function; the draw state holds everything needed. |
| Scissor | `glScissor` on the GE's scissor rectangle. | vitaGL implements scissor with a stencil-mask pass unless built with `STORE_DEPTH_STENCIL` (tests.c, gxm.c). | A scissor change is not free there. Infected changes scissor about 5 times per frame. |
| Clear | `glClear` for through-mode rectangle clears (none in Infected: it clears by drawing). | `glClear` ends the GXM scene if the framebuffer changed and draws a quad (misc.c:643). | Clearing by sprite draw (what Infected does) is as cheap. |
| Colour mask | `glColorMask`. | Supported. | The depth-only clear relies on it; changes about 2 per frame. |
| Culling, front face | `glCullFace`, `glFrontFace`. | Supported; vitaGL keeps a separate cull mapping for the display and for FBOs (misc.c:196). | Every game draw goes into an FBO (the display is one too; only the final blit touches the window). vitaGL's default flips FBOs "to match OpenGL standards" (a negated viewport y, misc.c:334, and the matching cull mapping), so a texture attached to one is sampled the way it was drawn, which is what the renderer assumes. **Do not build vitaGL with `HAVE_UNFLIPPED_FBOS`.** If culling comes out inverted on hardware, `flip_winding` reverses it. |
| Render to texture | `glFramebufferTexture2D` (RGBA8 colour texture) plus `glFramebufferRenderbuffer` (depth). | Supported (framebuffers.c). A framebuffer with an attached texture starts a new GXM scene when bound. | Infected binds a different target about 5 times per frame; each switch is a scene. Keep the targets few and the draws per target grouped (the renderer does not reorder). |
| Texture upload | `glTexImage2D` RGBA8 from the decoded texture, `glTexSubImage2D` for a texture re-uploaded at the same size, `GL_NEAREST`/`GL_LINEAR`, `GL_REPEAT`/`GL_CLAMP_TO_EDGE`, no mipmaps. | All supported. `glTexSubImage2D` on a texture used within the last `FRAME_PURGE_FREQ` frames first copies it to new memory and frees the old (textures.c:947, unless vitaGL is built with `TEXTURES_SPEEDHACK`). | A texture rewritten every frame (`upload_frame`, a movie) pays that copy each time; rotate two or three textures instead on the Vita. Decoded textures are RGBA8: 4 bytes per texel. The 565/5551/4444 uploads vitaGL supports would halve texture memory (the corpus has 17 MB at 375 textures); not done. Palette textures (`GL_COLOR_INDEX`-style) exist in vitaGL and could skip the decode of 8-bit CLUT textures; not done. |
| Texture units | One unit, one sampler. | Fine. | Two textures per draw would need a second unit; the GE never samples two. |
| `glReadPixels` | Only for tools (`capture_frame`, replay PNGs) and `writeback`. | Supported; synchronous. | Never in the frame loop (the default). |
| Errors | `glGetError` after setup and in the replayer; not per draw. | `SKIP_ERROR_HANDLING` builds always return `GL_NO_ERROR`. | |
| Extensions | `GL_EXT_blend_minmax` (optional). Nothing else. | | `glGetString(GL_EXTENSIONS)` is only used to enable that. |

### What has to differ on the Vita

1. `gl_platform_egl.cpp` is replaced by a platform file that calls `vglInit*` and presents with `vglSwapBuffers`.
   Display scale 2 renders into the 960x544 framebuffer.
2. vitaGL build options: `HAVE_SHADER_CACHE=1` (or precompiled GXPs through `glShaderBinary`), no
   `HAVE_UNFLIPPED_FBOS`; call `vglSetupRuntimeShaderCompiler` before `vglInit*`.
3. Warm all program variants during loading (11 for Infected; `GlRenderer::shader_variant_names()` lists
   what a run compiled, `ge_replay --stats` prints it).
4. `no_blend_color=1`, `writeback=0`, `depth_bits=16`, `offscreen_scale=1` (the defaults).
5. Lower `texture_budget_mb` to what is left of CDRAM after the framebuffers and the vitaGL pools (the desktop
   default of 96 MB is far more than the corpus needs: 17 MB).
6. Run the renderer on the thread that owns the GXM context; `submit()` is not thread-safe and is called from the
   GE walk's thread only.

### Shader variants

The program key has six flags, 64 combinations in principle; a run from the logos through the menus into a
mission and a pause used these eleven (`ge_replay --stats` lists them):

```
through                         2D untextured quads and bars
through+tex+rect                HUD, menus, text, movies (clamped texture rectangle)
transform+tex                   unlit world geometry
transform+tex+alpha             alpha-tested world geometry
transform+light+tex             lit characters
transform+light+tex+alpha       lit, alpha-tested
transform+light+fog+tex+alpha   lit, fogged, alpha-tested
transform+fog+tex+alpha         fogged, alpha-tested
transform+fog+alpha             fog without texture
transform+light+fog+alpha       lit and fogged, no texture
transform+light+alpha           lit, no texture
```

A game that uses more flags compiles more programs on first use; the count stays small enough to list and warm.

## The texture cache

A texture is decoded (`texture_decode`) and uploaded once, keyed by address, size, row length, format and
swizzle, and the palette's identity (address, format, shift/mask/offset and a hash of the entries the draw can
reach). Guest memory can change under a cached texture, so a cached texture is revalidated by hash, but not
every use:

* the first four uses, and the following 240 frames after a texture was ever found changed, hash the whole
  texture (a video or a text texture rewritten every frame shows itself within a few frames);
* otherwise a rotating eighth of the texture's 256-byte blocks is hashed every `texture_check_interval` (30)
  frames, so a texture changes visibly at most about 240 frames late (never, for the textures the corpus
  contains: no revalidation of a settled texture found a change);
* a texture in memory the GPU renders into is purged at once; a draw that samples a render target reads the
  target itself and never the guest bytes.

`texture_always_validate=1` is the old behaviour (hash and compare on every use) for comparison.

Numbers are in "Cost report" below.

## Recording and replaying frames

A recorded frame is everything the GE side of a flip depends on: guest RAM and VRAM as they are when the
flip's first display list runs (stored once per 4 KiB page, zlib-compressed, with each later event storing only
the pages that changed), the GE state machine's state, and the ordered events (list runs, CPU-written pictures,
the flip). The replayer restores them and runs the same lists through an unmodified `GeState`.

```sh
# record: flips 1830-1990 (every one) and every 200th from 2000 to 8000, on the interpreter, headless
INFECTED_GE_DUMP=/path/dumps INFECTED_GE_DUMP_FLIPS=1830-1990,2000-8000/200 \
INFECTED_NO_RENDER=1 INFECTED_NO_AUDIO=1 INFECTED_FRAME_LIMIT_US=16683 INFECTED_CPU_NS_PER_INSN=4 \
INFECTED_PAD_SCRIPT='250:CROSS,258:-,...,1830:LY=0+CROSS' timeout 1500 bin/InfectedNative

# replay to PNGs (headless EGL on Mesa llvmpipe), with per-flip statistics
EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 ge_replay /path/dumps --png out/f.png --stats
ge_replay /path/dumps --flips 4000-4002 --null-draw --repeat 10 --stats    # CPU cost without GL draw calls
ge_replay /path/dumps --flips 4000 --analyze                                # list the draws, no rendering
```

The pad script accepts the buttons by name joined with `+` and the analog axes as `LX=`, `LY=`, `RX=`, `RY=`
(0-255); steps are `AT:BUTTONS`, `AT` being a flip number, `<n>ms` or `<x>s`. A flip number is the better clock
for a menu walk, but a run is not repeatable to the flip: a key held for 8 flips is sometimes seen twice by the game's
own auto-repeat, so press for 3 flips and look at the picture before trusting a long script.

Replays of a sequence of non-consecutive dumps keep the targets and texture cache from one to the next, which is
right for a recorded run but means a frame that relies on earlier contents (a screen the game draws once and
leaves in the buffer) shows the contents the replay had, not the game's: replay a consecutive range to check it.

The same holds for a render target the game fills once and only blits afterwards: its pixels live on the GPU and
guest memory is not written back (only the displayed frame is, and only with `writeback`), so a dump that starts
after the target was drawn replays that target blank. Record from before the screen is built.

Live against replay: a run with the GL renderer (`<PREFIX>_RENDERER=gl`, headless EGL) that recorded flips
400-1400 every 50th and captured its own frames at the same numbers matched the replay of those dumps pixel for
pixel at 19 of 21 flips (menus, loading, a mission, death); the other two were subset effects of the kind above.

## Not done, not verified

* **Stencil.** The IR carries the stencil test and operations; the renderer does not emulate them. The game
  uses `ALWAYS/REPLACE` with reference FF on about 20 draws per 12 frames, which writes the alpha channel of the
  display, which nothing reads.
* **Colour test, logic ops.** Not used by Infected in any recorded frame (`ge_replay --regs`); not implemented.
* **Skinning is on the CPU** (`GeState` skins while decoding vertices, as for the Vulkan renderer). It is the
  largest single decode cost in a heavy frame.
* **Written before the first Vita run.** The vitaGL notes in this section come from reading its source. The renderer has since run on
  a console (mission 1 at 30 fps); what was measured there is in `vita/VITAGL_NOTES.md` and `docs/STATUS.md`.
* **Movies** (`upload_frame`): implemented and self-tested (`ge_replay --upload-test` draws a synthetic 480x272 picture
  whose quadrants and centre are known and checks them after `present`: PASS), but **no frame of a real movie was
  captured**. The interpreter decodes the cutscene movies (`sceMpegAvcDecode`, about a thousand pictures into a main
  memory buffer) while the game submits one draw per flip, the blit of the offscreen target it drew last, and no
  CPU-written picture, no block transfer and no flip from main memory; the picture stays frozen for the length of the
  movie. That is what the guest does under the bring-up HLE, not something the renderer was asked to draw, so it is a
  bring-up question (movie presentation or audio sync), not a renderer one.
