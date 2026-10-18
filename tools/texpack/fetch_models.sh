#!/bin/bash
# Downloads the upscaler weights into DIR (default private/models) and checks them against the sha256 sums of the files the shipped packs were made with.
# None of them is part of the repository, and the HDCube licence forbids passing the weights on: every user fetches them from the source.
#   RealESRGAN_x4plus.pth     Real-ESRGAN (BSD-3-Clause), release asset  https://github.com/xinntao/Real-ESRGAN/releases/tag/v0.1.0   (characters)
#   4xHDCube4Compact.pth      HDCube by Venomalia, https://github.com/Venomalia/hdcube (v4Compact/), licence in HDcube_LICENSE.txt     (world, large UI)
#   EVAL=1 also fetches realesr-general-x4v3.pth and realesr-animevideov3.pth (Real-ESRGAN v0.2.5.0 releases), used only for the model comparison (eval_models.py)
set -euo pipefail
dir=${1:-private/models}
mkdir -p "$dir"
rel=https://github.com/xinntao/Real-ESRGAN/releases/download
sha() { if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
fetch() {   # url file sha256
  if [ ! -s "$dir/$2" ]; then curl -sSL --fail -o "$dir/$2.part" "$1" && mv "$dir/$2.part" "$dir/$2"; fi
  got=$(sha "$dir/$2")
  if [ "$got" != "$3" ]; then echo "WARNING: $2 has sha256 $got, the packs were made with $3 (a newer or different file: the pack will differ)" >&2; fi
}
fetch $rel/v0.1.0/RealESRGAN_x4plus.pth RealESRGAN_x4plus.pth 4fa0d38905f75ac06eb49a7951b426670021be3018265fd191d2125df9d682f1
if [ ! -s "$dir/4xHDCube4Compact.pth" ]; then
  tmp=$(mktemp -d)
  GIT_LFS_SKIP_SMUDGE=1 git clone -q --depth 1 --filter=blob:none --sparse https://github.com/Venomalia/hdcube "$tmp/hdcube"
  git -C "$tmp/hdcube" sparse-checkout set v4Compact Compact
  cp "$tmp/hdcube/v4Compact/4xHDCube4Compact.pth" "$dir/"
  cp "$tmp/hdcube/LICENSE" "$dir/HDcube_LICENSE.txt"
  rm -rf "$tmp"
fi
got=$(sha "$dir/4xHDCube4Compact.pth")
[ "$got" = f841188a346581b0e851f6306b69d50da0f5ac668498bb46a32c7c1c03589e58 ] || echo "WARNING: 4xHDCube4Compact.pth has sha256 $got, the packs were made with f841188a..." >&2
if [ "${EVAL:-0}" = 1 ]; then
  fetch $rel/v0.2.5.0/realesr-general-x4v3.pth realesr-general-x4v3.pth 8dc7edb9ac80ccdc30c3a5dca6616509367f05fbc184ad95b731f05bece96292
  fetch $rel/v0.2.5.0/realesr-animevideov3.pth realesr-animevideov3.pth b8a8376811077954d82ca3fcf476f1ac3da3e8a68a4f4d71363008000a18b75d
fi
ls -la "$dir"
