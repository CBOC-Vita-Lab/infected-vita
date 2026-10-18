#!/usr/bin/env python3
"""gdiff.py LOG FLIP_A FLIP_B FLIP_C : words of the INFECTED_TUNE_DUMP dumps that differ A->B but are equal B==C (a state that changed once and stayed)."""
import sys, re, struct
def load(path):
    d = {}
    for l in open(path, errors='replace'):
        m = re.match(r'\[dump (\d+)\] ([0-9a-f]{8}):(.*)', l)
        if not m: continue
        f = int(m.group(1)); a = int(m.group(2), 16)
        for i, w in enumerate(m.group(3).split()): d.setdefault(f, {})[a + 4*i] = int(w, 16)
    return d
def fl(w): return struct.unpack('<f', struct.pack('<I', w))[0]
d = load(sys.argv[1]); a, b, c = (int(x) for x in sys.argv[2:5])
n = 0
for addr in sorted(d[a]):
    if addr in d[b] and addr in d[c] and d[a][addr] != d[b][addr] and d[b][addr] == d[c][addr]:
        print('%08x %08x -> %08x (f %.4g -> %.4g)' % (addr, d[a][addr], d[b][addr], fl(d[a][addr]), fl(d[b][addr]))); n += 1
        if n > 200: break
