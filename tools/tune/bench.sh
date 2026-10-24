#!/bin/bash
# usage: bench.sh [N=600] [FROM=3700] [NAME=bench] [VAR=val ...]
# N gameplay flips from flip FROM, from the fixed save (private/ms0.pristine) and the fixed flip-keyed pad script (PAD, default
# tools/continue_flips.script), on lean code only, virtual clock, audio output and GL draw calls nulled (INFECTED_BENCH applies those
# defaults). Prints [bench] lines.  BIN overrides the executable.
N=${1:-600}; FROM=${2:-3700}; NAME=${3:-bench}; shift 3 2>/dev/null
S=/work/ws/tune
BIN=${BIN:-$S/build/bin/InfectedNative}
PADF=${PAD:-$S/tools/continue_flips.script}
$S/tools/run.sh $BIN ${TIMEOUT:-1200} $NAME INFECTED_LEAN_ONLY=1 INFECTED_BENCH=flips:$N,from:$FROM "INFECTED_PAD_SCRIPT=@$PADF" "$@"
grep -E "^\[bench\]|^Bench:|Runtime stopped" $S/private/logs/$NAME.out
