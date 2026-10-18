#!/usr/bin/env bash
# Builds and runs the selftest on the host (platform/desktop_headless): proves the harness, T1/T2/T4/T5(null audio)/T10 logic and
# the platform twin compile and behave. GL (T6-T8), ARM coroutines (T3) and FFmpeg (T9) are SKIPped there.
#   usage: build-selftest-desktop.sh [run]
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
out=$here/build/desktop
mkdir -p "$out"
CXX=${CXX:-g++}
$CXX -std=c++20 -O1 -g -fno-exceptions -fno-rtti -Wall -Wextra -Wno-unused-parameter \
    -o "$out/infected_selftest_desktop" \
    "$here"/platform/desktop_headless/platform_desktop.cpp \
    "$here"/selftest/st_harness.cpp "$here"/selftest/st_console_stub.cpp "$here"/selftest/t_sys.cpp "$here"/selftest/t_audio.cpp \
    "$here"/selftest/t_coro.cpp "$here"/selftest/t_gl_stub.cpp "$here"/selftest/t_ffmpeg_stub.cpp \
    -lpthread
echo "built $out/infected_selftest_desktop"
if [ "${1:-}" = run ]; then
    rm -rf "$out/data"; mkdir -p "$out/data"
    printf 't1=1\nt4.ms=300\nt4.rounds=300\nt5.ms=600\nt10.mib=4\nexit_delay_ms=0\n' > "$out/data/selftest.cfg"
    INFECTED_DATA_ROOT="$out/data/" INFECTED_APP_ROOT="$out/app/" "$out/infected_selftest_desktop"; rc=$?
    echo "exit code $rc"; echo ---- selftest.txt; cat "$out/data/selftest.txt"
fi
