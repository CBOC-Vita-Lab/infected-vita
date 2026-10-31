#!/usr/bin/env bash
# Fetches the third-party sources the Vita libraries are built from into vita/third_party/ (commits and checksums as recorded in
# vita/LOG.md; nothing from there is committed). Next: vita/scripts/build-sysroot.sh.
#   usage: vita/scripts/fetch-third-party.sh [--with-reference]     --with-reference also clones taiHEN, Vita3K and the vitasdk samples (read-only reading
#                                                                    material for developers; the build does not need them)
set -euo pipefail
ref=0; [ "${1:-}" = "--with-reference" ] && ref=1
here=$(cd "$(dirname "$0")/.." && pwd)
tp=$here/third_party
mkdir -p "$tp/dl" "$tp/vdpm-pkgs"
cd "$tp"

clone_at() {   # url dir commit
  [ -d "$2/.git" ] || git clone -q "$1" "$2"
  git -C "$2" fetch -q origin
  git -C "$2" checkout -q "$3"
  echo "$2 $(git -C "$2" rev-parse HEAD)"
}
clone_at https://github.com/Rinnegatamante/vitaGL       vitaGL        9bf3e3f4e55efd1f4a191b179144f7f9a98cd925
# Our vitaGL patches (scripts/patches/vitagl-*.patch, in order), applied once on top of that commit.
for patch in "$here"/scripts/patches/vitagl-*.patch; do
  git -C vitaGL apply --check "$patch" 2>/dev/null && git -C vitaGL apply "$patch" && echo "vitaGL: applied $(basename "$patch")"
done
clone_at https://github.com/Rinnegatamante/vitaShaRK    vitaShaRK     df24065e65098b2d1ac533760109ad4367573f28
clone_at https://github.com/Rinnegatamante/SceShaccCgExt SceShaccCgExt 102192366c25fe7b8fec073d665494b6172605c3
clone_at https://github.com/Rinnegatamante/math-neon    math-neon     0faab814782c071ff4015527f1ca955ab1ccc470
if [ $ref = 1 ]; then
  clone_at https://github.com/yifanlu/taiHEN              taiHEN        309b3800bcb8ebbd5e4f5e5e920af3da3590b829
  clone_at https://github.com/Vita3K/Vita3K               Vita3K        b75d5e6292d5caa6a8411bacc9db1eca1531beab    # large
  clone_at https://github.com/vitasdk/samples             vitasdk-samples fe8fbef570f3280586c0c20157146e3faefb2181
fi

fetch_sum() {  # url file sha256
  [ -f "$2" ] || curl -sSL -o "$2" "$1"
  echo "$3  $2" | sha256sum -c -
}
fetch_sum https://ffmpeg.org/releases/ffmpeg-7.1.5.tar.xz dl/ffmpeg-7.1.5.tar.xz de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f
fetch_sum https://raw.githubusercontent.com/dhepper/font8x8/master/font8x8_basic.h dl/font8x8_basic.h 49d8df366296b203ca3211bc0672cf2a762135bf12710735b6292756b19dffd5
fetch_sum https://github.com/vitasdk/vitasdk-autobuild/releases/download/packages-2026.08-snapshot-20260925.50.1/taihen-0.11-1-vita.pkg.tar.xz \
          vdpm-pkgs/taihen-0.11-1-vita.pkg.tar.xz 93d2075b6b61a164224a2aad651d3d6787f31f3a19c3e2bada7c99844f9fae90

mkdir -p ffmpeg-src vdpm-pkgs/x-taihen
[ -d ffmpeg-src/ffmpeg-7.1.5 ] || tar -xJf dl/ffmpeg-7.1.5.tar.xz -C ffmpeg-src
[ -d vdpm-pkgs/x-taihen/arm-vita-eabi ] || tar -xJf vdpm-pkgs/taihen-0.11-1-vita.pkg.tar.xz -C vdpm-pkgs/x-taihen
echo "done; next: vita/scripts/build-sysroot.sh"
