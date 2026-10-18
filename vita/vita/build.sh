#!/usr/bin/env bash
# One-shot: LiveArea assets -> cmake configure -> build -> package.   Output: build/<dir>/infected_selftest.vpk
#   usage: vita/build.sh [--vitagl release|debug|fast|store] [--ffmpeg nopt|pt] [--attr2 N] [--outdir DIR] [--clean]
# Prerequisites (one-time, see LOG.md for the exact commands): scripts/build-vitagl.sh <variant>, scripts/build-ffmpeg.sh <variant>.
# The machine is shared: everything runs under nice and -j2.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
source "$here/scripts/env.sh"
vgl=release; ffm=nopt; attr2=12; outdir=""; clean=0
while [ $# -gt 0 ]; do
  case "$1" in
    --vitagl) vgl=$2; shift 2;;
    --ffmpeg) ffm=$2; shift 2;;
    --attr2)  attr2=$2; shift 2;;
    --outdir) outdir=$2; shift 2;;
    --clean)  clean=1; shift;;
    -h|--help) sed -n 2,6p "$0"; exit 0;;
    *) echo "unknown argument $1"; exit 2;;
  esac
done
[ -n "$outdir" ] || outdir="$here/build/selftest-$vgl"
[ $clean = 1 ] && rm -rf "$outdir"
mkdir -p "$outdir"
echo "== LiveArea assets"
python3 -I "$here/vita/assets/gen_livearea.py" "$outdir/livearea" "Infected Port Self-Test" "INFT00001"
echo "== configure (vitaGL=$vgl ffmpeg=$ffm ATTRIBUTE2=$attr2)"
nice -n 10 cmake -S "$here" -B "$outdir" -DVITAGL_VARIANT="$vgl" -DFFMPEG_VARIANT="$ffm" -DVITA_ATTRIBUTE2="$attr2" \
      -DLIVEAREA_DIR="$outdir/livearea" -DCMAKE_BUILD_TYPE=Release > "$outdir/configure.log" 2>&1 || { tail -30 "$outdir/configure.log"; exit 1; }
echo "== build"
nice -n 10 cmake --build "$outdir" -j2 > "$outdir/build.log" 2>&1 || { tail -60 "$outdir/build.log"; exit 1; }
vpk="$outdir/infected_selftest.vpk"
ls -la "$vpk" "$outdir/infected_selftest" "$outdir/infected_selftest.self" 2>/dev/null
arm-vita-eabi-size "$outdir/infected_selftest" | tail -2
echo "== param.sfo"
(cd "$outdir" && unzip -p "$vpk" sce_sys/param.sfo | strings | head -20) || true
echo "VPK: $vpk"
