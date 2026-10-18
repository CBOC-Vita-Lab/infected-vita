#!/usr/bin/env python3
"""funcs.py : build private/dis/funcs.txt = sorted function starts (jal targets plus stack-prologue heuristics) and answer queries.
usage: funcs.py build | funcs.py find ADDR [ADDR..]  | funcs.py callers ADDR | funcs.py refs ADDR"""
import sys, re, bisect, os
DIS = '/home/claude/ws/tune/private/dis/image.txt'
XR = '/home/claude/ws/tune/private/dis/xref.tsv'
OUT = '/home/claude/ws/tune/private/dis/funcs.txt'
def build():
    starts = set()
    for l in open(XR):
        a, pc, k = l.rstrip().split('\t')
        if k == 'jal': starts.add(int(a, 16))
    prev_ret = False; prev_delay = False
    rx = re.compile(r'^([0-9a-f]{8}): (\S+)\s*(.*)$')
    last = None
    for l in open(DIS):
        m = rx.match(l)
        if not m: continue
        a = int(m.group(1), 16); op = m.group(2); args = m.group(3)
        if prev_delay:
            prev_delay = False; after_ret = True
        else:
            after_ret = False
        if op == 'jr' and args.strip() == '$ra':
            prev_delay = True
        if after_ret:
            pass
        last = (a, op, args, after_ret)
        if after_ret_flag if False else False: pass
    # prologue heuristic pass
    rx2 = rx
    ret_seen = 0
    lines = open(DIS).read().split('\n')
    for i, l in enumerate(lines):
        m = rx2.match(l)
        if not m: continue
        op = m.group(2); args = m.group(3)
        if op == 'jr' and args.strip() == '$ra':
            ret_seen = 2
            continue
        if ret_seen > 0:
            ret_seen -= 1
            if ret_seen == 0 and op == 'addiu' and re.match(r'\$sp, \$sp, -0x', args):
                starts.add(int(m.group(1), 16))
    with open(OUT, 'w') as f:
        for s in sorted(starts): f.write('%08x\n' % s)
    print(len(starts))
def load():
    return [int(x, 16) for x in open(OUT).read().split()]
if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'build': build()
    elif cmd == 'find':
        fs = load()
        for a in sys.argv[2:]:
            a = int(a, 16); i = bisect.bisect_right(fs, a) - 1
            print('%08x in fn %08x (+0x%x)' % (a, fs[i], a - fs[i]))
    elif cmd == 'callers':
        t = int(sys.argv[2], 16); fs = load()
        for l in open(XR):
            a, pc, k = l.rstrip().split('\t')
            if int(a, 16) == t:
                pc = int(pc, 16); i = bisect.bisect_right(fs, pc) - 1
                print('%08x (%s) in fn %08x' % (pc, k, fs[i]))
    elif cmd == 'refs': 
        t = int(sys.argv[2], 16)
        for l in open(XR):
            a, pc, k = l.rstrip().split('\t')
            if int(a, 16) == t: print(pc, k)
