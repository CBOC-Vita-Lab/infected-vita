# Building From Your Disc

Use Linux x86-64 for the documented workflow. Windows and macOS builds are
not verified. The recovered recipes are included, but the final clean VPK
build has not been completed in this repository. Do not mistake the checks
below for a fresh-install or gameplay test.

## Requirements

- Your **Infected USA, ULUS-10054, version 1.02** ISO.
- VitaSDK with GCC 15.2.0 for the recorded build.
- `git`, CMake 3.20+, `make`, `curl`, `xz`, `patch`, `tar`, a C++20 host compiler,
  Python 3.10+ and Pillow. `pycdlib` is only needed for optional disc artwork.
- Allow about 10 GB of working space and at least 4 GB of memory.

Run all commands from the repository root. `private/`, `dist/`, dependency
checkouts and generated game code must stay out of Git.
Use a checkout/build path without spaces; the recovered compiler script
does not consistently quote them.

## 1. Prepare Libraries

```sh
export VITASDK="$HOME/vitasdk"
export PATH="$VITASDK/bin:$PATH"
scripts/get-portablekit.sh
vita/scripts/fetch-third-party.sh
vita/scripts/build-sysroot.sh
```

PortableKit is pinned to `33800c4060c53f3233e08f8ab2cc4590a0872063`.
The 64 patches must produce tree `ec7dc6d10df8aca87b77d3d6ee145808c40c2ecb`.
That fetch/apply/tree check passed on 2026-10-18. A different tree is a
failure, not a reason to remove the check. Use a new empty destination if
retrying; do not delete an existing development checkout.

The remaining pins and checksums are in `vita/scripts/fetch-third-party.sh`.
Libraries build into `vita/sysroot/`, not into your SDK. Those builds and
the final game link have not been rerun during this import.

## 2. Generate Your Game Code

```sh
mkdir -p private
python3 tools/iso_get.py /path/to/Infected.iso PSP_GAME/SYSDIR/BOOT.BIN private/BOOT.BIN
sha256sum private/BOOT.BIN
# Expected: 952d4bb2c08209f4cbc0020d95c4cc601e9944cad5c7ade4c1d060fe8daab1bc

PK=third_party/PortableKit
g++ -std=c++20 -O1 -I"$PK/include" tools/dump_image.cpp \
  "$PK/src/elf32.cpp" "$PK/src/guest_memory.cpp" -o private/dump_image
private/dump_image private/BOOT.BIN private/image.bin
sha256sum private/image.bin
# Expected: 332e061aa4b00cb1ebd82f3a40247a31e28d177dbe452dbe8a2dc4b74ec3c564

export LEAN_IMAGE="$PWD/private/image.bin"
export LEAN_BOOT="$PWD/private/BOOT.BIN"
python3 recomp/lean_gen.py --profile recomp/infected_profile.json --out private/gen
recomp/build_lib.sh vita private/gen private/vita 2
```

Stop if either hash differs. Renaming another revision does not make it
compatible. The translated sources and library contain game code; do not
upload them. Historical clean-copy checks covered extraction and repeatable
generation, but did not finish the final clean VPK comparison.

## 3. Build And Install

```sh
vita/app/build.sh --lean-lib "$PWD/private/vita/liblean_vita.a"
```

The output is `dist/infected.vpk`, title ID `INFC00001`.
Follow [Install](../INSTALL.md), keeping the same ISO used above.
Keep the resulting VPK private: it contains the translated game code.

`vita/app/build.sh --help` lists options. Defaults generate simple bubble
art. `--art DIR` uses your own pictures; `--shader-cache DIR` adds a compatible
cache. Neither is required. Do not change memory, renderer or clock flags
to match a screenshot from an older build.

## Optional Media

See [HD Textures](../HD-TEXTURES.md) and [HD Movies](HD_MOVIES.md).
The tools are included; weights, texture packs and movies are not. Model
permission does not grant rights to redistribute game-derived output.

## Problems

| Problem | Check |
| --- | --- |
| SDK or library missing | `VITASDK`, `PATH`, and the three library steps above |
| PortableKit tree mismatch | Stop and inspect the patch failure; do not skip verification |
| Generator rejects input | Both hashes and the supported disc revision |
| `LEAN_LIB` missing | Pass the full path to the library generated in step 2 |
| Missing Python package | Install it in a virtual environment, not the system Python |

Source-only checks do not prove the install works. The remaining clean-build,
device and rights checks are listed in [Known Issues](../KNOWN-ISSUES.md).
