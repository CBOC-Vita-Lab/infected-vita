#!/usr/bin/env python3
"""summ.py NAME.. : summary of a sustain run (logs/NAME.err): last flip, per-cell cap / max alive, pool peaks, heap top, world transitions, warnings"""
import re, sys
L = '/home/claude/ws/tune/private/logs/'
for n in sys.argv[1:]:
    try: lines = open(L + n + '.err', errors='replace').read().splitlines()
    except OSError: print(n, 'missing'); continue
    cells = [l for l in lines if l.startswith('[cells]')]
    pools = [l for l in lines if l.startswith('[pool]')]
    flips = [int(re.search(r'flip (\d+)', l).group(1)) for l in cells + pools]
    per = {}
    for l in cells:
        for m in re.finditer(r'\[id(\d+) cap=(\d+) alive=(\d+)\+(\d+) st=(\d+)', l):
            d = per.setdefault(m.group(1), dict(cap=set(), alive=[], st=set()))
            d['cap'].add(int(m.group(2))); d['alive'].append(int(m.group(3)) + int(m.group(4))); d['st'].add(int(m.group(5)))
    pk = {}
    for l in pools:
        for m in re.finditer(r'(\w+)=(\d+)/(\d+)\(pk (\d+)\)', l):
            pk[m.group(1)] = (max(pk.get(m.group(1), (0, 0))[0], int(m.group(4))), int(m.group(3)))
    tops = [(m.group(1), int(m.group(2), 16)) for m in (re.search(r'\[dump (\d+)\] 08add25c: \S+ \S+ \S+ (\S+)', l) for l in lines if l.startswith('[dump')) if m]
    worlds = [re.search(r'flip (\d+) world renderer (\S+)', l).groups() for l in lines if 'world renderer' in l]
    bad = [l[:100] for l in lines if re.search(r'stall|crash|fault|abort|SIG|unmapped|invalid|error', l, re.I) and not l.startswith('[io]')]
    print('==', n, 'last flip', max(flips) if flips else None)
    for k, d in sorted(per.items(), key=lambda kv: int(kv[0])):
        print('   cell id%s cap %s alive min %d max %d states %s' % (k, sorted(d['cap']), min(d['alive']), max(d['alive']), sorted(d['st'])))
    print('   pool peak/capacity:', ' '.join('%s %d/%d' % (k, v[0], v[1]) for k, v in pk.items()))
    print('   heap top free bytes at dumps:', ' '.join('%s:%.2fMB' % (f, t / 1048576) for f, t in tops))
    print('   world renderer:', worlds)
    print('   warnings:', bad[:3])
