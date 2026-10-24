#!/bin/bash
# usage: sweep.sh FRAMES NAME=params [NAME=params ...]   (each run: lean, virtual clock, bot with the given INFECTED_BOT_PARAMS)
# prints one summary line per run: deaths, completion events.
W=${BOT_WS:-/work/ws/bot}
F=$1; shift
for spec in "$@"; do
  n=${spec%%=*}; p=${spec#*=}
  $W/proj/tools/bot/run.sh sw_$n 900 INFECTED_MAX_FRAMES=$F INFECTED_BOT=1 INFECTED_BOT_PARAMS=$p > /dev/null
  echo "== $n [$p]"
  grep -E "event (mission_complete|player_dead)" $W/private/logs/sw_$n.err | cut -c1-160
done
