#!/usr/bin/env python3
"""Instruction-level validation of selftest/coro_arm.S + coro_core.cpp (T3) in the Unicorn ARM emulator.
We cannot run Vita code in this environment; this proves the context switch really preserves r4-r11/d8-d15 and that the
trampoline / C++ coroutine path works, before the code ever meets a real Cortex-A9.
usage: validate_coro_unicorn.py [--hops N]     (expects build/st/coro_test.elf, produced by scripts/build-selftest-coro-check.sh)
"""
import struct, subprocess, sys, os
from unicorn import *
from unicorn.arm_const import *

here = os.path.dirname(os.path.abspath(__file__))
ws = os.path.dirname(here)
elf = os.path.join(ws, "build/st/coro_test.elf")
hops = 3000
if "--hops" in sys.argv:
    hops = int(sys.argv[sys.argv.index("--hops") + 1])

def objcopy_bin(section):
    out = subprocess.check_output(["arm-vita-eabi-objcopy", "-O", "binary", "-j", section, elf, "/dev/stdout"])
    return out

def sym(name):
    for line in subprocess.check_output(["arm-vita-eabi-nm", elf]).decode().splitlines():
        p = line.split()
        if len(p) == 3 and p[2] == name:
            return int(p[0], 16)
    raise SystemExit("symbol %s not found" % name)

text = objcopy_bin(".text")
TEXT = 0x10000
BSSA, BSSZ = 0x40000, 0x1000
STK_MAIN = 0x100000
STK_CO = 0x200000
CO_BYTES = 256 * 1024
OUT = 0x300000
SENT = 0x8000

def run(mode, n):
    mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    try:
        mu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A9)
    except Exception as e:
        print("  (cpu model not selectable:", e, ")")
    mu.mem_map(TEXT, 0x10000)
    mu.mem_write(TEXT, text)
    mu.mem_map(BSSA, BSSZ)
    mu.mem_map(SENT & ~0xFFF, 0x1000)
    mu.mem_write(SENT, struct.pack("<I", 0xE12FFF1E))   # bx lr (never reached; we stop at SENT)
    mu.mem_map(STK_MAIN, 0x10000)
    mu.mem_map(STK_CO, 4 * CO_BYTES)
    mu.mem_map(OUT, 0x1000)
    # enable VFP/NEON
    mu.reg_write(UC_ARM_REG_C1_C0_2, 0xF00000)
    mu.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
    entry = sym("st_coro_run")   # nm prints the Thumb bit as the plain address; this function is Thumb
    sp = STK_MAIN + 0x10000 - 16
    mu.mem_write(sp, struct.pack("<I", OUT))   # 5th argument on the stack
    mu.reg_write(UC_ARM_REG_SP, sp)
    mu.reg_write(UC_ARM_REG_R0, mode)
    mu.reg_write(UC_ARM_REG_R1, n)
    mu.reg_write(UC_ARM_REG_R2, CO_BYTES)
    mu.reg_write(UC_ARM_REG_R3, STK_CO)
    mu.reg_write(UC_ARM_REG_LR, SENT)
    # poison callee-saved regs so a broken restore shows up
    for i, r in enumerate([UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11]):
        mu.reg_write(r, 0xDEAD0000 + i)
    mu.emu_start(entry | 1, SENT, count=400_000_000)
    sw, err, mask, deep = struct.unpack("<IIII", mu.mem_read(OUT, 16))
    r0 = mu.reg_read(UC_ARM_REG_R0)
    # callee-saved registers of the caller must be intact on return
    kept = all(mu.reg_read(r) == 0xDEAD0000 + i for i, r in enumerate([UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11]))
    return sw, err, mask, deep, r0, kept

ok = True
for mode, name, n in [(0, "star (scheduler<->coroutine)", hops), (1, "ring (coroutine->coroutine, asm body)", hops), (2, "ring with C++ bodies + deep recursion", max(8, hops // 30))]:
    sw, err, mask, deep, r0, kept = run(mode, n)
    good = err == 0 and mask == 0xF and r0 == 0 and kept
    ok &= good
    print("mode %d %-42s hops=%d switches=%d errors=%d mask=0x%X deep_levels=%d ret=%d caller_regs_kept=%s  -> %s"
          % (mode, name, n, sw, err, mask, deep, r0, kept, "OK" if good else "FAIL"))
sys.exit(0 if ok else 1)
