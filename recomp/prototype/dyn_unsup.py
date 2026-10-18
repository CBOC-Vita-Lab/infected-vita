import sys, glob, re, collections
sys.path.insert(0, '/home/claude/assess/experiments')
import lean_gen as L
cnt = collections.Counter()
for p in sorted(glob.glob('/home/claude/infected/stats/s18/pc_f*.csv')):
    n = int(re.search(r'pc_f(\d+)', p).group(1))
    if 3600 <= n <= 7600:
        for line in open(p):
            a, c = line.split(',')[:2]
            try: cnt[int(a, 16)] += int(c)
            except ValueError: pass
total = sum(cnt.values())
cls = collections.Counter(); ex = {}
for pc, c in cnt.items():
    if not (L.BASE <= pc < L.END): continue
    d = L.ins_at(pc)
    if d['mn'] != 'UNSUPPORTED': continue
    op = d['op']; fn = d['fn']
    if op == 0x12: k = 'COP2: mfv/mtv/mfvc/mtvc/bvf (VFPU moves/branch)'
    elif op in (0x18, 0x19, 0x1B): k = 'VFPU0/VFPU1/VFPU3 arithmetic/compare'
    elif op in (0x34, 0x35, 0x36, 0x3C, 0x3D, 0x3E, 0x32, 0x3A): k = 'VFPU load/store (lv.q/sv.q/lv.s/sv.s/lvl/lvr)'
    elif op == 0x1F: k = 'SPECIAL3 ext/ins/seb/seh/wsbh/bitrev'
    elif op == 0 and fn in (0x16, 0x17): k = 'clz/clo'
    elif op == 0 and fn in (2, 6) and d['rs'] == 1: k = 'rotr/rotrv'
    elif op == 0 and fn == 0x0C: k = 'syscall'
    elif op == 0 and fn == 0x0D: k = 'break'
    elif op in (0x22, 0x26, 0x2A, 0x2E): k = 'lwl/lwr/swl/swr'
    elif op == 0x2F: k = 'cache'
    elif op == 0x3B or op == 0x38 or op == 0x30: k = 'll/sc/other atomics'
    elif op == 0x1C: k = 'min/max/other'
    else: k = 'other op=%02x fn=%02x' % (op, fn)
    cls[k] += c; ex.setdefault(k, pc)
for k, v in cls.most_common(): print('%6.2f%% of all executed guest instructions  %-55s e.g. %08x' % (100.0 * v / total, k, ex[k]))
