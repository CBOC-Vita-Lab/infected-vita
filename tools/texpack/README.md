# HD texture pack tools (Infected, native Vita port)

Builds `texpack_2x.bin`, `texpack_4x.bin` and `texpack_4x_mixed.bin` from your own copy of the disc. The packs hold upscaled versions of every texture
of the game; the renderer (`host/gpu/gl/gl_hd_textures.*`) swaps a texture for its pack entry when it uploads it, matching by a hash of the texels and
palette (`docs/ARCHITECTURE.md`, section 7, has the file format and the key). Nothing derived from the disc may be committed, uploaded or attached to a
report: the packs, the extracted textures and the upscaled pictures are the game's artwork.

## One command (Apple-silicon Mac, or any Linux/macOS machine with Python 3.10+, cmake, a C++ compiler and git)

```
tools/texpack/build_packs.sh /path/to/Infected.iso
```

It makes everything under `./texpack-work/` (override with `TEXPACK_WORK=`) and leaves the packs in `./texpack-work/packs/`. Every step is
resumable: run the same command again after an interruption (finished images are skipped). Steps:

1. a Python virtual environment (`numpy pillow opencv-python-headless texture2ddecoder torch`; a plain `pip install torch` on an Apple-silicon Mac
   includes the MPS backend),
2. the model weights, downloaded from their authors (`fetch_models.sh`; checked against sha256 sums, see Provenance),
3. the DXT encoder (`bcenc/build.sh`: rgbcx from bc7enc_rdo, compiled with `c++`),
4. `texkey`, built from the patched PortableKit checkout (`third_party/PortableKit`, made by `scripts/get-portablekit.sh`; `PORTABLEKIT=` points elsewhere; `TEXKEY=` uses a ready binary),
5. `extract.py`: reads the level streams out of the ISO and keys every texture exactly as the renderer does (2,497 distinct images),
6. `upscale.py` fast pass (every image) and best pass (characters with the large network). The device is picked automatically: `mps` when torch reports
   it, else `cuda`, else `cpu`; override with `TEXPACK_DEVICE=`,
7. `pack.py`: DXT1/DXT5/RGBA8 with mip chains, recolour records, the three packs.

Smoke test of the whole chain in a few minutes: `TEXPACK_LIMIT=12 tools/texpack/build_packs.sh Infected.iso`.

Measured on the Linux build machine (2 CPU cores, shared with other jobs, no GPU): extract about half a minute, fast pass about 45 min wall (13.7 Mpx), best pass about
65 min (1.8 Mpx of characters through the large network), pack 2x 47 s, 4x 254 s, mixed 93 s. UNVERIFIED on a Mac: the PyTorch MPS path
(`TEXPACK_DEVICE=mps`, `PYTORCH_ENABLE_MPS_FALLBACK=1` is set so an unsupported operator falls back to the CPU instead of failing) is expected to be several times
faster than the CPU, but I could not run it here. Results differ in the last bits between devices, so a pack made on a Mac is not byte-identical to one made on
Linux (on one machine the build is deterministic: two builds gave identical sha256).
`realesrgan-ncnn-vulkan` is not used: it cannot load the HDCube network, which is what the world textures need.

## Install

Copy the packs next to the game's other data:

| Where | Path |
|---|---|
| PS Vita | `ux0:data/infected/texpack_2x.bin`, `ux0:data/infected/texpack_4x.bin` |
| desktop | `$INFECTED_DATA_DIR/` (the data directory of the build) |

`gfx.hd_textures` (`off`, `2x`, `4x`; default: the best pack present) or `INFECTED_HD_TEXTURES=off|2x|4x` selects it. `INFECTED_HD_BUDGET_MB` caps the
GPU bytes of replacement textures (a texture that does not fit keeps its original).
`texpack_4x_mixed.bin` is the same format with a scale per class (4x world and effects, 2x characters and UI, see below): save it as `texpack_4x.bin`
to use it as the 4x pack. It is 102 MB instead of 274 MB and needs a third of the GPU memory for the same look at 960x544.

GPU memory (bytes the renderer holds, measured over all 45 level streams of the disc): all textures of one level are 18-22 MB with no pack, 18-21 MB at 2x
(DXT), 26-37 MB with the mixed pack and 71-87 MB with the full 4x pack; one gameplay frame touches 4.6-14 MiB at 2x and 18-42 MiB at 4x. A full 4x pack
therefore does not fit a Vita (128 MB of video memory shared with the render targets); use 2x (HD budget 32 MB) or the mixed pack (HD budget 48 MB).
`INFECTED_HD_BUDGET_MB` (ge_replay: `--hd-budget-mb`) sets the budget (default 64 MB; there is no settings key for it yet); a texture that does not fit keeps its original.

## What the packs hold, and why

Every image gets the engine that suits its class (`classify.py`, `upscale.py` `ENGINE_BY_CLASS`), judged per class by looking at it next to the original
filtered bilinearly (labelled sheets from `sheet.py`, in-game frames from `compare.py`):

| Class | Images | Engine | Verdict against the filtered original |
|---|---|---|---|
| world: brick, asphalt, facades, signs, vehicles | 750 | 4xHDCube4Compact (a network trained on GameCube/Wii dumps: removes the dither, sharpens edges and lettering) | better |
| characters (recoloured at run time) | 280 | RealESRGAN_x4plus (hair: bicubic, the network invents strands) | modestly better |
| fx: smoke, blood, flash, muzzle | 128 | bicubic (striped sprites: pixel replication) | neutral to better |
| UI: menus, HUD, fonts, reticules, map | 1,339 | HDCube for 64 px and up, Scale2x (pixel exact) up to 32 px | better (text stays crisp and aligned); neutral for tiny sprites and big smooth backgrounds |

After a network, `tone_match` restores the colour and brightness the network shifts (a low-frequency back projection). Alpha is upscaled on its own and
the colour under transparent texels is bled outwards so no halos appear. Mip chains are built from premultiplied colour; coverage scaling is used only for
cut-outs of the world. 2x is the area-downscale of the 4x result.

Recolouring: the game rewrites about 32 palette entries of the characters' textures at run time, so those textures have palettes no disc stream contains. For
them (and the satellite map) the pack holds a recolour record keyed by the palette-free image key; the renderer rebuilds the picture for the palette in use.
Such a texture is RGBA8 on the GPU (about 0.7 MB at 2x, 2.8 MB at 4x for a 256x128 original), not DXT.

## Tools

| Tool | Use |
|---|---|
| `build_packs.sh` | the one command above |
| `extract.py`, `rwstream.py` | level streams to keyed textures (`catalog.json`, `orig/*.png`) |
| `census.py`, `corpus_report.py` | counts per class, format and stream |
| `upscale.py` (`plan`, `run`, `status`, `clean`), `upscale_lib.py`, `models.py` | the engines; resumable cache `up/x2`, `up/x4` |
| `pack.py` | the packs (`--scales 2,4`, `--mixed world=4,fx=4,character=2,ui=2`) |
| `pack_check.py` | every texture a run drew (INFECTED_TEX_DUMP) must hit the pack, directly or as a recolour |
| `sheet.py`, `quality_scan.py`, `compare.py`, `frame_crops.py` | labelled sheets, objective scan, replayed frames off/2x/4x, zoomed crops |
| `level_report.py` | GPU bytes and coverage per level stream |
| `iso_swap.py`, `coverage_run.py` | see any level through the mission-1 slot of an ISO copy, harvest its textures, replay frames, check the pack |

Run-time harvest (optional): `INFECTED_TEX_DUMP=dir` in a play session (or `ge_replay --tex-dump dir`) writes one json per texture drawn. Passing it as
`TEXPACK_RT=dir` lets the classifier use how a texture was actually drawn (2D or 3D); without it 14 of 2,497 images are classed from their names and may differ
(fonts drawn through the 3D pipeline are handled by name either way).

## Provenance and licences

| Component | Used for | Source | Licence |
|---|---|---|---|
| RealESRGAN_x4plus.pth (sha256 4fa0d389...82f1) | characters | https://github.com/xinntao/Real-ESRGAN (release v0.1.0 asset), Wang et al., "Real-ESRGAN: Training Real-World Blind Super-Resolution with Pure Synthetic Data" | BSD-3-Clause |
| 4xHDCube4Compact.pth (sha256 f841188a...9e58) | world, large UI | https://github.com/Venomalia/hdcube (`v4Compact/`), Venomalia, 2023 | "HDcube Model License": no claim on the upscaled output; the model itself may not be sold or redistributed, so it is fetched by each user and never shipped |
| realesr-general-x4v3, realesr-animevideov3 | evaluation only (`EVAL=1 fetch_models.sh`), not in the packs | Real-ESRGAN release v0.2.5.0 | BSD-3-Clause |
| rgbcx (bc7enc_rdo, commit b9438627) | DXT1/DXT5 encoding | https://github.com/richgel999/bc7enc_rdo | MIT or public domain, at your choice |
| Scale2x | tiny UI sprites | the algorithm of Andrea Mazzoleni (https://www.scale2x.it/); implemented from its description in `upscale_lib.py` | the algorithm is free to use |
| PyTorch, NumPy, OpenCV (headless), Pillow, texture2ddecoder | tooling | PyPI | BSD / Apache-2.0 / MIT-CMU / MIT |
| xxHash (XXH3) | the texture key | PortableKit `third_party` | BSD-2-Clause |

The output of the networks is derived from the owner's disc. HDCube states that its author claims no copyright on the output; Real-ESRGAN is published under BSD-3-Clause
(its repository carries no restriction on the output). I am not a lawyer: check the licence files in the repositories above. Packs are for the owner's own copy of the game: do not publish them.
