#!/usr/bin/env python3
"""reloc_dis.py IN.dis OUT.txt : relocate llvm-objdump output of image.o (file offsets) to guest addresses (base 0x08804000).
Branch targets (b*, relative) get +base; jal/j are already absolute. Strip <...> symbol annotations."""
import sys, re
BASE = 0x08804000
out = open(sys.argv[2], 'w')
rx = re.compile(r'^\s*([0-9a-f]+):\s+(.*)$')
for l in open(sys.argv[1]):
    m = rx.match(l)
    if not m: continue
    a = int(m.group(1), 16) + BASE
    t = re.sub(r'<[^>]*>', '', m.group(2)).rstrip()
    mm = re.match(r'(b\w*)\s+(.*?)(0x[0-9a-f]+)$', t)
    if mm and not t.startswith(('break', 'bitrev')):
        t = '%s %s0x%x' % (mm.group(1), mm.group(2), int(mm.group(3), 16) + BASE)
    out.write('%08x: %s\n' % (a, t))
