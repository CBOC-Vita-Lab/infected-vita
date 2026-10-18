#!/bin/bash
# Lean-code run of the merged desktop build (all branches together): the pad script plays to mission 1, opens the settings menu
# three times mid-mission (a menu script changes options each time) and the run ends with a few more pictures.
#
#   all_live.sh NAME SECONDS [VAR=val ...]
#
# Environment: BIN (default <workspace>/build/desktop/bin/InfectedNative), PAD (pad script file, default tools/all_live_flow.script),
# OVR (menu script, default tools/all_live.overlay), THREADED=1|0 (the GE thread on its own core; default 1 = the default setting),
# FLIPS (flips whose picture is captured), SRC (directory with EBOOT.ELF, settings.ini and ms0.pristine/, only read),
# PACKS (directory with texpack_2x.bin / texpack_4x.bin, linked into the data directory; "none" for no pack),
# SETTINGS (text written to the data directory's settings.txt, e.g. "gfx.hd_movies=on"; default: none, the defaults),
# HDMOVIES (a movies_hd directory of HD replacement movies, linked into the data directory as movies_hd; default none),
# W (where runs are made, default <workspace>/private/live).
# Logs: $W/logs/NAME.{out,err}; captures: $W/shots/NAME/ (flip_NNNNNN.png of the game, menu_*.png of the menu).
#
# The data directory is made fresh for every run (the memory stick restored, settings.txt removed): every run starts from the same
# state with the DEFAULT settings. Virtual clock, audio device off, so the run is deterministic and as fast as the host allows.
here=$(cd "$(dirname "$0")" && pwd)
ws=$(cd "$here/../.." && pwd)
S=${SRC:-/home/claude/ws/integ/private}
W=${W:-$ws/private/live}
N=$1; T=$2; shift 2
BIN=${BIN:-$ws/build/desktop/bin/InfectedNative}
PADF=${PAD:-$here/all_live_flow.script}
OVRF=${OVR:-$here/all_live.overlay}
PK=${PACKS:-/home/claude/ws/tex/private/packs}
FL=${FLIPS:-3890,3960,4090,4160,4290,4360,4480,4500}
D=$W/data_$N
mkdir -p "$D" "$W/logs" "$W/shots/$N"
rm -f "$W/shots/$N"/*.png
cp -f "$S/data/EBOOT.ELF" "$S/data/settings.ini" "$D/"
rm -rf "$D/ms0" "$D/settings.txt" "$D/texpack_2x.bin" "$D/texpack_4x.bin" "$D/movies_hd" && cp -a "$S/ms0.pristine" "$D/ms0"
[ -n "${SETTINGS:-}" ] && printf '%b\n' "$SETTINGS" > "$D/settings.txt"
[ -n "${HDMOVIES:-}" ] && ln -s "$(cd "$HDMOVIES" && pwd)" "$D/movies_hd"
if [ "$PK" != none ]; then
  for f in texpack_2x.bin texpack_4x.bin; do [ -f "$PK/$f" ] && ln -s "$PK/$f" "$D/$f"; done
fi
EXTRA=(); [ "${THREADED:-1}" = 0 ] && EXTRA=(INFECTED_PERF_THREADED_RENDERER=off)   # the default (no override) is the threaded renderer
export EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1
cd "$W" || exit 1
env INFECTED_DATA_DIR="$D" INFECTED_NO_AUDIO=1 INFECTED_LEAN_ONLY=1 INFECTED_LIBC_HOOKS=1 \
    INFECTED_CLOCK=virtual INFECTED_NO_BUSY_CLOCK=1 INFECTED_SPIN_INSNS=0 INFECTED_FRAME_LIMIT_US=16683 INFECTED_UNTHROTTLED=1 \
    "${EXTRA[@]}" "INFECTED_PAD_SCRIPT=$(tr -d '\n' < "$PADF")" "INFECTED_OVERLAY_SCRIPT=$(tr -d '\n' < "$OVRF")" \
    INFECTED_SCREENSHOT_DIR="$W/shots/$N" INFECTED_SCREENSHOT_FLIPS="$FL" \
    "$@" nice -n 10 timeout -s KILL "$T" stdbuf -oL "$BIN" > "$W/logs/$N.out" 2> "$W/logs/$N.err"
echo "exit=$?"
