#!/bin/bash
# Lean-code run of the desktop build with the virtual clock, for the graphics options and the settings overlay.
#
#   gfx_live.sh NAME SECONDS [VAR=val ...]
#
# Environment: BIN (default build/bin/InfectedNative of the gfx workspace), PAD (pad script file, passed as the text of
# <prefix>_PAD_SCRIPT; default
# tools/gfx_overlay_flow.script: it plays to mission 1, walks with the left stick, then holds SELECT+START at flip 3900),
# SRC (a directory with EBOOT.ELF, settings.ini and ms0.pristine/; default the integration workspace's private data, only read),
# W (where runs are made, default <workspace>/private/live). Prints the log tail. Logs: $W/logs/NAME.{out,err};
# captures: $W/shots/NAME/.
#
# The data directory is made fresh for every run (the memory stick is restored, settings.txt removed), so every run starts from
# the same state; the game's own files are never written. Virtual clock, audio device off, frame cap off, so the run is
# deterministic and as fast as the host allows.
S=${SRC:-/work/ws/integ/private}
G=${GFX:-/work/ws/gfx}
W=${W:-$G/private/live}
N=$1; T=$2; shift 2
BIN=${BIN:-$G/build/bin/InfectedNative}
PADF=${PAD:-$G/proj/tools/gfx_overlay_flow.script}
D=$W/data_$N
mkdir -p "$D" "$W/logs" "$W/shots/$N"
cp -f "$S/data/EBOOT.ELF" "$S/data/settings.ini" "$D/"
rm -rf "$D/ms0" "$D/settings.txt" && cp -a "$S/ms0.pristine" "$D/ms0"
export EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1
cd "$W" || exit 1
env INFECTED_DATA_DIR="$D" INFECTED_NO_AUDIO=1 INFECTED_LEAN_ONLY=1 INFECTED_LIBC_HOOKS=1 \
    INFECTED_CLOCK=virtual INFECTED_NO_BUSY_CLOCK=1 INFECTED_SPIN_INSNS=0 INFECTED_FRAME_LIMIT_US=16683 INFECTED_UNTHROTTLED=1 \
    INFECTED_PERF_FRAME_CAP=off "INFECTED_PAD_SCRIPT=$(tr -d '\n' < "$PADF")" INFECTED_SCREENSHOT_DIR="$W/shots/$N" \
    "$@" nice -n 10 timeout -s KILL "$T" stdbuf -oL "$BIN" > "$W/logs/$N.out" 2> "$W/logs/$N.err"
echo "exit=$?"
