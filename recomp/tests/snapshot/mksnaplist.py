#!/usr/bin/env python3
"""Choose the functions and occurrences to snapshot.

    mksnaplist.py ANALYSIS.pkl PCHIST.pkl OUT.txt [HOT=60]

ANALYSIS.pkl is the pickle written by lean_gen.py --analysis (P, L); PCHIST.pkl holds (hist of the run to cover, hist of all runs):
dicts pc -> executed count.  For every function that was executed at least once the output lists occurrence numbers of its
entry: 1 and two later ones (a quarter and half way through its executions); the HOT hottest functions (by executed instructions of
the first histogram) get six occurrences spread over the whole run.  The format is what LEAN_SNAP_FUNCS expects:
    0xADDRESS occ[,occ...]
"""
import sys, pickle, collections, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
sys.setrecursionlimit(100000)
analysis, hist, out = sys.argv[1:4]
hot_n = int(sys.argv[4]) if len(sys.argv) > 4 else 60
P, L = pickle.load(open(analysis, 'rb'))
first, allc = pickle.load(open(hist, 'rb'))
pc2f = {}
for e, s in P.S.items():
    for pc in s.f.pcs: pc2f[pc] = e
per = collections.Counter()
for pc, c in first.items(): per[pc2f[pc]] += c
hot = set(e for e, c in per.most_common(hot_n))
rows = []
for e in sorted(P.S):
    n = allc.get(e, 0)
    if n <= 0: continue
    occ = {1, n // 20 + 1, n // 8 + 1, n // 4 + 1, n // 2 + 1, (3 * n) // 4 + 1} if e in hot else {1, n // 4 + 1, n // 2 + 1}
    rows.append((e, sorted(o for o in occ if 1 <= o <= n)))
with open(out, 'w') as f:
    for e, occ in rows: f.write('0x%08X %s\n' % (e, ','.join(map(str, occ))))
print(len(rows), 'functions', sum(len(o) for e, o in rows), 'snapshots')
