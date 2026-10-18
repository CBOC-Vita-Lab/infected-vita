#!/bin/bash
# usage: run.sh SECONDS LOGNAME [VAR=val ...]   -> logs/LOGNAME.out / .err
T=$1; N=$2; shift 2
export INFECTED_DATA_DIR=/home/claude/infected/data
export INFECTED_NO_RENDER=1 INFECTED_NO_AUDIO=1
cd /home/claude/infected
env "$@" timeout -s KILL $T /home/claude/infected/port/out/bin/InfectedNative > logs/$N.out 2> logs/$N.err
echo "exit=$? "
