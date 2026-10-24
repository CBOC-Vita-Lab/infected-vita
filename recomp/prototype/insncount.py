#!/usr/bin/env python3
"""Count ARM instructions / bytes of selected function symbols in an object file (Vita toolchain objdump).
usage: insncount.py OBJ SYMREGEX   -> prints per-symbol 'insns bytes' and the total.
Literal-pool words (.word / .short / .byte lines) are excluded from the instruction count but bytes are the symbol size."""
import re, subprocess, sys
env = {'PATH': '/work/vitasdk-root/vitasdk/bin:/usr/bin:/bin'}
def symbols(obj, rx):
    out = subprocess.run(['arm-vita-eabi-nm', '-S', '--defined-only', obj], capture_output=True, text=True, env=env).stdout
    res = {}
    for l in out.splitlines():
        m = re.match(r'([0-9a-f]+) ([0-9a-f]+) [Tt] (\S+)', l)
        if m and re.search(rx, m.group(3)): res[m.group(3)] = (int(m.group(1), 16), int(m.group(2), 16))
    return res
def count(obj, rx):
    syms = symbols(obj, rx)
    dis = subprocess.run(['arm-vita-eabi-objdump', '-d', '--no-show-raw-insn', obj], capture_output=True, text=True, env=env).stdout
    # parse into (addr, mnemonic)
    ins = []
    for l in dis.splitlines():
        m = re.match(r'\s*([0-9a-f]+):\s+(\S+)', l)
        if m: ins.append((int(m.group(1), 16), m.group(2)))
    ins.sort()
    tot_i = tot_b = 0
    per = {}
    for name, (a, sz) in syms.items():
        n = sum(1 for (ad, mn) in ins if a <= ad < a + sz and not mn.startswith('.'))
        per[name] = (n, sz); tot_i += n; tot_b += sz
    return per, tot_i, tot_b
if __name__ == '__main__':
    per, ti, tb = count(sys.argv[1], sys.argv[2])
    for k, (n, b) in sorted(per.items()): print('%-60s %6d insns %7d B' % (k, n, b))
    print('TOTAL %d insns %d B (%d symbols)' % (ti, tb, len(per)))
