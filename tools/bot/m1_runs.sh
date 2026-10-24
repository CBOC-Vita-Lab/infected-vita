#!/bin/bash
# usage: m1_runs.sh [PHASE ...]   mission 1 from a pristine memory stick, one bot run per menu-timing phase (lean, virtual clock);
# prints the mission_complete / death events, the save file result (score at 0x3283) and the results-screen flip.
W=${BOT_WS:-/work/ws/bot}
PH=${@:-0 17 53 81}
for p in $PH; do
  $W/proj/tools/bot/run.sh m1_ph$p 900 INFECTED_MAX_FRAMES=${FRAMES:-12500} INFECTED_BOT=1 INFECTED_BOT_PARAMS=phase=$p > /dev/null
  echo "== phase $p"
  grep -E "event (mission_complete|player_dead|player_revived)" $W/private/logs/m1_ph$p.err | cut -c1-190
  python3 - "$W/private/data_m1_ph$p/ms0/PSP/SAVEDATA/ULUS100540000/INFECTED.BIN" <<'PY'
import sys, struct
b = open(sys.argv[1], 'rb').read()
print("   save: mission 1 score", struct.unpack_from('<H', b, 0x3283)[0], "hazard", struct.unpack_from('<H', b, 0x329a)[0], " mission 2 score", struct.unpack_from('<H', b, 0x33c5)[0])
PY
done
