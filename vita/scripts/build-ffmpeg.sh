#!/usr/bin/env bash
# Minimal FFmpeg 7.1.5 for the Vita: ATRAC3, ATRAC3plus, H.264 decoders only. Static libavcodec + libavutil.
#   usage: build-ffmpeg.sh [nopt|pt]     nopt = --disable-pthreads (default), pt = --enable-pthreads (vitasdk pthread-embedded)
# Output: $VITA_SYSROOT/ffmpeg-<variant>/ffmpeg-vita/{lib,include}
# Source: https://ffmpeg.org/releases/ffmpeg-7.1.5.tar.xz sha256 de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f
#         (same release + same --disable-everything approach as PortableKit cmake/FFmpeg.cmake)
# IMPORTANT ABI NOTE: the vitasdk toolchain defaults to -fshort-enums (sizeof(enum)==1 for small enums). FFmpeg assumes int-sized
# enums (and libavutil/opt.c fails to compile with GCC 15 otherwise). Everything that includes libavcodec headers MUST also be
# compiled with -fno-short-enums.
set -euo pipefail
source "$(dirname "$0")/env.sh"
variant=${1:-nopt}
src=$VITA_WS/third_party/ffmpeg-src/ffmpeg-7.1.5
b=$VITA_WS/build/ffmpeg-$variant
out=$VITA_SYSROOT/ffmpeg-$variant
case "$variant" in
  nopt) TH="--disable-pthreads --disable-w32threads --disable-os2threads";;
  pt)   TH="--enable-pthreads";;
  *) echo "variant nopt|pt"; exit 2;;
esac
# Local patch (idempotent): link only the float av_tx codelets. Without it tx_double.o + tx_int32.o add ~12.6 MiB of BSS.
if ! grep -q "Infected Vita port" "$src/libavutil/tx.c"; then
  (cd "$src" && patch -p1 < "$VITA_WS/scripts/patches/ffmpeg-tx-float-only.patch")
fi
rm -rf "$b" "$out"; mkdir -p "$b" "$out"; cd "$b"
nice -n 10 "$src/configure" --prefix=/ffmpeg-vita \
  --enable-cross-compile --cross-prefix=arm-vita-eabi- --arch=arm --cpu=cortex-a9 --target-os=none \
  --enable-static --disable-shared --disable-programs --disable-doc \
  --disable-avdevice --disable-avformat --disable-avfilter --disable-swscale --disable-swresample --disable-postproc \
  --disable-network --disable-autodetect --disable-everything \
  --enable-decoder=atrac3,atrac3p,h264 --enable-parser=h264 \
  --enable-asm --enable-neon $TH \
  --disable-debug --disable-symver --disable-stripping \
  --extra-cflags="-O3 -mfpu=neon -mfloat-abi=hard -mtune=cortex-a9 -fno-short-enums -fno-math-errno -fno-signed-zeros" \
  > configure.out 2>&1
nice -n 10 make -j${FF_JOBS:-2} > make.out 2>&1
nice -n 10 make install DESTDIR="$out" > install.out 2>&1
ls -la "$out"/ffmpeg-vita/lib/*.a
