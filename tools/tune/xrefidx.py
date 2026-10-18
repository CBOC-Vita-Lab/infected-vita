#!/usr/bin/env python3
"""xrefidx.py DIS.txt OUT.tsv : index of absolute addresses formed by lui + (addiu|ori|load|store) pairs and jal targets.
Output lines: ADDR<TAB>PC<TAB>kind   (kind = la/lw/sw/lb/.. or jal)"""
import sys, re
rx = re.compile(r'^([0-9a-f]{8}): (\S+)\s*(.*)$')
lui = {}  # reg -> (hi, pc)
out = open(sys.argv[2], 'w')
def reg(s): return s.strip()
def s16(x):
    return x - 0x10000 if x & 0x8000 else x
for l in open(sys.argv[1]):
    m = rx.match(l)
    if not m: continue
    pc = int(m.group(1), 16); op = m.group(2); args = m.group(3)
    if op == 'jal' or op == 'j':
        a = int(args.split()[0], 16)
        out.write('%08x\t%08x\t%s\n' % (a, pc, op)); continue
    if op == 'lui':
        r, imm = [x.strip() for x in args.split(',')]
        lui[r] = (int(imm, 16) << 16, pc); continue
    mm = re.match(r'(\$\w+), (\$\w+), (-?0x[0-9a-f]+)$', args)
    if op in ('addiu', 'ori', 'addi') and mm:
        rd, rs, imm = mm.groups()
        if rs in lui:
            v = int(imm, 16)
            a = (lui[rs][0] + (s16(v & 0xffff) if op != 'ori' else (v & 0xffff))) & 0xffffffff
            out.write('%08x\t%08x\t%s\n' % (a, pc, 'la'))
            lui[rd] = (a & 0xffff0000, pc) if False else lui.get(rd) if rd != rs else lui[rd]
            if rd != rs: lui.pop(rd, None)
        else:
            lui.pop(rd, None)
        continue
    mm = re.match(r'(\$\w+), (-?0x[0-9a-f]+|0x0)\((\$\w+)\)$', args)
    if mm and op in ('lw','sw','lb','lbu','lh','lhu','sb','sh','lwc1','swc1','lwl','lwr','swl','swr','lv.s','sv.s','lv.q','sv.q'):
        rt, imm, base = mm.groups()
        if base in lui:
            a = (lui[base][0] + s16(int(imm, 16) & 0xffff)) & 0xffffffff
            out.write('%08x\t%08x\t%s\n' % (a, pc, op))
        if op.startswith('l') and rt in lui and rt != base: lui.pop(rt, None)
        continue
    # any other write to a register clears it conservatively: handle common dests
    mm = re.match(r'(\$\w+),', args)
    if mm and not op.startswith(('b','sw','sb','sh','j','mt','sv','sc','c.')) and mm.group(1) in lui and op != 'lui':
        lui.pop(mm.group(1), None)
