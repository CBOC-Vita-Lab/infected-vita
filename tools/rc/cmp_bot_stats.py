#!/usr/bin/env python3 -I
"""cmp_bot_stats.py DESKTOP.err VITA3K_LOG : compares the `[bot] stats flip ...` lines and `[bot] event mission_complete` / `results_screen` lines of two runs (flip, emu_us, score, kills, shots, seconds, medal, heap_top, npc_live)."""
import re, sys
def grab(path):
    st, ev = {}, {}
    for line in open(path, errors='replace'):
        m = re.search(r'\[bot\] stats flip (\d+) (.*)', line)
        if m: st[int(m.group(1))] = m.group(2).strip()
        m = re.search(r'\[bot\] event (mission_complete|results_screen|player_dead|player_revived) flip (\d+)(.*)', line)
        if m: ev[(m.group(1), int(m.group(2)))] = m.group(3).strip()
    return st, ev
a, ea = grab(sys.argv[1]); b, eb = grab(sys.argv[2])
common = sorted(set(a) & set(b))
same = sum(1 for f in common if a[f] == b[f])
print(f"stats lines: desktop {len(a)}, other {len(b)}, common {len(common)}, identical {same}")
for f in common:
    if a[f] != b[f]: print(f"  DIFF flip {f}\n    {a[f]}\n    {b[f]}")
print("events only in one run:", sorted(set(ea) ^ set(eb)))
print("events in both with a different text:", [k for k in sorted(set(ea) & set(eb)) if ea[k] != eb[k]])
print("last common flip:", common[-1] if common else None)
