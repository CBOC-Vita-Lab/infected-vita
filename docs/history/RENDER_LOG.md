# GL renderer work log

Branch `gl-renderer` of PortableKit (from `infected-bringup` df5ff52). The proj clone has no changes.

## Commits (PortableKit)
- af309df  Abstract `gpu::Renderer` interface and GE frame recorder (`<PREFIX>_GE_DUMP`, `_GE_DUMP_FLIPS`).
- f058fb0  GLES2 renderer, shaders and `ge_replay`.
- 1b1bc66  Pixel mask (depth-only clears), stencil/mask IR fields, settings parser, replay timing split.
- ca26c83  Replay merge-break statistics; scripted analog input for the bring-up pad script.
- 26424be  Cheaper frames (write-back off, list window, triangle fast path, target verdict memo), upload self test, `docs/GL_RENDERER.md`.

## Steps
1. Interface + recorder: dump holds the page-granular memory delta between events; flips selectable as N, A-B, A-B/STEP.
2. GlRenderer: one FBO per guest framebuffer (address, stride), depth per target, 6-flag program key (11 variants seen), DrawState memcmp merge, client-array vertices.
3. Corpus recorded with the interpreter (probe.sh, pad scripts): logos, title, menus, briefing, gameplay (run3, run5, run9 dense), pause, death, mission failed.
4. Verified by looking at replays and contact sheets; HUD compared with the debug rasteriser shots (s17).
5. Cost cuts: write-back off by default, 4 KiB command window in the list walk, direct index writing for triangle lists/strips/fans, memo of "is this address a target" verdicts, uniform dirty tracking, rotating sampled texture revalidation.
6. Offscreen scale measured: 1x vs 2x differ by RMSE 0.6-1.3 percent and 2x costs +17 percent time, so the default is 1.
7. Movie path: `upload_frame` validated with `ge_replay --upload-test` (synthetic quadrants, PASS). No real movie frame could be captured in the scripted flows (see below).

## Verification notes
- Live GL run (interpreter, headless llvmpipe) vs replay of dumps recorded during that run: 19 of 21 flips (400-1400, every 50th) identical pixel for pixel; the other two are subset-replay effects (a screen the game drew once into a buffer, or into a GPU-only render target, in flips that were not recorded).
- The "missing menu text" seen in two early live runs was a stalled guest (same menu frame for 800 presented frames, the game never left the menu transition); three later runs with the same binary and script showed the text and matched the replay. Not a renderer fault.
- Menu black bar under the tabs on the CUTSCENES screen: a 2x2 palettised texture whose texels are all opaque black (the game's own "MOVIES:" label strip), not a renderer fault.
- Movies: no real movie frame captured. The cutscene movie decodes (about a thousand `sceMpegAvcDecode` pictures) while the game submits one blit per flip and the picture stays frozen; `ge_replay --upload-test` is the only check of `upload_frame`. The legal splash at boot is a plain texture.
- Replayer now performs GE block transfers like the host (none occur in the recorded corpora).
- Final desktop timing, heavy gameplay (run9, 131 flips): GeState walk + vertex decode 1.2 ms, GL submission CPU 1.75 ms (null draw), 552 GL draws after merging 722 submits.

## Open items
- Stencil is not emulated (the game writes it but never tests it in the recorded frames).
- Tiny swizzled textures (row shorter than 16 bytes, fewer than 8 rows) are not unswizzled by `texture_decode` (shared with the Vulkan renderer); not seen to matter in the corpus (the one found is all zero).
- Cutscene movie presentation (see above).
