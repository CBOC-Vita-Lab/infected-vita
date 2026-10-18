#!/usr/bin/env python3
"""mkplay.py OUT.plan SEGFILE: plan (tools/mkpad.py format) for the play test: the boot/menu walk of mkflow.py (CROSS presses at the title,
main menu, campaign menu, mission selection, briefing) followed by the gameplay segments in SEGFILE, one per line:
    REL_FLIP BUTTONS...      (REL_FLIP counted from the start of gameplay, flip 5900; '-' releases everything)
Lines starting with # are comments. The output is sorted by absolute flip."""
import sys
out, seg = sys.argv[1], sys.argv[2]
plan = []
def pulse(t, btn, length=6):
    plan.append((t, btn)); plan.append((t + length, '-'))
for t in (2650, 2800, 5100, 5250, 5400, 5550, 5700):
    pulse(t, 'CROSS')
G = 5900
for line in open(seg):
    line = line.split('#')[0].strip()
    if not line: continue
    f, _, rest = line.partition(' ')
    plan.append((G + int(f), rest.strip() or '-'))
plan.sort(key=lambda e: e[0])
with open(out, 'w') as fo:
    for fl, b in plan: fo.write('%d %s\n' % (fl, b))
