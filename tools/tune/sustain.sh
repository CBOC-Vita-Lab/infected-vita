#!/bin/bash
# usage: sustain.sh NAME DENSITY [EXTRA VAR=val ...]  : the stab.script flow (mission 1 from the menus), null draw, 30000 flips, probes every 600 flips,
# heap top size dumped (dlmalloc state at 0x08ADD25C: +8 dvsize, +0xC topsize)
S=/home/claude/ws/tune
N=$1; D=$2; shift 2
PAD=${PAD:-$S/tools/stab.script} $S/tools/play.sh $N 2400 INFECTED_MAX_FRAMES=${FRAMES:-30000} INFECTED_GL_NULL_DRAW=1 INFECTED_PERF_ENEMY_DENSITY=$D \
  INFECTED_TUNE_CELLS=600 INFECTED_TUNE_POOLS=600 INFECTED_TUNE_LOG=1 \
  INFECTED_TUNE_DUMP="6000:08ADD25C:6,12000:08ADD25C:6,18000:08ADD25C:6,24000:08ADD25C:6,29900:08ADD25C:6" "$@" > $S/private/logs/$N.sh.out 2>&1
