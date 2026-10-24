#!/bin/bash
# after gl_chain2.sh: the enemy density pictures of Mission_9A (private/swap/iso9a.iso)
S=/work/ws/tune
while [ ! -f $S/private/shots9/s4A_200/flip_006100.png ]; do sleep 20; done
sleep 20
cd $S
for d in 100 150 200; do DISC=$S/private/swap/iso9a.iso tools/shots.sh sn$d 6000,6600,7200,8000 8100 INFECTED_PERF_ENEMY_DENSITY=$d INFECTED_TUNE_GOD=1; done
