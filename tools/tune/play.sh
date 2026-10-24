#!/bin/bash
# usage: play.sh NAME SECONDS [VAR=val ...]
# Lean-only run with the libc hooks on and the deterministic virtual clock (CLOCKMODE=realtime for the real clock).
# Environment: BIN (default build/bin/InfectedNative), PAD (pad script file, default tools/continue_flips.script), MS0 (memory stick image).
S=/work/ws/tune
N=$1; T=$2; shift 2
BIN=${BIN:-$S/build/bin/InfectedNative}
PADF=${PAD:-$S/tools/continue_flips.script}
if [ "${CLOCKMODE:-virtual}" = realtime ]; then
  CLK=(INFECTED_CLOCK=realtime)
else
  CLK=(INFECTED_CLOCK=virtual INFECTED_NO_BUSY_CLOCK=1 INFECTED_SPIN_INSNS=0 INFECTED_FRAME_LIMIT_US=16683 INFECTED_UNTHROTTLED=1)
fi
exec $S/tools/run.sh $BIN $T $N INFECTED_LEAN_ONLY=1 INFECTED_LIBC_HOOKS=1 "${CLK[@]}" "INFECTED_PAD_SCRIPT=@$PADF" "$@"
