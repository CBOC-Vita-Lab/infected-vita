#!/usr/bin/env python3
"""mkflow.py OUT.plan [GAMEPLAY_FLIPS]: plan (tools/mkpad.py format) for the full flow of the pristine save:
logos + intro.pmf play unskipped, CONTINUE at the title, CAMPAIGN in the main menu, tut1.pmf plays unskipped, Mission Selection,
mission 1 briefing, then a gameplay macro (lock-on, pistol, viral gun, turning, popup dismissal) repeated for GAMEPLAY_FLIPS flips.
Flip numbers are those of the virtual-clock lean run (movies are flip-counted: 30 flips per emulated second)."""
import sys
out = sys.argv[1]
n = int(sys.argv[2]) if len(sys.argv) > 2 else 11000
plan = []
def pulse(t, btn, length=6):
    plan.append((t, btn)); plan.append((t + length, '-'))
for t in (2650, 2800, 5100, 5250, 5400, 5550, 5700):
    pulse(t, 'CROSS')
start = 5900
cycle = 120
t = start
phases = [(0, 'R+CROSS+LY=40'), (40, 'R+SQUARE+LX=60'), (80, 'R+CROSS+LX=200'), (110, '-'), (113, 'TRIANGLE'), (118, '-')]
while t < start + n:
    for off, st in phases:
        plan.append((t + off, st))
    t += cycle
# keep the last events of the same flip in order: stable sort by flip
plan.sort(key=lambda e: e[0])
with open(out, 'w') as f:
    for fl, b in plan: f.write('%d %s\n' % (fl, b))
