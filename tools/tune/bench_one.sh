#!/bin/bash
# usage: bench_one.sh NAME DENSITY VIEW [mission9a]  : a 600 flip bench window (flips 8000..8600 of the stab.script flow), god mode on, with the GE statistics
# of the window written to private/stats/NAME/ge_win.txt (draws, vertices, primitives per flip). Mission_9A through the swapped ISO when the 4th argument is given.
S=/home/claude/ws/tune
N=$1; D=$2; V=$3
[ "$4" = mission9a ] && export DISC=$S/private/swap/iso9a.iso
mkdir -p $S/private/stats/$N
PAD=$S/tools/stab.script TIMEOUT=900 $S/tools/bench.sh 600 8000 $N INFECTED_LIBC_HOOKS=1 INFECTED_TUNE_GOD=1 INFECTED_TUNE_CELLS=300 INFECTED_TUNE_POOLS=300 \
  INFECTED_PERF_ENEMY_DENSITY=$D INFECTED_PERF_VIEW_DISTANCE=$V INFECTED_STATS=$S/private/stats/$N INFECTED_STATS_PHASES=pre@8000,win@8600 > $S/private/logs/$N.sh.out 2>&1
