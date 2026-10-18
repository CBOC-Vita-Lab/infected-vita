#!/usr/bin/env python3
"""mkpad.py: build a flip-keyed pad script from a compact plan.
plan lines (stdin or file):  FLIP BUTTONS [LX=n LY=n RX=n RY=n]    ('-' = nothing); a line holds until the next one.
Output: one line `flip:BTN+BTN+LX=n,...` suitable for INFECTED_PAD_SCRIPT=@file (newlines work as separators)."""
import sys
out = []
for line in (open(sys.argv[1]) if len(sys.argv) > 1 else sys.stdin):
    line = line.split('#')[0].strip()
    if not line: continue
    p = line.split()
    flip, parts = p[0], []
    for t in p[1:]:
        parts += t.split('+') if '=' not in t else [t]
    out.append('%s:%s' % (flip, '+'.join(x for x in parts if x) or '-'))
print('\n'.join(out))
