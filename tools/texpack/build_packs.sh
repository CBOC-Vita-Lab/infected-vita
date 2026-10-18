#!/bin/bash
# Infected.iso -> texpack_2x.bin, texpack_4x.bin and texpack_4x_mixed.bin, in one command (resumable: run it again after an interruption).
#
#     tools/texpack/build_packs.sh /path/to/Infected.iso [OUT_DIR]
#
# Everything derived from the disc (extracted textures, upscaled pictures, the packs) is written under $TEXPACK_WORK (default ./texpack-work)
# and never belongs in a repository or a bug report. OUT_DIR defaults to $TEXPACK_WORK/packs.
#
# Environment (all optional):
#   TEXPACK_WORK     work directory (default ./texpack-work)
#   PORTABLEKIT      the PortableKit checkout (default: third_party/PortableKit of this repository, made by scripts/get-portablekit.sh; it must be that patched copy); only tools/extract_iso.py and tools/texkey are used
#   TEXKEY           a ready texkey binary (skips building it)
#   TEXPACK_DEVICE   cpu | mps | cuda (default: mps when torch reports it, else cpu)
#   TEXPACK_THREADS  CPU threads for the networks and the packer (default: all cores)
#   TEXPACK_LIMIT    only the first N images (a smoke test of the whole chain, minutes instead of hours)
#   TEXPACK_MIXED    scale per class of the "mixed" pack (default world=4,fx=4,character=2,ui=2)
#   PYTHON           python3 to create the virtual environment with
set -euo pipefail

ISO=${1:?usage: build_packs.sh /path/to/Infected.iso [OUT_DIR]}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${TEXPACK_WORK:-$PWD/texpack-work}
OUT=${2:-$WORK/packs}
PYTHON=${PYTHON:-python3}
mkdir -p "$WORK" "$OUT"
WORK=$(cd "$WORK" && pwd)
ISO=$(cd "$(dirname "$ISO")" && pwd)/$(basename "$ISO")
[ -f "$ISO" ] || { echo "no such disc image: $ISO" >&2; exit 1; }
if command -v sysctl >/dev/null 2>&1 && sysctl -n hw.ncpu >/dev/null 2>&1; then CORES=$(sysctl -n hw.ncpu); else CORES=$(nproc); fi
THREADS=${TEXPACK_THREADS:-$CORES}
LIMIT=${TEXPACK_LIMIT:-0}
MIXED=${TEXPACK_MIXED:-world=4,fx=4,character=2,ui=2}
step() { echo; echo "==== $* ($(date +%H:%M:%S))"; }

# PortableKit: extract_iso.py and the texkey tool
if [ -z "${PORTABLEKIT:-}" ]; then
  for c in "$HERE/../../third_party/PortableKit" "$HERE/../../../PortableKit" "$PWD/PortableKit"; do
    [ -f "$c/tools/extract_iso.py" ] && PORTABLEKIT=$(cd "$c" && pwd) && break
  done
fi
[ -f "${PORTABLEKIT:-/nonexistent}/tools/extract_iso.py" ] || { echo "PortableKit not found: set PORTABLEKIT=/path/to/PortableKit" >&2; exit 1; }
export PORTABLEKIT

step "1/7 python environment"
if [ ! -x "$WORK/venv/bin/python" ]; then
  "$PYTHON" -m venv "$WORK/venv"
  "$WORK/venv/bin/pip" install --upgrade pip
  # plain `pip install torch` is the right wheel on an Apple-silicon Mac (it includes the MPS backend); Linux without a GPU: add
  # --index-url https://download.pytorch.org/whl/cpu
  "$WORK/venv/bin/pip" install numpy pillow opencv-python-headless texture2ddecoder torch
fi
PY="$WORK/venv/bin/python"
if [ -z "${TEXPACK_DEVICE:-}" ]; then
  TEXPACK_DEVICE=$("$PY" -c "import torch; print('mps' if torch.backends.mps.is_available() else ('cuda' if torch.cuda.is_available() else 'cpu'))")
fi
export TEXPACK_DEVICE PYTORCH_ENABLE_MPS_FALLBACK=1
echo "device: $TEXPACK_DEVICE, threads: $THREADS"

step "2/7 model weights (downloaded from their authors, checked against the sums the shipped packs were made with)"
"$HERE/fetch_models.sh" "$WORK/models"

step "3/7 DXT encoder (rgbcx from bc7enc_rdo, MIT / public domain)"
[ -f "$WORK/bcenc/libbcenc.so" ] || "$HERE/bcenc/build.sh" "$WORK/bcenc"
export BCENC_LIB="$WORK/bcenc/libbcenc.so"

step "4/7 texkey (the renderer's own texture key and decode, as a tool)"
if [ -z "${TEXKEY:-}" ]; then
  TEXKEY="$WORK/build-texkey/bin/texkey"
  if [ ! -x "$TEXKEY" ]; then
    GEN=(); command -v ninja >/dev/null 2>&1 && GEN=(-G Ninja)
    cmake -S "$PORTABLEKIT" -B "$WORK/build-texkey" ${GEN[@]+"${GEN[@]}"} -DCMAKE_BUILD_TYPE=Release -DPORTABLEKIT_TEXKEY=ON -DPORTABLEKIT_BUILD_RUNTIME_LIBRARY=OFF
    cmake --build "$WORK/build-texkey" --target texkey -j "$CORES"
  fi
fi
[ -x "$TEXKEY" ] || { echo "texkey not built: $TEXKEY" >&2; exit 1; }

step "5/7 extract every texture of the disc's level streams"
if [ ! -f "$WORK/extract/catalog.json" ]; then
  "$PY" "$HERE/extract.py" --iso "$ISO" --texkey "$TEXKEY" --out "$WORK/extract"
  rm -rf "$WORK/extract/iso"
fi

step "6/7 upscale (fast pass: every image; best pass: characters with the large network)"
LIM=(); [ "$LIMIT" != 0 ] && LIM=(--limit "$LIMIT")
RT=(); [ -n "${TEXPACK_RT:-}" ] && RT=(--rt "$TEXPACK_RT")   # optional run-time harvest, see README
"$PY" "$HERE/upscale.py" run --extract "$WORK/extract" ${RT[@]+"${RT[@]}"} --models "$WORK/models" --out "$WORK/up" --pass fast --threads "$THREADS" ${LIM[@]+"${LIM[@]}"}
"$PY" "$HERE/upscale.py" run --extract "$WORK/extract" ${RT[@]+"${RT[@]}"} --models "$WORK/models" --out "$WORK/up" --pass best --threads "$THREADS" ${LIM[@]+"${LIM[@]}"}

step "7/7 packs (2x, 4x and the mixed 4x: $MIXED)"
"$PY" "$HERE/pack.py" --extract "$WORK/extract" --up "$WORK/up" --out "$OUT" ${RT[@]+"${RT[@]}"} --scales 2,4 --mixed "$MIXED" --texkey "$TEXKEY" --jobs "$CORES" ${LIM[@]+"${LIM[@]}"}
cat "$OUT"/pack_stats*.json

echo
ls -la "$OUT"/texpack_*.bin
cat <<MSG

Done. Copy the pack you want to the Vita (the game reads texpack_2x.bin for gfx.hd_textures=2x and texpack_4x.bin for 4x):
    ux0:data/infected/texpack_2x.bin
    ux0:data/infected/texpack_4x.bin          (recommended: the mixed pack, saved as texpack_4x.bin:  cp texpack_4x_mixed.bin texpack_4x.bin)
On a desktop build the same files go into the data directory (INFECTED_DATA_DIR).
MSG
