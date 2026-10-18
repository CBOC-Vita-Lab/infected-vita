#!/bin/bash
# Fetches rgbcx (bc7enc_rdo) and builds libbcenc.so next to this script's output dir.
#   build.sh OUT_DIR           (default: ./build)
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/build}
mkdir -p "$out"
if [ ! -d "$out/bc7enc_rdo" ]; then
  GIT_LFS_SKIP_SMUDGE=1 git clone --depth 1 https://github.com/richgel999/bc7enc_rdo "$out/bc7enc_rdo"
fi
${CXX:-c++} -O3 -std=c++17 -fPIC -shared -I"$out/bc7enc_rdo" -o "$out/libbcenc.so" "$here/bcenc.cpp" "$out/bc7enc_rdo/rgbcx.cpp"
echo "built $out/libbcenc.so"
