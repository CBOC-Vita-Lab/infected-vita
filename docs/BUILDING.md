# Building the VPK from your own disc

The package that runs on the Vita contains code translated from the game, so it cannot be shared and there is nothing to download.
You build it once, on a PC, from your own copy of **Infected** (US, ULUS-10054, 1.02) as an ISO. Installing the result is
[`INSTALL.md`](../INSTALL.md).

Sections 2 to 4 were run on a Linux x86-64 machine (2 cores, 7 GB of memory) from a clean copy of this repository on 23 Oct 2026, and the
VPK they made has the program, the core-3 module, the shader list and the bubble art in it. Where a step has not
been run on another system, it says so. No step needs root.

```
 your Infected.iso
   │  tools/iso_get.py                         BOOT.BIN        (the game's executable, checked by its SHA-256)
   │  tools/dump_image.cpp                     image.bin       (the same, relocated to where the PSP loads it)
   │  recomp/lean_gen.py                       private/gen/    (73 C++ files: the whole game as C++)
   │  recomp/build_lib.sh vita                 liblean_vita.a  (the game, compiled for the Vita's ARM core)
   ▼
 vita/app/build.sh  + PortableKit + vitaGL + FFmpeg  →  dist/infected.vpk
```

## 1. What you need

| | |
| --- | --- |
| A PC | Linux x86-64 (what this was run on). About 2 GB of free disk and 4 GB of memory. Measured on 2 cores: the libraries 4 minutes, the recompiler 1 minute, the game library 6 minutes, the VPK 10 minutes (about 40 minutes of CPU time in all). |
| Your disc image | `Infected.iso`, the US version, ULUS-10054, 1.02. The generator and the app both check the game's SHA-256 and refuse any other. |
| [VitaSDK](https://vitasdk.org/) | With `arm-vita-eabi-gcc` on its `bin/` and `VITASDK` pointing at it. This was built with GCC 15.2.0 (newlib and pthread-embedded of 26 Sep 2026). Install it with the `vdpm` instructions on vitasdk.org. |
| Tools | `git`, `cmake` (3.16 or newer; 3.28 was used), `make`, `curl`, `xz`, `patch`, `tar`, a host `g++` with C++20, `python3` (3.10 or newer) |
| Python packages | `Pillow` for the artwork scripts, `pycdlib` only for `art_from_disc.py`. The texture packs need more (section 5). |

On Ubuntu or Debian the tools are one line: `sudo apt install git cmake make curl xz-utils patch g++ python3 python3-pip`, then
`python3 -m pip install --user Pillow pycdlib`. **Windows:** use WSL with Ubuntu and run everything inside it (not tried yet, but it is the
same Linux toolchain). **macOS:** see "What is not covered here" at the end.

The commands below run from the root of this repository. `private/` is where everything made from your disc goes; git ignores it, and
nothing in it should ever be committed, uploaded or attached to a report.

## 2. The libraries (no disc needed)

```sh
export VITASDK=$HOME/vitasdk                  # wherever VitaSDK is

scripts/get-portablekit.sh                    # PortableKit + the patches for this port -> third_party/PortableKit
vita/scripts/fetch-third-party.sh             # vitaGL, vitaShaRK, SceShaccCgExt, math-neon, FFmpeg, taiHEN stubs -> vita/third_party
vita/scripts/build-sysroot.sh                 # builds them into vita/sysroot (nothing is installed into the SDK)
```

* `get-portablekit.sh` clones upstream [PortableKit](https://github.com/TeamGDB/PortableKit) at the commit the port was written on and
  applies `patches/portablekit/`. It checks that the result is exactly the tree the patches were made from.
* `fetch-third-party.sh` takes every source at a pinned commit or checksum (`vita/LOG.md` lists them) and applies the four small vitaGL
  patches in `vita/scripts/patches/`.
* `build-sysroot.sh` is safe to run again (it skips what is built). Its flags and the reasons for them are in `vita/LOG.md` and
  `vita/VITAGL_NOTES.md`. Add `--all` for the extra vitaGL variants and the threaded FFmpeg, which only the self-test and measurement
  builds use.

## 3. The game library (your disc)

```sh
mkdir -p private

# BOOT.BIN, straight out of the ISO (standard library only, no 7-Zip needed)
python3 tools/iso_get.py /path/to/Infected.iso PSP_GAME/SYSDIR/BOOT.BIN private/BOOT.BIN
sha256sum private/BOOT.BIN
#   952d4bb2c08209f4cbc0020d95c4cc601e9944cad5c7ade4c1d060fe8daab1bc   (anything else is another version)

# the same executable, relocated to the address the PSP loads it at
PK=third_party/PortableKit
g++ -std=c++20 -O1 -I$PK/include tools/dump_image.cpp $PK/src/elf32.cpp $PK/src/guest_memory.cpp -o private/dump_image
private/dump_image private/BOOT.BIN private/image.bin
sha256sum private/image.bin
#   332e061aa4b00cb1ebd82f3a40247a31e28d177dbe452dbe8a2dc4b74ec3c564

# the recompiler: the game as C++ (about a minute, about 1.2 GB of memory)
export LEAN_IMAGE=$PWD/private/image.bin LEAN_BOOT=$PWD/private/BOOT.BIN
python3 recomp/lean_gen.py --profile recomp/infected_profile.json --out private/gen
#   ... functions: 9577  typed 9561  regfile 16  fallback 0 ... translation units: 73

# compile it for the Vita (about 12 minutes of CPU time, 6 on two cores; the last argument is the number of parallel compiles)
LEAN_LTO=1 recomp/build_lib.sh vita private/gen private/vita 2
#   built private/vita/lto/liblean_vita_lto.a
```

`LEAN_LTO=1` compiles the game for link-time optimisation: the final link then optimises the 73 units as one program, so the many
small functions the recompiler makes are inlined across units (the package build takes about 10 minutes longer). It is how the
released builds are made; leave it out (the library is then `private/vita/liblean_vita.a`) for a quicker build.

The generator is deterministic: the 77 files it writes were byte-identical in two separate runs, one of them from a clean copy of this repository.

## 4. The package

```sh
vita/app/build.sh --lean-lib $PWD/private/vita/lto/liblean_vita_lto.a
```

The result is **`dist/infected.vpk`** (and `dist/infected.elf`, the unstripped program, for `addr2line`). Install it as described in
[`INSTALL.md`](../INSTALL.md). `build.sh --help` lists the options; the ones that matter:

| Option | What it does |
| --- | --- |
| `--art DIR` | Use these bubble and RetroFlow pictures instead of the original artwork that `vita/vita/assets/gen_bubble.py` draws. Make `DIR` with `art_from_disc.py` or `art_from_image.py` (below). |
| `--shader-cache DIR` | Ship precompiled shaders. Optional: without them the game compiles each shader the first time it is drawn. `DIR` is the `shader_cache` folder of an earlier run (`ux0:data/infected/shader_cache`, filled fastest with `prewarm_shaders=1`, see `vita/README-VITA.md`). |
| `--heap-mib N`, `--vitagl`, `--ffmpeg`, `--attr2` | Memory split and library variants. The defaults are the ones the measurements in `docs/STATUS.md` were made with. |
| `--clean` | Start the CMake build directory again. |

### Artwork for the bubble and RetroFlow (optional)

The default is original artwork drawn by a script, which is what any copy of this repository builds. To use pictures of your own:

```sh
# from your disc: its icon and menu picture (needs Pillow and pycdlib; optionally a screenshot for RetroFlow's background)
python3 vita/vita/assets/art_from_disc.py /path/to/Infected.iso private/art [screenshot.png]

# or from any picture you have (fan art, a poster)
python3 vita/vita/assets/art_from_image.py picture.png private/art --cover cover.png

vita/app/build.sh --lean-lib $PWD/private/vita/lto/liblean_vita_lto.a --art private/art
```

Pictures made from the game or from someone's artwork stay in your private copy: the package that contains them is as unshareable as the one
that contains the game code.

## 5. HD textures (optional)

`tools/texpack/build_packs.sh /path/to/Infected.iso` upscales every texture of the game with neural networks and writes
`texpack_2x.bin` and `texpack_4x.bin`. Expect hours on a PC without a GPU. `tools/texpack/README.md` has the steps, the Python packages,
the licences of the networks and how the packs are used. It builds a small tool from the patched PortableKit of section 2, so run that first.

## If a step fails

| You see | It means |
| --- | --- |
| `set VITASDK to the root of the Vita SDK` | `export VITASDK=...` in the shell you are building in. |
| `get-portablekit.sh: the patched tree is ..., expected ...` | Upstream PortableKit changed under the pinned commit, or a patch did not apply. Nothing else works until this does; open an issue with the output. |
| `lean_gen.py` stops on the image or the BOOT.BIN | `LEAN_IMAGE` and `LEAN_BOOT` must point at files made from the same US 1.02 disc. Check the two SHA-256 sums above. |
| `LEAN_LIB is not set` | `build.sh` needs `--lean-lib FILE` (section 4). |
| a missing `libvitaGL.a`, `libavcodec.a` ... in the sysroot | Run `vita/scripts/build-sysroot.sh` (section 2) with the same `VITASDK`. |
| The Vita says the ISO is not the one the build was generated from | The ISO on the Vita is a different version from the one in section 3. |

## What is not covered here

* macOS and Windows: not tried. Everything is shell, Python and CMake, so macOS should work with the tools above; the texture packs were also written for an Apple-silicon Mac but only run on Linux.
* The development scripts in `tools/` (the bot, replay checks, console test drivers) assume the layout of the machine they were written on. `tools/README.md` says which are self-contained.
