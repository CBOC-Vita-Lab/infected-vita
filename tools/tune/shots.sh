#!/bin/bash
# usage: shots.sh NAME FLIPS-LIST FRAMES [VAR=val ...]  : real GL renderer run of the stab.script flow (mission 1, or Mission_9A with DISC=private/swap/iso9a.iso) with screenshots (960x544)
# at the given flips (comma list) into private/shots9/NAME/. Extra variables go to the game (INFECTED_PERF_VIEW_DISTANCE=..., INFECTED_PERF_ENEMY_DENSITY=..., INFECTED_TUNE_GOD=1).
S=/work/ws/tune
N=$1; F=$2; M=$3; shift 3
mkdir -p $S/private/shots9/$N
PAD=${PAD:-$S/tools/stab.script} $S/tools/play.sh $N ${TIMEOUT:-3000} INFECTED_MAX_FRAMES=$M INFECTED_SCREENSHOT_DIR=$S/private/shots9/$N INFECTED_SCREENSHOT_FLIPS=$F "$@" > $S/private/logs/$N.sh.out 2>&1
