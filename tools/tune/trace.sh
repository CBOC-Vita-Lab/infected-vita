#!/bin/bash
# usage: trace.sh NAME SECONDS ADDRS [VAR=val ...]  : lean run (hooks on, virtual clock, null draw) where the functions ADDRS (comma list, hex) run in the INTERPRETER
# with a PC watch on each of them (registers and, with PSPRECOMP_INTERPRETER_WATCH_STACK=1, the stack). Indirect (vtable) calls only: direct lean calls bypass the choice.
S=/work/ws/tune
N=$1; T=$2; A=$3; shift 3
PADF=${PAD:-$S/tools/ep1.script}
exec $S/tools/run.sh ${BIN:-$S/build/bin/InfectedNative} $T $N INFECTED_INTERP_ONLY=$A PSPRECOMP_INTERPRETER_WATCH=$A PSPRECOMP_INTERPRETER_WATCH_STACK=1 INFECTED_LIBC_HOOKS=1 INFECTED_CLOCK=virtual INFECTED_NO_BUSY_CLOCK=1 INFECTED_SPIN_INSNS=0 INFECTED_FRAME_LIMIT_US=16683 INFECTED_UNTHROTTLED=1 INFECTED_GL_NULL_DRAW=1 "INFECTED_PAD_SCRIPT=@$PADF" "$@"
