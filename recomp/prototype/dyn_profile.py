#!/usr/bin/env python3
"""Dynamic-profile analysis: weight the lean generator's coverage and cost by the executed-PC histograms the bring-up
agent recorded under the interpreter (stats/s18/pc_f*.csv, one file per 200 flips; counts are executed guest instructions).
usage: dyn_profile.py FIRST_FLIP LAST_FLIP   (phase files whose label lies in [FIRST, LAST])"""
import bisect, collections, glob, json, os, re, sys
sys.path.insert(0, '/home/claude/assess/experiments')
import lean_gen as L
import insncount

lo, hi = int(sys.argv[1]), int(sys.argv[2])
files = []
for p in sorted(glob.glob('/home/claude/infected/stats/s18/pc_f*.csv')):
    n = int(re.search(r'pc_f(\d+)', p).group(1))
    if lo <= n <= hi: files.append((n, p))
flips = 200 * len(files)
cnt = collections.Counter()
for n, p in files:
    for line in open(p):
        a, c = line.split(',')[:2]
        try: cnt[int(a, 16)] += int(c)
        except ValueError: pass
total = sum(cnt.values())
print('phase files', len(files), 'flips', flips, 'total guest instructions', total, ' = %.0f per flip' % (total / flips))

fns, callee_args, callers = L.analyze_program(True)
starts = sorted(fns)
def owner(pc):
    i = bisect.bisect_right(starts, pc) - 1
    if i < 0: return None
    f = fns[starts[i]]
    return f if pc < f.end else None
# classify
good = set()
reason = {}
for s, f in fns.items():
    if f.unsupported:
        key = f.unsupported.split(' at ')[0].split(' %')[0]
        reason[s] = 'jump table / jr reg' if key.startswith('jr $') else ('unsupported opcode' if key.startswith('opcode') else key)
    elif f.nonconf:
        reason[s] = 'nonconforming: ' + f.nonconf[0].split(' ')[0] + ' ' + f.nonconf[0].split(' ')[1]
    else:
        good.add(s)
dyn = collections.Counter(); dyn_fn = collections.Counter(); unowned = 0
for pc, c in cnt.items():
    f = owner(pc)
    if f is None: unowned += c; continue
    dyn_fn[f.start] += c
    dyn['good' if f.start in good else reason[f.start]] += c
print('instructions in addresses outside any discovered function:', unowned)
print('--- dynamic coverage of the lean prototype (weights = executed guest instructions)')
for k, v in dyn.most_common(): print('  %-48s %6.2f%%  (%d)' % (k, 100.0 * v / total, v))

# ARM cost per function from the nomask objects
armi = {}; armb = {}
for o in sorted(glob.glob('/home/claude/assess/experiments/lean_all/lean_*_nomask.o')):
    per, _, _ = insncount.count(o, r'f_[0-9A-F]{8}')
    for name, (n, b) in per.items():
        m = re.search(r'f_([0-9A-F]{8})', name)
        if m: armi[int(m.group(1), 16)] = n; armb[int(m.group(1), 16)] = b
tot_arm = 0.0; tot_guest_cov = 0
for s in good:
    if s in dyn_fn and s in armi and len(fns[s].pcs):
        tot_arm += dyn_fn[s] * armi[s] / len(fns[s].pcs)
        tot_guest_cov += dyn_fn[s]
print('--- estimated dynamic ARM instructions for the translated part: %.3f per covered guest instruction (static density weighted by execution)' % (tot_arm / max(1, tot_guest_cov)))

# dynamic instruction mix (by guest mnemonic) of everything executed
mix = collections.Counter()
for pc, c in cnt.items():
    if L.BASE <= pc < L.END:
        mix[L.ins_at(pc)['mn']] += c
cls = collections.Counter()
LD = {'lb', 'lh', 'lw', 'lbu', 'lhu', 'lwc1'}; ST = {'sb', 'sh', 'sw', 'swc1'}
for m, c in mix.items():
    if m in LD: cls['load'] += c
    elif m in ST: cls['store'] += c
    elif m in L.CTRL: cls['branch/jump/call'] += c
    elif m in ('mult', 'multu', 'div', 'divu', 'mfhi', 'mflo', 'mthi', 'mtlo'): cls['mul/div/hilo'] += c
    elif m.endswith('.s') or m in ('mfc1', 'mtc1', 'cvt.s.w'): cls['fpu'] += c
    elif m == 'nop': cls['nop'] += c
    elif m == 'UNSUPPORTED': cls['unsupported(VFPU/lwl/ext/..)'] += c
    else: cls['alu'] += c
print('--- dynamic guest instruction mix')
for k, v in cls.most_common(): print('  %-30s %6.2f%%' % (k, 100.0 * v / total))
print('--- top 15 functions NOT translated by the prototype, by dynamic weight')
rows = [(c, s) for s, c in dyn_fn.items() if s not in good]
for c, s in sorted(rows, reverse=True)[:15]:
    print('  %08x %6.2f%%  %-28s span=%d words' % (s, 100.0 * c / total, reason[s], (fns[s].end - s) // 4))
print('--- top 12 translated functions by dynamic weight (arm insns / reachable words)')
rows = [(c, s) for s, c in dyn_fn.items() if s in good]
for c, s in sorted(rows, reverse=True)[:12]:
    print('  %08x %6.2f%%  words=%d arm_insns=%s' % (s, 100.0 * c / total, len(fns[s].pcs), armi.get(s)))
json.dump(dict(flips=flips, total=total, per_flip=total / flips, cover={k: v / total for k, v in dyn.items()}, mix={k: v / total for k, v in cls.items()}),
          open('/home/claude/assess/experiments/dyn_profile_%d_%d.json' % (lo, hi), 'w'), indent=1)
