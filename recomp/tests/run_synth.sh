#!/bin/bash
# Focused tests: synthetic guest functions (tests/synth/cases_*.py) through the generator and the differential harness.
#   core    delay slots, branch-likely, calls, tail calls, jump tables, lwl/lwr, ALU, HI/LO, FPU
#   vfpu    every VFPU form with random prefixes, specialised (fast) and generic lowering, plus prefix-tracking programs
# Usage: run_synth.sh [TRIALS=60] [VFPU_SEEDS="1 2 3"]
# Output: private/synth_*/  (generated corpora and harness binaries); the result lines go to stdout.
set -e
W=${LEAN_WS:-/home/claude/ws/lean}
REC=$W/proj/recomp
TRIALS=${1:-60}
SEEDS=${2:-"1 2 3"}
run() {   # name, extra generator flags, difftest flags
  local name=$1 gflags=$2 dflags=$3
  LEAN_GEN=$name LEAN_OUT=${name}_x86 bash $REC/tests/build_tests.sh -O1 1 > $W/private/$name.build.log 2>&1 || { cat $W/private/$name.build.log; exit 1; }
  echo "== $name  ($dflags)"
  LEAN_IMAGE=$W/private/$name/synth.bin nice -n 10 timeout -s KILL 1800 $W/private/${name}_x86/difftest --trials $TRIALS --tries 4 $dflags --out $W/private/$name.mismatches.txt 2>&1 \
    | grep -E "^functions=|^compared=|NaN-vs-NaN|self-modifying|mismatch class|^  bad |junk" || true
}
gen() {   # name, case file, flags
  nice -n 10 python3 $REC/lean_gen.py --synth $REC/tests/synth/$2 --out $W/private/$1 $3 2>&1 | grep -E "^functions:|^stats:" | tr '\n' ' '; echo
}
gen synth_core cases_core.py ""
run synth_core "" ""
for s in $SEEDS; do
  export LEAN_SYNTH_SEED=$s
  gen synth_vfpu cases_vfpu.py ""
  run synth_vfpu "" "--special --seed $s"
  gen synth_vfpu_g cases_vfpu.py "--no-vfpu-fast"
  run synth_vfpu_g "" "--special --seed $s"
done
