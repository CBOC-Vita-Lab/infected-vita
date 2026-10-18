#!/bin/bash
# after gl_chain.sh: the view distance pictures of two more levels (Mission_14B and Mission_4A through private/swap/isov.iso)
S=/home/claude/ws/tune
IS=/home/claude/ws/tex/proj/tools/texpack/iso_swap.py
SW=$S/private/swap/isov.iso
while [ ! -f $S/private/shots9/sd200/flip_007000.png ]; do sleep 20; done
sleep 30
cd $S
for X in 14B 4A; do
  python3 $IS $SW --restore > /dev/null 2>&1
  python3 $IS $SW --redirect PSP_GAME/USRDIR/IMAGE/levels/Mission_1A.stream.zpm PSP_GAME/USRDIR/IMAGE/levels/Mission_$X.stream.zpm > /dev/null
  for v in 100 200; do DISC=$SW tools/shots.sh s${X}_$v 5900,6100 6150 INFECTED_PERF_VIEW_DISTANCE=$v INFECTED_TUNE_GOD=1; done
done
