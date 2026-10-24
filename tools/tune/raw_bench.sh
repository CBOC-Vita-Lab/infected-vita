#!/bin/bash
# usage: raw_bench.sh NAME RAWFACTOR [mission9a] : the bench_one window with the view factor forced (INFECTED_TUNE_VIEW_RAW, unclamped) to see what a far plane that really bites costs
S=/work/ws/tune
N=$1; R=$2
[ "$3" = mission9a ] && export DISC=$S/private/swap/iso9a.iso
mkdir -p $S/private/stats/$N
PAD=$S/tools/stab.script TIMEOUT=900 $S/tools/bench.sh 600 8000 $N INFECTED_LIBC_HOOKS=1 INFECTED_TUNE_GOD=1 INFECTED_TUNE_VIEW_RAW=$R \
  INFECTED_STATS=$S/private/stats/$N INFECTED_STATS_PHASES=pre@8000,win@8600 > $S/private/logs/$N.sh.out 2>&1
