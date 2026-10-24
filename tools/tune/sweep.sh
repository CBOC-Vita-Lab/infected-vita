#!/bin/bash
# usage: sweep.sh LEVEL..  : for each level (4A 14B ...) run Mission_1A's slot as that level (private/swap/isov.iso, iso_swap.py) and bench 120 flips from flip 6000 of the stab.script flow at
# view distance 100 and 200 (null draw, god mode); prints draws / vertices / lean entries per flip. Output private/logs/sweep.txt
S=/work/ws/tune
SW=$S/private/swap/isov.iso
IS=/work/ws/tex/proj/tools/texpack/iso_swap.py
L=/work/ws/tune/private/logs/sweep.txt
for X in "$@"; do
  python3 $IS $SW --restore > /dev/null 2>&1
  python3 $IS $SW --redirect PSP_GAME/USRDIR/IMAGE/levels/Mission_1A.stream.zpm PSP_GAME/USRDIR/IMAGE/levels/Mission_$X.stream.zpm > /dev/null
  for V in 100 200; do
    N=sw${X}_$V
    mkdir -p $S/private/stats/$N
    DISC=$SW PAD=$S/tools/stab.script TIMEOUT=900 $S/tools/bench.sh 120 6000 $N INFECTED_LIBC_HOOKS=1 INFECTED_TUNE_GOD=1 INFECTED_PERF_VIEW_DISTANCE=$V \
      INFECTED_STATS=$S/private/stats/$N INFECTED_STATS_PHASES=pre@6000,win@6120 > $S/private/logs/$N.sh.out 2>&1
    echo "$X view $V: $(sed -n 3p $S/private/stats/$N/ge_win.txt | cut -c1-130) | $(grep -E 'per flip: pk_hle' $S/private/logs/$N.sh.out | sed -E 's/.*lean ([0-9.]+),.*/lean \1/')" >> $L
  done
done
