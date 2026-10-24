#!/bin/bash
# usage: run.sh NAME SECONDS [VAR=val ...]
# Lean-only run (or INTERP=1: interpreter only) with the deterministic virtual clock, from a pristine memory stick.
# Output: private/logs/NAME.out/.err ; data dir private/data_NAME (settings.ini, ms0 = copy of private/$MS0, default ms0.pristine).
# Environment: BIN (default build/bin/InfectedNative), PAD (pad script file; default none), MS0, DISC (disc image), GL=null|real (default null draw),
#              SCALE (INFECTED_GL_DISPLAY_SCALE, default 1), CLOCKMODE=realtime,
#              PACKS (directory with texpack_2x.bin / texpack_4x.bin, linked into the data directory), SETTINGS (text for the data directory's
#              settings.txt, e.g. "gfx.hd_textures=4x\nperf.enemy_density=200"), HDMOVIES (a movies_hd directory, linked as movies_hd),
#              THREADED=0 (INFECTED_PERF_THREADED_RENDERER=off; the default is the threaded renderer, as the port ships).
W=${BOT_WS:-/work/ws/bot}
N=$1; T=$2; shift 2
BIN=${BIN:-$W/build/bin/InfectedNative}
MS=${MS0:-ms0.pristine}
D=$W/private/data_$N
mkdir -p $D $W/private/logs
cp -f $W/private/data/EBOOT.ELF $W/private/data/settings.ini $D/ 2>/dev/null
[ -e $D/ms0 ] && rm -rf $D/ms0
cp -a $W/private/$MS $D/ms0
[ -n "$DISC" ] && sed -i "s|^disc_image=.*|disc_image=$DISC|" $D/settings.ini
rm -rf $D/settings.txt $D/texpack_2x.bin $D/texpack_4x.bin $D/movies_hd
[ -n "$SETTINGS" ] && printf '%b\n' "$SETTINGS" > $D/settings.txt
[ -n "$HDMOVIES" ] && ln -s "$(cd "$HDMOVIES" && pwd)" $D/movies_hd
if [ -n "$PACKS" ]; then for f in texpack_2x.bin texpack_4x.bin; do [ -f "$PACKS/$f" ] && ln -s "$PACKS/$f" $D/$f; done; fi
if [ "${CLOCKMODE:-virtual}" = realtime ]; then
  CLK=(INFECTED_CLOCK=realtime)
else
  CLK=(INFECTED_CLOCK=virtual INFECTED_NO_BUSY_CLOCK=1 INFECTED_SPIN_INSNS=0 INFECTED_FRAME_LIMIT_US=16683 INFECTED_UNTHROTTLED=1)
fi
if [ -n "$INTERP" ]; then MODE=(INFECTED_INTERP_ONLY=all); else MODE=(INFECTED_LEAN_ONLY=1); fi
GLM=()
if [ "${GL:-null}" = null ]; then GLM=(INFECTED_GL_NULL_DRAW=1); else GLM=(INFECTED_GL_DISPLAY_SCALE=${SCALE:-1}); fi
PADV=()
[ -n "$PAD" ] && PADV=("INFECTED_PAD_SCRIPT=@$(realpath $PAD)")
[ "${THREADED:-1}" = 0 ] && export INFECTED_PERF_THREADED_RENDERER=off
export INFECTED_DATA_DIR=$D
export INFECTED_NO_AUDIO=${NO_AUDIO:-1}
export EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1
cd $W/private
env INFECTED_LIBC_HOOKS=1 "${MODE[@]}" "${CLK[@]}" "${GLM[@]}" "${PADV[@]}" "$@" nice -n 10 timeout -s KILL $T stdbuf -oL $BIN > logs/$N.out 2> logs/$N.err
echo "exit=$?"
