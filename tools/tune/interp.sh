#!/bin/bash
# usage: interp.sh NAME SECONDS [VAR=val ...]  : interpreter-only deterministic run (virtual clock) from the pristine save, null GL draw, continue_flips pad script (PAD overrides)
T=/work/ws/tune
N=$1; S=$2; shift 2
PADF=${PAD:-$T/tools/continue_flips.script}
exec $T/tools/run.sh ${BIN:-$T/build/bin/InfectedNative} $S $N INFECTED_INTERP_ONLY=all INFECTED_LIBC_HOOKS=1 INFECTED_CLOCK=virtual INFECTED_NO_BUSY_CLOCK=1 INFECTED_SPIN_INSNS=0 INFECTED_FRAME_LIMIT_US=16683 INFECTED_UNTHROTTLED=1 INFECTED_GL_NULL_DRAW=1 "INFECTED_PAD_SCRIPT=@$PADF" "$@"
