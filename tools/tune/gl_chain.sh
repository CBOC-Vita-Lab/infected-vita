#!/bin/bash
# waits for the level sweep (16 lines in private/logs/sweep.txt), then renders the comparison screenshots (real GL renderer, stab.script flow, god mode)
S=/work/ws/tune
while [ "$(wc -l < $S/private/logs/sweep.txt 2>/dev/null || echo 0)" -lt 16 ]; do sleep 20; done
cd $S
DISC=$S/private/swap/iso9a.iso tools/shots.sh sv150 5700,6000,6600,7200 7300 INFECTED_PERF_VIEW_DISTANCE=150 INFECTED_TUNE_GOD=1
for v in 100 150 200; do tools/shots.sh sm$v 6000,6600,7200 7300 INFECTED_PERF_VIEW_DISTANCE=$v INFECTED_TUNE_GOD=1; done
for d in 100 150 200; do tools/shots.sh sd$d 5800,6200,6600,7000 7100 INFECTED_PERF_ENEMY_DENSITY=$d INFECTED_TUNE_GOD=1; done
