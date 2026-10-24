#!/bin/bash
# real-GL replay of the recorded mission-1 win at the Vita default settings with the packs as installed on the Vita (low priority: the box is shared)
cd /work/ws/all
export INFECTED_SCREENSHOT_DIR=/work/ws/all/private/shots_rc/replay_gl INFECTED_SCREENSHOT_FLIPS=3600,5000,7000,9000,9300,9470
s=$(date +%s)
NAME=replay_gl GL=real SCALE=2 PACKS=/work/ws/all/private/packs_vita TIMEOUT=7000 BOT_WS=/work/ws/all \
  BIN=/work/ws/all/build/desktop/bin/InfectedNative nice -n 9 proj/tools/bot/replay_check.sh
echo "wall $(( $(date +%s) - s )) s"
