#!/usr/bin/env python3
"""tab.py LOG [FROM [TO]] : condense the '[bot] flip ...' table lines of a run: player pos/hp/heading and each NPC as short-id hp state @distance/bearing."""
import sys, re, math
log = sys.argv[1]; lo = int(sys.argv[2]) if len(sys.argv) > 2 else 0; hi = int(sys.argv[3]) if len(sys.argv) > 3 else 1 << 60
cur = None
def flush():
    if cur is None: return
    f, p, ns = cur
    px, pz, hx, hz = p['x'], p['z'], p['hx'], p['hz']
    items = []
    for n in ns:
        dx, dz = n['x'] - px, n['z'] - pz
        d = math.hypot(dx, dz)
        bearing = math.degrees(math.atan2(dz, dx) - math.atan2(hz, hx))
        bearing = (bearing + 180) % 360 - 180
        items.append('%s:%g/%g%s d%.0f b%+.0f' % (n['id'][-4:], n['hp'], n['thr'], ('' if n['st'] in ('0402', '1402') else ' s' + n['st']), d, bearing))
    print('%6d P(%.0f,%.0f) h%.0f hp%g | %s' % (f, px, pz, math.degrees(math.atan2(hz, hx)), p['hp'], ' | '.join(items)))
for l in open(log, errors='replace'):
    m = re.match(r'\[bot\] flip (\d+) player (\w+) hp (\S+)/(\S+) pos \((\S+) (\S+) (\S+)\) h \((\S+) (\S+)\) st (\w+)(?: mode (\d+))?', l)
    if m:
        flush(); cur = None
        f = int(m.group(1))
        if lo <= f <= hi:
            cur = (f, dict(hp=float(m.group(3)), x=float(m.group(5)), z=float(m.group(7)), hx=float(m.group(8)), hz=float(m.group(9)), st=m.group(10)), [])
        continue
    m = re.match(r'\[bot\]   npc (\w+) vt (\w+) hp (\S+)/(\S+) thr (\S+) pos \((\S+) (\S+) (\S+)\) st (\w+) w328 \S+ body \((\S+) (\S+) (\S+)\)', l)
    if m and cur is not None:
        cur[2].append(dict(id=m.group(1), vt=m.group(2), hp=float(m.group(3)), thr=float(m.group(5)), x=float(m.group(10)), z=float(m.group(12)), st=m.group(9)))
flush()
