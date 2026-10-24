#!/bin/bash
# usage: run.sh BIN SECONDS NAME [VAR=val ...]   -> private/logs/NAME.out / .err ; data dir private/data_NAME
# Restores the memory stick first so every run starts from the same state (private/ms0.pristine unless MS0=name).
BIN=$1; T=$2; N=$3; shift 3
W=/work/ws/tune/private
MS=${MS0:-ms0.pristine}
D=$W/data_$N
mkdir -p $D
cp -f $W/data/EBOOT.ELF $W/data/settings.ini $D/ 2>/dev/null
[ -e $D/ms0 ] && mv $D/ms0 $D/ms0.old.$$; cp -a $W/$MS $D/ms0
[ -n "$DISC" ] && sed -i "s|^disc_image=.*|disc_image=$DISC|" $D/settings.ini
export INFECTED_DATA_DIR=$D
export INFECTED_NO_AUDIO=1
export EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1
cd $W
mkdir -p logs
env "$@" nice -n 10 timeout -s KILL $T stdbuf -oL $BIN > logs/$N.out 2> logs/$N.err
echo "exit=$?"
