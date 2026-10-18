#!/usr/bin/env python3
"""dumpdiff.py LOG FLIP_A FLIP_B [tag]  : words that differ between two [bot-probe] dumps of the same log (player or npc objects).
Prints offset, both words and float readings. tag = player | npc (default player). With 'show' as 5th arg prints the whole object of FLIP_A."""
import sys, re, struct
def f(w):
    return struct.unpack('<f', struct.pack('<I', w))[0]
def parse(path):
    cur = None; res = {}
    for l in open(path, errors='replace'):
        m = re.match(r'\[bot-probe\] flip (\d+)', l)
        if m: cur = int(m.group(1)); res[cur] = {}; continue
        m = re.match(r'\[(player|npc|ptr) ([0-9a-f]{8})\] \+([0-9a-f]{4}):(.*)', l)
        if m and cur is not None:
            tag, base, off, rest = m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4).split()
            d = res[cur].setdefault((tag, base), {})
            for i, w in enumerate(rest): d[off + 4*i] = int(w, 16)
    return res
def fmt(w):
    fl = f(w)
    s = ''
    if w >= 0x08800000 and w < 0x0a000000: s = ' ptr'
    elif abs(fl) > 1e-6 and abs(fl) < 1e7: s = ' f=%.4g' % fl
    return '%08x%s' % (w, s)
if __name__ == '__main__':
    log, a, b = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    tag = sys.argv[4] if len(sys.argv) > 4 else 'player'
    res = parse(log)
    for (t, base), da in res[a].items():
        if t != tag: continue
        db = res[b].get((t, base))
        if db is None: print('%s %08x missing in %d' % (t, base, b)); continue
        print('== %s %08x' % (t, base))
        for off in sorted(da):
            if len(sys.argv) > 5 and sys.argv[5] == 'show': print('+%04x %s' % (off, fmt(da[off])))
            elif da[off] != db.get(off): print('+%04x %s -> %s' % (off, fmt(da[off]), fmt(db.get(off, 0))))
