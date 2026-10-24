#!/bin/bash
# Compile the generated lean corpus into a static library.
#
#   build_lib.sh vita|x86 [GEN_DIR] [OUT_DIR] [JOBS]
#
#   GEN_DIR  directory written by `lean_gen.py --out` (default: $LEAN_GEN or ./gen)
#   OUT_DIR  where objects and the archive go (default: ./build_<target>)
#   JOBS     parallel compiles (default 1; the TUs are 300 KB of C++ each, keep this small on a shared machine)
#
# Environment:
#   PK_INCLUDE   PortableKit include dir (psprecomp/allegrex_context.hpp); default: $LEAN_PK/include, ../third_party/PortableKit/include or ../../PortableKit/include
#   VITASDK      Vita SDK root (required for the vita target)
#   LEAN_EXTRA   extra compiler flags, e.g. -DLEAN_NO_HOOKS or -DLEAN_CHECK_VFPU_ENTRY
#   LEAN_OPT     optimisation level (default -O2)
#   LEAN_LTO=1   vita: link-time optimisation objects (GCC GIMPLE, archived with gcc-ar as liblean_vita_lto.a). The application
#                then links with -DINFECTED_LEAN_LTO=ON, which inlines across the 73 units at link time (the generated functions
#                are small and call across units all the time). Objects go to OUT/lto.
#
# The flags are the contract: -ffp-contract=off (no fused multiply-add; the guest has none), -fno-math-errno, no exceptions/RTTI.
# vita: Cortex-A9 + NEON + hard float, Thumb-2.  x86: the scalar fallback of the NEON forms (no LEAN_NEON_SHIM).
set -e
TARGET=${1:?usage: build_lib.sh vita|x86 [GEN_DIR] [OUT_DIR] [JOBS]}
HERE=$(cd "$(dirname "$0")" && pwd)
GEN=$(cd "${2:-${LEAN_GEN:-gen}}" && pwd)
OUT=${3:-build_$TARGET}
JOBS=${4:-1}
PK=${PK_INCLUDE:-${LEAN_PK:+$LEAN_PK/include}}
PK=${PK:-$HERE/../third_party/PortableKit/include}
[ -d "$PK" ] || PK=$HERE/../../PortableKit/include
OPT=${LEAN_OPT:--O2}
COMMON="-std=c++20 $OPT -fno-exceptions -fno-rtti -fno-math-errno -ffp-contract=off -I$PK -I$HERE -I$GEN ${LEAN_EXTRA:-}"
case $TARGET in
  vita)
    SDK=${VITASDK:?set VITASDK to the root of the Vita SDK}
    CXX=$SDK/bin/arm-vita-eabi-g++
    AR=$SDK/bin/arm-vita-eabi-ar
    FLAGS="$COMMON -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb"
    if [ "${LEAN_LTO:-0}" = 1 ]; then
      AR=$SDK/bin/arm-vita-eabi-gcc-ar
      FLAGS="$FLAGS -flto"
      SUFFIX=_lto
    fi
    ;;
  x86)
    CXX=${CXX:-g++}
    AR=${AR:-ar}
    FLAGS="$COMMON"
    ;;
  *) echo "unknown target $TARGET" >&2; exit 2 ;;
esac
[ -n "${SUFFIX:-}" ] && OUT=$OUT/lto
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
for f in "$GEN"/lean_[0-9]*.cpp "$GEN"/lean_table.cpp; do
  o=$OUT/$(basename "$f" .cpp).o
  if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ "$HERE/lean_abi.h" -nt "$o" ] || [ "$HERE/lean_vfpu.h" -nt "$o" ] || [ "$HERE/lean_vfpu_fast.h" -nt "$o" ] || [ "$GEN/lean_decls.h" -nt "$o" ]; then
    echo "$f"
  fi
done | nice -n 10 xargs -P "$JOBS" -I{} sh -c 'f={}; o='"$OUT"'/$(basename $f .cpp).o; s=$(date +%s); '"$CXX $FLAGS"' -c $f -o $o || exit 255; echo "$(basename $f) $(( $(date +%s) - s ))s" >> '"$OUT"'/compile_times.txt'
rm -f "$OUT/liblean_$TARGET${SUFFIX:-}.a"
"$AR" rcs "$OUT/liblean_$TARGET${SUFFIX:-}.a" "$OUT"/lean_[0-9]*.o "$OUT"/lean_table.o
echo "built $OUT/liblean_$TARGET${SUFFIX:-}.a"
