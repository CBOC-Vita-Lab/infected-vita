#!/usr/bin/env python3
# usage: hotfuncs.py N pc1.csv [pc2.csv ...]  -> top N guest functions by interpreted instructions
# Function starts: jal targets plus `addiu sp,sp,-N` prologues in the relocated image (work/image.bin).
import sys, struct, bisect
BASE = 0x08804000
img = open('/home/claude/infected/work/image.bin', 'rb').read()
n = len(img) // 4
words = struct.unpack('<%dI' % n, img[:n*4])
starts = set()
for i, w in enumerate(words):
    op = w >> 26
    if op == 3:  # jal
        t = ((w & 0x3FFFFFF) << 2) | 0x08000000
        if BASE <= t < BASE + n*4: starts.add(t)
    elif op == 9 and ((w >> 21) & 31) == 29 and ((w >> 16) & 31) == 29 and (w & 0x8000):  # addiu sp,sp,-N
        starts.add(BASE + i*4)
starts = sorted(starts)
agg = {}; total = 0
N = int(sys.argv[1])
lines = (l for f in sys.argv[2:] for l in open(f))
for line in lines:
    a, c = line.split(',')[:2]
    try: pc = int(a, 16); c = int(c)
    except ValueError: continue
    total += c
    k = bisect.bisect_right(starts, pc) - 1
    f = starts[k] if k >= 0 else 0
    agg[f] = agg.get(f, 0) + c
top = sorted(agg.items(), key=lambda kv: -kv[1])[:N]
size = lambda f: (starts[bisect.bisect_right(starts, f)] - f) // 4 if bisect.bisect_right(starts, f) < len(starts) else 0
print('total instructions', total)
cum = 0
for f, c in top:
    cum += c
    print('0x%08X  %5.2f%%  cum %5.2f%%  %12d  ~%d insns long' % (f, 100*c/total, 100*cum/total, c, size(f)))
