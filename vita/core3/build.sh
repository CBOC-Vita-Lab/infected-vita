#!/usr/bin/env bash
# Builds infected_core3.skprx (see core3.c) into OUT_DIR (default: this directory). Needs VITASDK and the taiHEN kernel stubs in
# VITA_SYSROOT/lib (vita/scripts/build-sysroot.sh puts them there).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here}
: "${VITASDK:?set VITASDK}"
sysroot=${VITA_SYSROOT:-$here/../sysroot}
export PATH=$VITASDK/bin:$PATH
mkdir -p "$out"
arm-vita-eabi-gcc -Wl,-q -Wall -Wextra -O2 -mthumb -nostartfiles -nostdlib -I"$sysroot/include" "$here/core3.c" \
    -L"$sysroot/lib" -ltaihenForKernel_stub -lSceSysrootForDriver_stub -o "$out/infected_core3.elf"
vita-elf-create -e "$here/exports.yml" "$out/infected_core3.elf" "$out/infected_core3.velf" > /dev/null
vita-make-fself -s -c "$out/infected_core3.velf" "$out/infected_core3.skprx" > /dev/null
echo "$out/infected_core3.skprx"
