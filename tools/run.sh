#!/bin/bash
# usage: run.sh SECONDS LOGNAME [VAR=val ...]   -> logs/LOGNAME.out / .err
T=$1; N=$2; shift 2
export INFECTED_DATA_DIR=/work/infected/data
export INFECTED_NO_RENDER=1 INFECTED_NO_AUDIO=1
cd /work/infected
env "$@" timeout -s KILL $T /work/infected/port/out/bin/InfectedNative > logs/$N.out 2> logs/$N.err
echo "exit=$? "
