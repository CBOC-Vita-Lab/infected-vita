#!/bin/bash
# usage: density_bench.sh LABEL "VAR=val ..." [N=600] [FROM=8000]  : one bench window of the stab.script flow (mission 1 played by the pad script)
# prints the [bench] lines and the cell/pool probe lines at the window start
S=/work/ws/tune
L=$1; V=$2; N=${3:-600}; F=${4:-8000}
PAD=${PAD:-$S/tools/stab.script} TIMEOUT=${TIMEOUT:-900} $S/tools/bench.sh $N $F $L INFECTED_LIBC_HOOKS=1 INFECTED_TUNE_CELLS=300 INFECTED_TUNE_POOLS=300 $V > $S/private/logs/$L.sh.out 2>&1
