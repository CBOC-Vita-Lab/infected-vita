#!/bin/bash
# Build the x86-64 differential-test binary.  Usage: build_tests.sh [OPT=-O1] [JOBS=1] [TRACE-FLAG]
# Environment: LEAN_EXTRA  extra compiler flags for the generated corpus (default: the NEON stand-in and the VFPU entry check)
#              LEAN_OUT    object directory name below $W/private (default x86; x86t with a trace flag)
#              LEAN_GEN    generated corpus directory name below $W/private (default gen)
# Layout (all under the private workspace; nothing here is committed):
#   $W/private/gen    generated corpus (lean_gen.py --out)
#   $W/private/x86    objects, liblean_x86.a, difftest
#   $W/private/pk     PortableKit objects (interpreter oracle)
set -e
W=${LEAN_WS:-/work/ws/lean}
R=$W/PortableKit
REC=$W/proj/recomp
GEN=$W/private/${LEAN_GEN:-gen}
OUT=$W/private/${LEAN_OUT:-x86}
OPT=${1:--O1}
TRACE=${3:-}                      # pass -DLEAN_TRACE as the 3rd argument for the bisection build (objects go to x86t)
[ -n "$TRACE" ] && OUT=$W/private/${LEAN_OUT:-x86t}
EXTRA=${LEAN_EXTRA--DLEAN_NEON_SHIM -DLEAN_CHECK_VFPU_ENTRY}
JOBS=${2:-1}
mkdir -p $OUT
CXXFLAGS="-std=c++20 $OPT -fno-exceptions -fno-rtti -fno-strict-aliasing -fno-math-errno -ffp-contract=off -I$R/include -I$REC -I$REC/tests -I$GEN -DLEAN_X86_TEST $EXTRA $TRACE"
bash $W/private/pk/build_pk.sh
for f in $GEN/lean_[0-9]*.cpp $GEN/lean_table.cpp; do
  o=$OUT/$(basename $f .cpp).o
  if [ ! -f $o ] || [ $f -nt $o ] || [ $REC/lean_abi.h -nt $o ] || [ $REC/lean_vfpu.h -nt $o ] || [ $REC/lean_vfpu_fast.h -nt $o ] || [ $REC/tests/lean_neon_shim.h -nt $o ] || [ $GEN/lean_decls.h -nt $o ]; then
    echo "$f"
  fi
done | nice -n 10 xargs -P $JOBS -I{} sh -c 'f={}; o='$OUT'/$(basename $f .cpp).o; g++ '"$CXXFLAGS"' -c $f -o $o || exit 255'
rm -f $OUT/liblean_x86.a
ar rcs $OUT/liblean_x86.a $OUT/lean_[0-9]*.o $OUT/lean_table.o
# harness (exceptions and RTTI on: PortableKit uses them)
nice -n 10 g++ -std=c++20 -O1 -g -I$R/include -I$REC -I$REC/tests -I$GEN -c $REC/tests/stub_runtime.cpp -o $OUT/stub_runtime.o
nice -n 10 g++ -std=c++20 -O1 -g -I$R/include -I$REC -I$REC/tests -I$GEN $TRACE -c $REC/tests/difftest.cpp -o $OUT/difftest.o
nice -n 10 g++ -o $OUT/difftest $OUT/difftest.o $OUT/stub_runtime.o $OUT/liblean_x86.a $W/private/pk/*.o -lpthread
echo built $OUT/difftest
