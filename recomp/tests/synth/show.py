"""print the guest code of one function of a synthetic case file:  show.py CASES.py ADDR|NAME [count]"""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..'))
from lean_gen import load_synth
from leangen import mips
im, _ = load_synth(sys.argv[1])
arg = sys.argv[2]
cnt = int(sys.argv[3]) if len(sys.argv) > 3 else 40
a = im.labels[arg] if arg in im.labels else int(arg, 0)
inv = {}
for n, x in im.labels.items(): inv.setdefault(x, []).append(n)
for i in range(cnt):
    pc = a + 4 * i
    w = im.word(pc)
    d = mips.decode(pc, w)
    print('%08x  %08x  %-10s %s' % (pc, w, d.mn, ' '.join(inv.get(pc, []))))
    if d.mn == 'jr' and d.rs == 31 and i > 0 and mips.decode(pc + 4, im.word(pc + 4)).mn == 'nop' and False: break
