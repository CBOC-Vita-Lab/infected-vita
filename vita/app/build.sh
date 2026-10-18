#!/usr/bin/env bash
# One script: LiveArea assets -> configure -> build -> infected.vpk -> dist/  (ELF unstripped, README-VITA.md, the VPK; the release documents are in vita/release, make_release.sh assembles them).
#   usage: vita/app/build.sh [--outdir DIR] [--dist DIR] [--vitagl release|debug|fast|store] [--ffmpeg nopt|pt] [--attr2 N]
#                            [--heap-mib N] [--lean-lib FILE] [--portablekit DIR] [--shader-cache DIR] [--art DIR] [--clean] [--target NAME]
# --art DIR: the bubble and RetroFlow pictures to use (the layout of vita/assets/art_from_disc.py and gen_bubble.py: DIR/livearea,
#            DIR/retroflow). Without it the original artwork of gen_bubble.py is drawn (nothing from the game).
# Defaults: build in build/vita and dist in dist/ of this repository (both ignored by git); PortableKit in third_party/PortableKit
#           (scripts/get-portablekit.sh) or next to this repository. Needs VITASDK set and --lean-lib (docs/BUILDING.md).
# The machine is shared: everything runs under nice and -j2.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)             # vita/app
tree=$(cd "$here/.." && pwd)                    # vita
proj=$(cd "$tree/.." && pwd)                    # repository root
source "$here/env.sh"
outdir="$proj/build/vita"; dist="$proj/dist"; vgl=release; ffm=nopt; attr2=12; heap=""; lean=""; pk=""; shcache=""; art=""; clean=0; target=""
while [ $# -gt 0 ]; do
  case "$1" in
    --outdir) outdir=$2; shift 2;;
    --dist) dist=$2; shift 2;;
    --vitagl) vgl=$2; shift 2;;
    --ffmpeg) ffm=$2; shift 2;;
    --attr2) attr2=$2; shift 2;;
    --heap-mib) heap=$2; shift 2;;
    --lean-lib) lean=$2; shift 2;;
    --portablekit) pk=$2; shift 2;;
    --shader-cache) shcache=$2; shift 2;;
    --art) art=$2; shift 2;;
    --target) target=$2; shift 2;;
    --clean) clean=1; shift;;
    -h|--help) sed -n 2,6p "$0"; exit 0;;
    *) echo "unknown argument $1"; exit 2;;
  esac
done
[ $clean = 1 ] && rm -rf "$outdir"
mkdir -p "$outdir" "$dist"
echo "== bubble and RetroFlow art"
if [ -n "$art" ]; then
  [ -f "$art/livearea/sce_sys/icon0.png" ] || { echo "error: $art/livearea/sce_sys/icon0.png not found (see --art in the usage line)"; exit 2; }
  [ -f "$art/retroflow/COVERS/Homebrew/INFC00001.png" ] || echo "note: no RetroFlow cover in $art/retroflow; the package will not carry one"
else
  art="$outdir/art"
  rm -rf "$art"
  python3 -I "$tree/vita/assets/gen_bubble.py" "$art"
fi
echo "== configure (vitaGL=$vgl ffmpeg=$ffm ATTRIBUTE2=$attr2)"
extra=()
[ -n "$heap" ] && extra+=("-DNEWLIB_HEAP_MIB=$heap")
[ -n "$lean" ] && extra+=("-DLEAN_LIB=$lean")
[ -n "$pk" ] && extra+=("-DPORTABLEKIT_DIR=$pk")
[ -n "$shcache" ] && extra+=("-DINFECTED_SHADER_CACHE_DIR=$shcache")
nice -n 10 cmake -S "$here" -B "$outdir" -DVITAGL_VARIANT="$vgl" -DFFMPEG_VARIANT="$ffm" -DVITA_ATTRIBUTE2="$attr2" \
      -DLIVEAREA_DIR="$art/livearea" -DRETROFLOW_DIR="$art/retroflow" -DCMAKE_BUILD_TYPE=Release "${extra[@]}" > "$outdir/configure.log" 2>&1 \
      || { tail -40 "$outdir/configure.log"; exit 1; }
echo "== build"
if [ -n "$target" ]; then
  nice -n 10 cmake --build "$outdir" --target "$target" -j2 > "$outdir/build.log" 2>&1 || { grep -n "error\|Error" "$outdir/build.log" | head -40; exit 1; }
  exit 0
fi
build_or_fail() { nice -n 10 cmake --build "$outdir" --target "$1" -j2 > "$outdir/build.log" 2>&1 || { grep -n "error\|Error" "$outdir/build.log" | head -40; exit 1; }; }
build_or_fail infected
# vita-elf-create needs >= 8 KiB between the end of the code segment and the next (64 KiB aligned) segment: measure, pad, relink.
need=8192
free=$(arm-vita-eabi-readelf -lW "$outdir/infected" | python3 -c "
import sys
loads=[l.split() for l in sys.stdin if l.strip().startswith('LOAD')]
# LOAD offset vaddr paddr filesz memsz flags align: the code segment ends at vaddr+filesz, the data segment starts at its vaddr
print(int(loads[1][2],16) - (int(loads[0][2],16) + int(loads[0][4],16)) if len(loads) > 1 else 1 << 20)")
if [ -n "$free" ] && [ "$free" -lt "$need" ]; then
  pad=$(grep -o "INFECTED_TEXT_PAD:STRING=[0-9]*" "$outdir/CMakeCache.txt" | cut -d= -f2)
  pad=$(( ${pad:-0} + need + 4096 - free ))
  echo "== padding .text by $pad bytes (only $free bytes before the data segment)"
  nice -n 10 cmake -S "$here" -B "$outdir" -DINFECTED_TEXT_PAD="$pad" > "$outdir/configure2.log" 2>&1 || { tail -20 "$outdir/configure2.log"; exit 1; }
  build_or_fail infected
fi
build_or_fail infected.vpk-vpk
echo "== dist"
cp -f "$outdir/infected.vpk" "$dist/infected.vpk"
cp -f "$outdir/infected" "$dist/infected.elf"             # unstripped: addr2line -e dist/infected.elf <pc>
[ -f "$tree/README-VITA.md" ] && cp -f "$tree/README-VITA.md" "$dist/README-VITA.md"
ls -la "$dist"
arm-vita-eabi-size "$outdir/infected" | tail -2
echo "VPK: $dist/infected.vpk"
