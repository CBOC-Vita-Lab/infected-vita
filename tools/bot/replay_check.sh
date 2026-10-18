#!/bin/bash
# replay_check.sh [PADFILE [EXPECTFILE]]
# End-to-end regression test: replays a recorded pad log (no bot, no decisions) on lean code with the deterministic virtual clock
# and checks the outcome: "mission complete" reached at the expected flip with the expected score board, the results screen
# at the expected flip, and the score written to the save file (INFECTED.BIN u16 at 0x3283 / hazard pay at 0x329a, mission 1 record).
# The observer (INFECTED_BOT_OBSERVE=1) only READS guest memory and prints "[bot] event ..." lines; it supplies no input.
# Environment: TIMEOUT (seconds, default 900; a real-GL run needs more), NAME, BOT_WS (workspace, default /home/claude/ws/bot), BIN, MS0, DISC as for run.sh; STRICT=0 accepts any completion
# with the expected score (for a platform whose floating point is not bit-identical).
HERE=$(dirname "$(realpath "$0")")
W=${BOT_WS:-/home/claude/ws/bot}
PAD=$(realpath "${1:-$HERE/mission1_win.pad}")
EXP=${2:-$HERE/mission1_win.expect}
. "$EXP"
NAME=${NAME:-replay_check}   # NAME=... keeps several variants (settings, renderer) side by side
PAD=$PAD "$HERE/run.sh" $NAME ${TIMEOUT:-900} INFECTED_MAX_FRAMES=$FRAMES INFECTED_BOT_OBSERVE=1 > /dev/null
ERR=$W/private/logs/$NAME.err
fail=0
chk() { if [ "$2" = "$3" ]; then echo "PASS $1 = $3"; else echo "FAIL $1: expected $2 got $3"; fail=1; fi; }
EV=$(grep -E "event mission_complete" "$ERR" | head -1)
get() { echo "$EV" | sed -n "s/.* $1 \([0-9]*\).*/\1/p"; }
if [ -z "$EV" ]; then echo "FAIL no mission_complete event"; fail=1; else
  if [ "${STRICT:-1}" = 1 ]; then chk "mission complete flip" "$COMPLETE_FLIP" "$(echo "$EV" | sed -n 's/.*flip \([0-9]*\).*/\1/p')"; fi
  chk score "$SCORE" "$(get score)"; chk kills "$KILLS" "$(get kills)"; chk viral_shots "$SHOTS" "$(get viral_shots)"
  chk seconds "$SECONDS_" "$(get seconds)"; chk hazard "$HAZARD" "$(get hazard)"; chk medal "$MEDAL" "$(get medal)"; chk deaths "$DEATHS" "$(get deaths)"
fi
if [ "${STRICT:-1}" = 1 ]; then
  chk "results screen flip" "$RESULTS_FLIP" "$(grep -E 'event results_screen' "$ERR" | head -1 | sed -n 's/.*flip \([0-9]*\).*/\1/p')"
fi
SAVE=$W/private/data_$NAME/ms0/PSP/SAVEDATA/ULUS100540000/INFECTED.BIN
# the mission 1 record is {score u16, ..., hazard pay u16 at +0x17}; its offset moves with the length of the officer name (0x3283 for the
# one-letter name of private/ms0.pristine, 0x3295 for ten letters), so look for the pair
read s h < <(python3 - "$SAVE" "$SAVE_SCORE" "$SAVE_HAZARD" <<'PY'
import sys, struct
b = open(sys.argv[1], 'rb').read() if __import__('os').path.exists(sys.argv[1]) else b''
want_s, want_h = int(sys.argv[2]), int(sys.argv[3])
for i in range(0x3000, min(len(b) - 0x19, 0x4000)):
    if struct.unpack_from('<H', b, i)[0] == want_s and struct.unpack_from('<H', b, i + 0x17)[0] == want_h:
        print(want_s, want_h); break
else:
    print(0, 0)
PY
)
chk "save score (mission 1 record)" "$SAVE_SCORE" "$s"; chk "save hazard pay (mission 1 record)" "$SAVE_HAZARD" "$h"
[ $fail = 0 ] && echo "REPLAY OK" || echo "REPLAY FAILED"
exit $fail
