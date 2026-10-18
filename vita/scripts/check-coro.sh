#!/usr/bin/env bash
# Build the freestanding coroutine core for ARM and run it in the Unicorn emulator (python3 -m pip install unicorn).
# This is the only way to exercise selftest/coro_arm.S without hardware or Vita3K.
#   usage: check-coro.sh [--hops N]     real switch; every mode must print OK (exit 0)
#          check-coro.sh --negative     negative control: the same test against a deliberately broken switch (r7 clobbered
#                                       after the restore); the validator must report FAIL (exit 1), otherwise the test is blind
set -euo pipefail
source "$(dirname "$0")/env.sh"
cd "$VITA_WS"
mkdir -p build/st
asm=selftest/coro_arm.S
neg=0
if [ "${1:-}" = "--negative" ]; then
  neg=1; shift
  asm=build/st/coro_arm_broken.S
  # clobber r7 after the context is restored -> the register check in the test body must notice
  sed 's/^        mov     sp, r12$/        mov     r7, #0\n        mov     sp, r12/' selftest/coro_arm.S > "$asm"
  cmp -s "$asm" selftest/coro_arm.S && { echo "negative control: sed did not change the assembly"; exit 2; }
fi
arm-vita-eabi-g++ -std=c++20 -O2 -mthumb -fno-exceptions -fno-rtti -fno-short-enums -ffreestanding -c selftest/coro_core.cpp -o build/st/coro_core.o
arm-vita-eabi-gcc -c "$asm" -o build/st/coro_arm.o
arm-vita-eabi-gcc -nostdlib -nostartfiles -static -Wl,-Ttext=0x10000 -Wl,-Tdata=0x40000 -Wl,--gc-sections -Wl,-e,st_coro_run \
    build/st/coro_core.o build/st/coro_arm.o -o build/st/coro_test.elf
if [ $neg = 1 ]; then
  if python3 -I scripts/validate_coro_unicorn.py "$@"; then echo "NEGATIVE CONTROL FAILED: the broken switch passed"; exit 1; fi
  echo "negative control OK: the validator rejects the broken switch"
else
  python3 -I scripts/validate_coro_unicorn.py "$@"
fi
