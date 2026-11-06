#!/usr/bin/env bash
# Builds infected_clock.skprx (see clock500.c) into OUT_DIR (default: this directory). Needs VITASDK.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here}
: "${VITASDK:?set VITASDK}"
export PATH=$VITASDK/bin:$PATH
mkdir -p "$out"
arm-vita-eabi-gcc -Wl,-q -Wall -Wextra -O2 -mthumb -fno-builtin -fno-tree-loop-distribute-patterns -nostartfiles -nostdlib "$here/clock500.c" \
    -lSceSysmemForDriver_stub -lSceSysrootForDriver_stub -lSceProcEventForDriver_stub -lSceIofilemgrForDriver_stub \
    -lSceThreadmgrForDriver_stub -o "$out/infected_clock.elf"
vita-elf-create -e "$here/exports.yml" "$out/infected_clock.elf" "$out/infected_clock.velf" > /dev/null
vita-make-fself -s -c "$out/infected_clock.velf" "$out/infected_clock.skprx" > /dev/null
echo "$out/infected_clock.skprx"
