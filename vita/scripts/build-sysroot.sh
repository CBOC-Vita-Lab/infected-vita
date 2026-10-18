#!/usr/bin/env bash
# Builds the libraries the Vita application links into vita/sysroot/ (nothing is installed into the Vita SDK):
#   taiHEN stubs, SceShaccCgExt, vitaShaRK, math-neon, vitaGL (release variant), FFmpeg (nopt: the ATRAC3, ATRAC3plus and H.264 decoders only).
# Needs VITASDK (the Vita SDK) and the sources from vita/scripts/fetch-third-party.sh. Safe to run again: a library that is already in
# the sysroot is skipped (delete vita/sysroot to start again). The exact flags are explained in vita/LOG.md and vita/VITAGL_NOTES.md.
#   usage: vita/scripts/build-sysroot.sh [--all]     --all adds the vitaGL variants debug, fast, store and profile and the pthread FFmpeg
#                                                      (only the self-test package and measurement builds use them)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
source "$here/env.sh"
cd "$VITA_WS"
all=0; [ "${1:-}" = "--all" ] && all=1
[ -d third_party/vitaGL ] || { echo "third_party is empty: run vita/scripts/fetch-third-party.sh first" >&2; exit 1; }
mkdir -p "$VITA_SYSROOT/lib" "$VITA_SYSROOT/include" build
have() { [ -f "$VITA_SYSROOT/lib/$1" ]; }

# 1. taiHEN stubs: the prebuilt vdpm package (building the stubs from source fails with this SDK's kernel headers)
if ! have libtaihen_stub.a; then
  echo "== taiHEN stubs"
  mkdir -p third_party/vdpm-pkgs/x-taihen
  tar -xJf third_party/vdpm-pkgs/taihen-0.11-1-vita.pkg.tar.xz -C third_party/vdpm-pkgs/x-taihen
  cp third_party/vdpm-pkgs/x-taihen/arm-vita-eabi/lib/*.a "$VITA_SYSROOT/lib/"
  cp third_party/vdpm-pkgs/x-taihen/arm-vita-eabi/include/taihen.h "$VITA_SYSROOT/include/"
fi

# 2. SceShaccCgExt (-std=gnu17: C23 changes the meaning of "()" prototypes)
if ! have libSceShaccCgExt.a; then
  echo "== SceShaccCgExt"
  rm -rf build/shaccgext && mkdir -p build/shaccgext
  (cd build/shaccgext
   nice -n 10 cmake -S ../../third_party/SceShaccCgExt -B . -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
       -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="-std=gnu17 -I$VITA_SYSROOT/include" -DCMAKE_CXX_FLAGS="-I$VITA_SYSROOT/include" > /dev/null
   nice -n 10 make -j2 > /dev/null
   cp libSceShaccCgExt.a "$VITA_SYSROOT/lib/" && cp ../../third_party/SceShaccCgExt/include/shacccg_ext.h "$VITA_SYSROOT/include/")
fi

# 3. vitaShaRK (includes shacccg_ext.h from step 2)
if ! have libvitashark.a; then
  echo "== vitaShaRK"
  (cd third_party/vitaShaRK
   nice -n 10 make CFLAGS="-g -Wl,-q -O2 -ffast-math -mtune=cortex-a9 -mfpu=neon -ftree-vectorize -std=gnu17 -I$VITA_SYSROOT/include" > /dev/null
   cp libvitashark.a "$VITA_SYSROOT/lib/" && cp source/vitashark.h "$VITA_SYSROOT/include/")
fi

# 4. math-neon
if ! have libmathneon.a; then
  echo "== math-neon"
  (cd third_party/math-neon && nice -n 10 make -j2 > /dev/null)
  cp third_party/math-neon/libmathneon.a "$VITA_SYSROOT/lib/" && cp third_party/math-neon/source/math_neon.h "$VITA_SYSROOT/include/"
fi

# 5. vitaGL (the sources already carry vita/scripts/patches/vitagl-*.patch: fetch-third-party.sh applied them)
variants="release"; [ $all = 1 ] && variants="release debug fast store profile"
for v in $variants; do
  suffix=""; [ "$v" = release ] || suffix="_$v"
  have "libvitaGL$suffix.a" || { echo "== vitaGL $v"; "$here/build-vitagl.sh" "$v"; }
done

# 6. FFmpeg
ffvariants="nopt"; [ $all = 1 ] && ffvariants="nopt pt"
for v in $ffvariants; do
  [ -f "$VITA_SYSROOT/ffmpeg-$v/ffmpeg-vita/lib/libavcodec.a" ] || { echo "== FFmpeg $v"; "$here/build-ffmpeg.sh" "$v" > "build/ffmpeg-$v.log" 2>&1 || { tail -30 "build/ffmpeg-$v.log"; exit 1; }; }
done
echo "sysroot ready: $VITA_SYSROOT"
