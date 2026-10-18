#!/usr/bin/env python3
"""
Prototype of the LEAN generator (experiment only; the emitted C++ is compiled for size, never run on a guest runtime).

Reads the relocated Infected image (/home/claude/infected/work/image.bin, base 0x08804000), splits it into
functions (jal targets + prologue heuristic), builds an instruction-granular CFG with delay slots, runs an
interprocedural register-liveness analysis to derive TYPED function signatures
        RET f_ADDR(sp?, int args..., float args...)
(args = registers live-in at entry among $4-$11, f12-f19, plus $sp when read), and emits lean C++:
  * guest registers are C locals (u32 / float), not ctx.gpr[] / ctx.fpr[]
  * jal -> direct C call, jr $ra -> return, delay slots folded in, branch-likely handled
  * memory = unchecked base-pointer access (BIAS + addr), optional mask (-DLEAN_MASK)
  * prologue/epilogue save/restore of $s0-$s7/$fp/$ra elided when the slot is only touched by that pair
  * jalr -> flat table dispatch (g_fn_tbl)
Functions the prototype cannot express (unsupported opcode, jr via register i.e. jump table, tail jumps outside
the function, ABI non-conforming) are *reported and skipped*, never silently translated.

usage: lean_gen.py stats                      -> analysis statistics (json)
       lean_gen.py emit OUTDIR [--nelide] [--maxfn N]   -> writes lean_*.cpp TUs + manifest.json
"""
import struct, sys, collections, json, os

BASE = 0x08804000
IMG = open('/home/claude/infected/work/image.bin', 'rb').read()
NW = 0x2860D4 // 4
W = struct.unpack('<%dI' % NW, IMG[:NW * 4])
END = BASE + NW * 4
HI, LO, CC = 65, 66, 64
def F(n): return 32 + n
def s16(x): return x - 0x10000 if x & 0x8000 else x

# ------------------------------------------------------------------ decode
def decode(w):
    op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31; rd = (w >> 11) & 31; sa = (w >> 6) & 31; fn = w & 63
    d = dict(w=w, op=op, rs=rs, rt=rt, rd=rd, sa=sa, fn=fn, imm=w & 0xFFFF, simm=s16(w & 0xFFFF), tgt=(w & 0x3FFFFFF) << 2)
    mn = None
    if w == 0: mn = 'nop'
    elif op == 0:
        mn = {0: 'sll', 2: 'srl', 3: 'sra', 4: 'sllv', 6: 'srlv', 7: 'srav', 8: 'jr', 9: 'jalr', 10: 'movz', 11: 'movn',
              15: 'sync', 16: 'mfhi', 17: 'mthi', 18: 'mflo', 19: 'mtlo', 24: 'mult', 25: 'multu', 26: 'div', 27: 'divu',
              32: 'add', 33: 'addu', 34: 'sub', 35: 'subu', 36: 'and', 37: 'or', 38: 'xor', 39: 'nor', 42: 'slt',
              43: 'sltu', 44: 'max', 45: 'min'}.get(fn)
    elif op == 1:
        mn = {0: 'bltz', 1: 'bgez', 2: 'bltzl', 3: 'bgezl'}.get(rt)
    elif op == 0x11:
        if rs == 0: mn = 'mfc1'
        elif rs == 4: mn = 'mtc1'
        elif rs == 8: mn = {0: 'bc1f', 1: 'bc1t', 2: 'bc1fl', 3: 'bc1tl'}.get(rt)
        elif rs == 16:
            mn = {0: 'add.s', 1: 'sub.s', 2: 'mul.s', 3: 'div.s', 4: 'sqrt.s', 5: 'abs.s', 6: 'mov.s', 7: 'neg.s',
                  13: 'trunc.w.s', 36: 'cvt.w.s', 48: 'c.f', 49: 'c.un', 50: 'c.eq', 51: 'c.ueq', 52: 'c.olt', 53: 'c.ult',
                  54: 'c.ole', 55: 'c.ule', 60: 'c.lt', 62: 'c.le'}.get(fn)
        elif rs == 20 and fn == 32: mn = 'cvt.s.w'
    else:
        mn = {2: 'j', 3: 'jal', 4: 'beq', 5: 'bne', 6: 'blez', 7: 'bgtz', 8: 'addi', 9: 'addiu', 10: 'slti', 11: 'sltiu',
              12: 'andi', 13: 'ori', 14: 'xori', 15: 'lui', 20: 'beql', 21: 'bnel', 22: 'blezl', 23: 'bgtzl',
              32: 'lb', 33: 'lh', 35: 'lw', 36: 'lbu', 37: 'lhu', 40: 'sb', 41: 'sh', 43: 'sw', 49: 'lwc1', 57: 'swc1'}.get(op)
    d['mn'] = mn or 'UNSUPPORTED'
    return d

INS = {}
def ins_at(pc):
    d = INS.get(pc)
    if d is None:
        d = INS[pc] = decode(W[(pc - BASE) >> 2])
    return d

BR_COND = {'beq', 'bne', 'blez', 'bgtz', 'bltz', 'bgez', 'bc1f', 'bc1t'}
BR_LIKELY = {'beql', 'bnel', 'blezl', 'bgtzl', 'bltzl', 'bgezl', 'bc1fl', 'bc1tl'}
CTRL = BR_COND | BR_LIKELY | {'j', 'jal', 'jr', 'jalr'}

def usedef(d):
    """(uses, defs) of a non-control instruction; None if unsupported."""
    m = d['mn']; rs, rt, rd, sa = d['rs'], d['rt'], d['rd'], d['sa']
    if m in ('nop', 'sync'): return set(), set()
    if m in ('sll', 'srl', 'sra'): return {rt}, {rd}
    if m in ('sllv', 'srlv', 'srav'): return {rt, rs}, {rd}
    if m in ('movz', 'movn'): return {rs, rt, rd}, {rd}
    if m in ('add', 'addu', 'sub', 'subu', 'and', 'or', 'xor', 'nor', 'slt', 'sltu', 'max', 'min'): return {rs, rt}, {rd}
    if m in ('addi', 'addiu', 'slti', 'sltiu', 'andi', 'ori', 'xori'): return {rs}, {rt}
    if m == 'lui': return set(), {rt}
    if m in ('lb', 'lh', 'lw', 'lbu', 'lhu'): return {rs}, {rt}
    if m in ('sb', 'sh', 'sw'): return {rs, rt}, set()
    if m == 'lwc1': return {rs}, {F(rt)}
    if m == 'swc1': return {rs, F(rt)}, set()
    if m in ('mult', 'multu', 'div', 'divu'): return {rs, rt}, {HI, LO}
    if m == 'mfhi': return {HI}, {rd}
    if m == 'mflo': return {LO}, {rd}
    if m == 'mthi': return {rs}, {HI}
    if m == 'mtlo': return {rs}, {LO}
    if m == 'mfc1': return {F(rd)}, {rt}
    if m == 'mtc1': return {rt}, {F(rd)}
    # COP1 S-format: ft=rt, fs=rd, fd=sa
    if m in ('add.s', 'sub.s', 'mul.s', 'div.s'): return {F(rd), F(rt)}, {F(sa)}
    if m in ('sqrt.s', 'abs.s', 'mov.s', 'neg.s', 'trunc.w.s', 'cvt.w.s', 'cvt.s.w'): return {F(rd)}, {F(sa)}
    if m.startswith('c.'): return {F(rd), F(rt)}, {CC}
    return None

def ud(d):
    r = usedef(d)
    if r is None: return None
    u, df = r
    return {x for x in u if x != 0}, {x for x in df if x != 0}

def branch_target(pc, d): return pc + 4 + (d['simm'] << 2)

def branch_const(d):
    """True/False when the branch outcome is statically known (b = beq $z,$z, etc.), else None."""
    m = d['mn'].rstrip('l') if d['mn'] in BR_LIKELY else d['mn']
    rs, rt = d['rs'], d['rt']
    if m == 'beq' and rs == rt: return True
    if m == 'bne' and rs == rt: return False
    if rs == 0 and m in ('blez', 'bgez'): return True
    if rs == 0 and m in ('bgtz', 'bltz'): return False
    return None

# ------------------------------------------------------------------ function discovery
def plausible_start(i):
    """word index i follows a 'jr $ra' + delay slot (+ nop padding)."""
    j = i - 1
    while j > 0 and W[j] == 0: j -= 1          # skip nop padding
    return j >= 1 and (W[j - 1] == 0x03E00008 or W[j] == 0x03E00008 or (W[j - 1] >> 26) == 2)

def find_seeds():
    seeds = {BASE}
    for w in W:
        if (w >> 26) == 3:
            t = (w & 0x3FFFFFF) << 2
            if BASE <= t < END: seeds.add(t)
    for i in range(2, NW - 2):
        w = W[i]
        if (w >> 26) == 9 and ((w >> 16) & 31) == 29 and ((w >> 21) & 31) == 29 and (w & 0x8000):
            if W[i - 2] == 0x03E00008 or W[i - 1] == 0x03E00008:
                seeds.add(BASE + i * 4)
    # code pointers stored in the relocated data segment (vtables, callbacks)
    for off in range(NW * 4, min(len(IMG), 0x2D1AF8 + 0x2664) - 3, 4):
        v = struct.unpack_from('<I', IMG, off)[0]
        if BASE <= v < END and v % 4 == 0 and plausible_start((v - BASE) >> 2): seeds.add(v)
    # lui/addiu or lui/ori constants that form a code address
    lui = {}
    for i, w in enumerate(W):
        op = w >> 26
        if op == 0x0F: lui[(w >> 16) & 31] = (w & 0xFFFF) << 16; continue
        if op in (9, 13):
            rs = (w >> 21) & 31
            if rs in lui:
                v = (lui[rs] + (s16(w & 0xFFFF) if op == 9 else (w & 0xFFFF))) & 0xFFFFFFFF
                if BASE <= v < END and v % 4 == 0 and plausible_start((v - BASE) >> 2): seeds.add(v)
        if op in (0x23, 0x2B, 4, 5, 3, 2) : pass
    return sorted(seeds)

class Fn:
    def __init__(s, start, end):
        s.start, s.end = start, end
        s.unsupported = None
        s.nodes = {}; s.order = []; s.pcs = []
        s.elide = set(); s.elide_regs = set()
        s.nonconf = []
        s.args = set()
        s.calls = []      # (nodeid, target or None)

CALLER_SAVED = set(range(1, 16)) | {24, 25} | {F(i) for i in range(0, 20)} | {HI, LO, CC}
CALLEE_SAVED = set(range(16, 24)) | {30} | {F(i) for i in range(20, 32)}
ARG_INT = list(range(4, 12)); ARG_FP = [F(i) for i in range(12, 20)]
ARG_MASK = set(ARG_INT) | set(ARG_FP) | {29}

def build_cfg(fn):
    start, end = fn.start, fn.end
    nodes = fn.nodes
    work = [start]; seen = set()
    def bad(msg): fn.unsupported = msg; return False
    while work:
        pc = work.pop()
        if pc in seen: continue
        seen.add(pc)
        if not (start <= pc < end): return bad('flow leaves function at %08x' % pc)
        d = ins_at(pc); m = d['mn']
        if m == 'UNSUPPORTED': return bad('opcode %08x at %08x' % (d['w'], pc))
        node = dict(pc=pc, d=d, succ=[], kind='n')
        nodes[pc] = node
        if m in CTRL:
            sp = pc + 4
            if not (start <= sp < end): return bad('delay slot outside')
            sd = ins_at(sp)
            if sd['mn'] == 'UNSUPPORTED': return bad('opcode %08x at %08x' % (sd['w'], sp))
            if sd['mn'] in CTRL: return bad('branch in delay slot at %08x' % pc)
            node['kind'] = m
            S = ('S', pc)
            if m in BR_LIKELY:
                t = branch_target(pc, d); node['target'] = t
                L = ('L', pc); bc = branch_const(d); node['const'] = bc
                nodes[L] = dict(pc=sp, d=sd, succ=[t], kind='slot')
                if bc is True: node['succ'] = [L]; work.append(t)
                elif bc is False: node['succ'] = [pc + 8]; work.append(pc + 8)
                else: node['succ'] = [L, pc + 8]; work += [t, pc + 8]
            elif m in BR_COND:
                t = branch_target(pc, d); node['target'] = t
                bc = branch_const(d); node['const'] = bc
                succ = [t, pc + 8] if bc is None else ([t] if bc else [pc + 8])
                nodes[S] = dict(pc=sp, d=sd, succ=succ, kind='slot')
                node['succ'] = [S]
                work += succ
            elif m == 'j':
                t = (pc & 0xF0000000) | d['tgt']; node['target'] = t
                nodes[S] = dict(pc=sp, d=sd, succ=[t], kind='slot')
                node['succ'] = [S]; work.append(t)
            elif m in ('jal', 'jalr'):
                C = ('C', pc)
                if m == 'jal': node['target'] = (pc & 0xF0000000) | d['tgt']
                nodes[S] = dict(pc=sp, d=sd, succ=[C], kind='slot')
                nodes[C] = dict(pc=pc, d=d, succ=[pc + 8], kind='call', target=node.get('target'))
                node['succ'] = [S]; work.append(pc + 8)
                fn.calls.append(C)
            elif m == 'jr':
                if d['rs'] != 31: return bad('jr $%d (jump table / tail jump) at %08x' % (d['rs'], pc))
                nodes[S] = dict(pc=sp, d=sd, succ=[], kind='slot', ret=True)
                node['succ'] = [S]
        else:
            node['succ'] = [pc + 4]; work.append(pc + 4)
    fn.pcs = sorted(p for p in nodes if isinstance(p, int))
    return True

# ------------------------------------------------------------------ forward state for jalr argument discovery
def jalr_arg_uses(fn):
    """For each jalr call node: which of $4-$7 carry a meaningful value (entry value passed through, or defined locally
    since the last call), as opposed to garbage left over after a call."""
    nodes = fn.nodes
    ARGR = {4, 5, 6, 7}
    st = {k: None for k in nodes}
    st[fn.start] = (frozenset(ARGR), frozenset(), frozenset())
    work = [fn.start]
    def apply(k, state):
        E, D, C = state
        n = nodes[k]
        if is_el(fn, k): return state
        if n['kind'] == 'call':
            return (frozenset(), frozenset(), frozenset(ARGR))
        if n['kind'] in ('n', 'slot'):
            r = ud(n['d'])
            if r:
                for x in r[1] & ARGR:
                    E = E - {x}; C = C - {x}; D = D | {x}
        return (E, D, C)
    out_of = {}
    while work:
        k = work.pop()
        ins = st[k]
        outs = apply(k, ins)
        for sidx in nodes[k]['succ']:
            cur = st[sidx]
            new = outs if cur is None else (cur[0] | outs[0], cur[1] | outs[1], cur[2] | outs[2])
            if new != cur:
                st[sidx] = new; work.append(sidx)
    fn.jalr_uses = {}
    for k, n in nodes.items():
        if n['kind'] == 'call' and n['d']['mn'] == 'jalr' and st[k] is not None:
            E, D, C = st[k]
            fn.jalr_uses[k] = set(E | D)
        elif n['kind'] == 'call' and n['d']['mn'] == 'jalr':
            fn.jalr_uses[k] = set()

# ------------------------------------------------------------------ save/restore elision
def is_el(fn, nid):
    n = fn.nodes[nid]
    return n['kind'] in ('n', 'slot') and n['pc'] in fn.elide

def find_elisions(fn):
    """Elide `sw $s_i/$ra, off($sp)` prologue saves and their `lw` epilogue restores (by instruction address)."""
    nodes = fn.nodes
    acc = {}                                  # ipc -> (off,size,mn,reg)
    first_def = {}
    for nid, n in nodes.items():
        if n['kind'] not in ('n', 'slot'): continue
        dd = n['d']; mm = dd['mn']; ipc = n['pc']
        if dd['rs'] == 29 and mm in ('lw', 'sw', 'lb', 'lbu', 'sb', 'lh', 'lhu', 'sh', 'lwc1', 'swc1'):
            size = 4 if mm in ('lw', 'sw', 'lwc1', 'swc1') else (2 if mm in ('lh', 'lhu', 'sh') else 1)
            acc[ipc] = (dd['simm'], size, mm, dd['rt'])
        r = ud(dd)
        if r:
            for x in r[1]: first_def[x] = min(first_def.get(x, 1 << 40), ipc)
    for nid, n in nodes.items():
        if n['kind'] in ('jal', 'jalr'): first_def[31] = min(first_def.get(31, 1 << 40), n['pc'])
    cands = collections.defaultdict(list)
    for ipc, (off, size, mm, reg) in acc.items():
        if mm == 'sw' and (reg in CALLEE_SAVED or reg == 31): key = reg
        elif mm == 'swc1' and F(reg) in CALLEE_SAVED: key = F(reg)
        else: continue
        if first_def.get(key, 1 << 40) >= ipc: cands[(off, key)].append(ipc)
    for (off, key), sts in cands.items():
        if len(sts) != 1: continue
        ok = True; restores = []
        for ipc, (o2, size, mm, r2) in acc.items():
            if o2 + size <= off or o2 >= off + 4 or ipc == sts[0]: continue
            if ((mm == 'lw' and r2 == key) or (mm == 'lwc1' and F(r2) == key)) and o2 == off: restores.append(ipc); continue
            ok = False; break
        if ok and restores:
            fn.elide.add(sts[0]); fn.elide.update(restores); fn.elide_regs.add(key)

# ------------------------------------------------------------------ liveness
STRICT = [False]
def node_ud(fn, nid, callee_args):
    n = fn.nodes[nid]; d = n['d']; k = n['kind']
    if is_el(fn, nid): return set(), set()
    if k == 'call' and STRICT[0]:
        return {29}, set(CALLER_SAVED)
    if k == 'call':
        if d['mn'] == 'jal':
            a = callee_args.get(n['target'])
            if a is None: a = {29, 4, 5, 6, 7}
            return set(a), set(CALLER_SAVED)
        return {29} | fn.jalr_uses.get(nid, set()), set(CALLER_SAVED)
    if k in ('n', 'slot'):
        r = ud(d); return r if r is not None else (set(), set())
    # control instruction node (operand reads happen here)
    m = d['mn']
    if m in ('beq', 'bne', 'beql', 'bnel'): return {d['rs'], d['rt']} - {0}, set()
    if m in ('blez', 'bgtz', 'bltz', 'bgez', 'blezl', 'bgtzl', 'bltzl', 'bgezl'): return {d['rs']} - {0}, set()
    if m in ('bc1t', 'bc1f', 'bc1tl', 'bc1fl'): return {CC}, set()
    if m == 'jalr': return {d['rs']} - {0}, set()
    return set(), set()

def liveness(fn, callee_args):
    nodes = fn.nodes
    cache = {k: node_ud(fn, k, callee_args) for k in nodes}
    live = {k: set() for k in nodes}
    # iterate in reverse address order until fixpoint
    keys = sorted(nodes.keys(), key=lambda k: (k[1] if isinstance(k, tuple) else k, 0 if isinstance(k, tuple) else 1), reverse=True)
    changed = True
    while changed:
        changed = False
        for k in keys:
            out = set()
            for s in nodes[k]['succ']: out |= live[s]
            u, df = cache[k]
            new = (out - df) | u
            if new != live[k]: live[k] = new; changed = True
    fn.live = live; fn.udc = cache
    return live[fn.start]

# ------------------------------------------------------------------ emission helpers
def R(n): return '0u' if n == 0 else 'r%d' % n
def FR(n): return 'f%d' % n
def addr(base, off):
    b = R(base)
    if off == 0: return b
    if base == 0: return '0x%08Xu' % (off & 0xFFFFFFFF)
    return '(%s + %du)' % (b, off) if off > 0 else '(%s - %du)' % (b, -off)

def emit_simple(d):
    """C statement(s) for one non-control instruction (None for no-op)."""
    m = d['mn']; rs, rt, rd, sa = d['rs'], d['rt'], d['rd'], d['sa']
    s, t = R(rs), R(rt)
    def asg(reg, expr): return None if reg == 0 else '%s = %s;' % (R(reg), expr)
    if m in ('nop', 'sync'): return None
    if m == 'sll': return asg(rd, '%s << %d' % (t, sa))
    if m == 'srl': return asg(rd, '%s >> %d' % (t, sa))
    if m == 'sra': return asg(rd, '(u32)((s32)%s >> %d)' % (t, sa))
    if m == 'sllv': return asg(rd, '%s << (%s & 31)' % (t, s))
    if m == 'srlv': return asg(rd, '%s >> (%s & 31)' % (t, s))
    if m == 'srav': return asg(rd, '(u32)((s32)%s >> (%s & 31))' % (t, s))
    if m == 'movz': return None if rd == 0 else 'if (%s == 0) %s = %s;' % (t, R(rd), s)
    if m == 'movn': return None if rd == 0 else 'if (%s != 0) %s = %s;' % (t, R(rd), s)
    if m in ('add', 'addu'): return asg(rd, '%s + %s' % (s, t))
    if m in ('sub', 'subu'): return asg(rd, '%s - %s' % (s, t))
    if m == 'and': return asg(rd, '%s & %s' % (s, t))
    if m == 'or': return asg(rd, '%s | %s' % (s, t))
    if m == 'xor': return asg(rd, '%s ^ %s' % (s, t))
    if m == 'nor': return asg(rd, '~(%s | %s)' % (s, t))
    if m == 'slt': return asg(rd, '(u32)((s32)%s < (s32)%s)' % (s, t))
    if m == 'sltu': return asg(rd, '(u32)(%s < %s)' % (s, t))
    if m == 'max': return asg(rd, '(u32)MAXS((s32)%s, (s32)%s)' % (s, t))
    if m == 'min': return asg(rd, '(u32)MINS((s32)%s, (s32)%s)' % (s, t))
    if m in ('addi', 'addiu'): return asg(rt, '%s + %du' % (s, d['simm']) if d['simm'] >= 0 else '%s - %du' % (s, -d['simm']))
    if m == 'slti': return asg(rt, '(u32)((s32)%s < %d)' % (s, d['simm']))
    if m == 'sltiu': return asg(rt, '(u32)(%s < 0x%Xu)' % (s, d['simm'] & 0xFFFFFFFF))
    if m == 'andi': return asg(rt, '%s & 0x%Xu' % (s, d['imm']))
    if m == 'ori': return asg(rt, '%s | 0x%Xu' % (s, d['imm']))
    if m == 'xori': return asg(rt, '%s ^ 0x%Xu' % (s, d['imm']))
    if m == 'lui': return asg(rt, '0x%08Xu' % (d['imm'] << 16))
    a = addr(rs, d['simm'])
    if m == 'lw': return asg(rt, 'LW(%s)' % a)
    if m == 'lh': return asg(rt, '(u32)LH(%s)' % a)
    if m == 'lhu': return asg(rt, 'LHU(%s)' % a)
    if m == 'lb': return asg(rt, '(u32)LB(%s)' % a)
    if m == 'lbu': return asg(rt, 'LBU(%s)' % a)
    if m == 'sw': return 'SW(%s, %s);' % (a, t)
    if m == 'sh': return 'SH(%s, %s);' % (a, t)
    if m == 'sb': return 'SB(%s, %s);' % (a, t)
    if m == 'lwc1': return '%s = LWF(%s);' % (FR(rt), a)
    if m == 'swc1': return 'SWF(%s, %s);' % (a, FR(rt))
    if m == 'mult': return '{ s64 p_ = (s64)(s32)%s * (s32)%s; lo = (u32)p_; hi = (u32)(p_ >> 32); }' % (s, t)
    if m == 'multu': return '{ u64 p_ = (u64)%s * %s; lo = (u32)p_; hi = (u32)(p_ >> 32); }' % (s, t)
    if m == 'div': return 'DIV_S(%s, %s, lo, hi);' % (s, t)
    if m == 'divu': return 'DIV_U(%s, %s, lo, hi);' % (s, t)
    if m == 'mfhi': return asg(rd, 'hi')
    if m == 'mflo': return asg(rd, 'lo')
    if m == 'mthi': return 'hi = %s;' % s
    if m == 'mtlo': return 'lo = %s;' % s
    if m == 'mfc1': return asg(rt, 'BC_U(%s)' % FR(rd))
    if m == 'mtc1': return '%s = BC_F(%s);' % (FR(rd), t)
    fs, ft, fd = FR(rd), FR(rt), FR(sa)
    if m == 'add.s': return '%s = %s + %s;' % (fd, fs, ft)
    if m == 'sub.s': return '%s = %s - %s;' % (fd, fs, ft)
    if m == 'mul.s': return '%s = %s * %s;' % (fd, fs, ft)
    if m == 'div.s': return '%s = %s / %s;' % (fd, fs, ft)
    if m == 'sqrt.s': return '%s = SQRTF(%s);' % (fd, fs)
    if m == 'abs.s': return '%s = FABSF(%s);' % (fd, fs)
    if m == 'mov.s': return '%s = %s;' % (fd, fs)
    if m == 'neg.s': return '%s = -%s;' % (fd, fs)
    if m == 'trunc.w.s': return '%s = BC_F(TRUNC_W(%s));' % (fd, fs)
    if m == 'cvt.w.s': return '%s = BC_F(CVT_W(%s));' % (fd, fs)
    if m == 'cvt.s.w': return '%s = (float)(s32)BC_U(%s);' % (fd, fs)
    cmp = {'c.f': '0', 'c.un': '(%s != %s || %s != %s)' % (fs, fs, ft, ft), 'c.eq': '%s == %s' % (fs, ft),
           'c.ueq': '!(%s < %s || %s > %s)' % (fs, ft, fs, ft), 'c.olt': '%s < %s' % (fs, ft), 'c.ult': '!(%s >= %s)' % (fs, ft),
           'c.ole': '%s <= %s' % (fs, ft), 'c.ule': '!(%s > %s)' % (fs, ft), 'c.lt': '%s < %s' % (fs, ft), 'c.le': '%s <= %s' % (fs, ft)}
    if m in cmp: return 'cc = %s;' % cmp[m]
    raise ValueError(m)

def cond_expr(d):
    m = d['mn'].rstrip('l') if d['mn'] in BR_LIKELY else d['mn']
    s, t = R(d['rs']), R(d['rt'])
    if m == 'beq': return '%s == %s' % (s, t)
    if m == 'bne': return '%s != %s' % (s, t)
    if m == 'blez': return '(s32)%s <= 0' % s
    if m == 'bgtz': return '(s32)%s > 0' % s
    if m == 'bltz': return '(s32)%s < 0' % s
    if m == 'bgez': return '(s32)%s >= 0' % s
    if m == 'bc1t': return 'cc'
    if m == 'bc1f': return '!cc'
    raise ValueError(m)

# ------------------------------------------------------------------ whole-program analysis
def analyze_program(use_elision=True, verbose=False):
    seeds = find_seeds()
    fns = {}
    for i, s in enumerate(seeds):
        e = seeds[i + 1] if i + 1 < len(seeds) else END
        fn = Fn(s, e)
        fns[s] = fn
        if build_cfg(fn):
            if use_elision: find_elisions(fn)
            jalr_arg_uses(fn)
    ok = [f for f in fns.values() if not f.unsupported]
    callee_args = {}
    callers = collections.defaultdict(set)
    for f in ok:
        for C in f.calls:
            t = f.nodes[C].get('target')
            if t is not None: callers[t].add(f.start)
    # worklist interprocedural live-in(args)
    work = collections.deque(f.start for f in ok)
    inq = set(work)
    it = 0
    while work:
        s = work.popleft(); inq.discard(s); it += 1
        f = fns[s]
        li = liveness(f, callee_args)
        a = li & ARG_MASK
        if callee_args.get(s) != a:
            callee_args[s] = a
            for c in callers.get(s, ()):
                if c not in inq and not fns[c].unsupported: work.append(c); inq.add(c)
    # final liveness with converged args, plus conformance checks
    for f in ok:
        li = liveness(f, callee_args)
        f.args = li & ARG_MASK
        STRICT[0] = True
        li2 = liveness(f, callee_args)
        STRICT[0] = False
        liveness(f, callee_args)
        extra = li2 - ARG_MASK - {31}
        if extra: f.nonconf.append('reads reg(s) %s before def' % sorted(extra))
        # callee-saved regs defined without elided save/restore
        defd = set()
        for k, n in f.nodes.items():
            if is_el(f, k): continue
            if n['kind'] in ('n', 'slot'):
                r = ud(n['d'])
                if r: defd |= r[1]
        bad = (defd & CALLEE_SAVED) - f.elide_regs
        # a callee-saved reg that is defined and also restored is fine only if elided pair exists
        if bad: f.nonconf.append('clobbers callee-saved %s' % sorted(bad))
        # ra read other than jr/save?
        for k, n in f.nodes.items():
            if n['kind'] not in ('n', 'slot') or is_el(f, k): continue
            r = ud(n['d'])
            if r and 31 in r[0]: f.nonconf.append('reads $ra'); break
    # strict liveness: argument pass-through to callees is not a 'read' (stale/garbage args are harmless)
    STRICT[0] = True
    for f in ok:
        liveness(f, callee_args)
        f.live_strict = f.live
    STRICT[0] = False
    for f in ok:
        liveness(f, callee_args)          # restore normal liveness (used by emission/ret computation)
    # needed return regs from direct call sites; call-site clobber violations
    needed = collections.defaultdict(set)
    viol = collections.Counter()
    for f in ok:
        for C in f.calls:
            n = f.nodes[C]
            after = f.live_strict[n['succ'][0]]
            t = n.get('target')
            if n['d']['mn'] == 'jal' and t in fns:
                needed[t] |= (f.live[n['succ'][0]] & {2, 3, F(0)})
                bad = (after & CALLER_SAVED) - {2, 3, F(0)}
                if bad: f.nonconf.append('reads clobbered %s after call %08x' % (sorted(bad), n['pc']))
            else:
                bad = (after & CALLER_SAVED) - {2, 3}
                if bad: f.nonconf.append('reads clobbered %s after jalr %08x' % (sorted(bad), n['pc']))
    for f in fns.values():
        if f.unsupported: f.ret = set(needed[f.start]) if needed[f.start] else set()
    for f in ok:
        t = f.start
        if t in callers and callers[t]:
            f.ret = needed[t]
        else:
            r = set()
            for k, n in f.nodes.items():
                if n['kind'] in ('n', 'slot') and not is_el(f, k):
                    q = ud(n['d'])
                    if q: r |= q[1] & {2, 3, F(0)}
            f.ret = r & {2, F(0)} if r else set()
        if len(f.ret) == 3: f.nonconf.append('returns v0,v1,f0')
    return fns, callee_args, callers

# ------------------------------------------------------------------ signature / emission
def ret_type(ret):
    if not ret: return 'void'
    if ret == {2}: return 'u32'
    if ret == {F(0)}: return 'float'
    return 'u64'

def fname(a): return 'f_%08X' % a

def params(args, with_names=True):
    ps = []
    if 29 in args: ps.append('u32 r29' if with_names else 'u32')
    for r in ARG_INT:
        if r in args: ps.append('u32 r%d' % r if with_names else 'u32')
    for r in ARG_FP:
        if r in args: ps.append('float f%d' % (r - 32) if with_names else 'float')
    return ps

# ------------------------------------------------------------------ signature / emission
def ret_type(ret):
    if not ret: return 'void'
    if ret == {2}: return 'u32'
    if ret == {F(0)}: return 'float'
    return 'u64'

def fname(a): return 'f_%08X' % a

def params(args, names=True):
    ps = []
    if 29 in args: ps.append('u32 r29' if names else 'u32')
    for r in ARG_INT:
        if r in args: ps.append('u32 r%d' % r if names else 'u32')
    for r in ARG_FP:
        if r in args: ps.append('float f%d' % (r - 32) if names else 'float')
    return ps

def branch_uses(d):
    m = d['mn']
    if m in ('beq', 'bne', 'beql', 'bnel'): return {d['rs'], d['rt']} - {0}
    if m in ('blez', 'bgtz', 'bltz', 'bgez', 'blezl', 'bgtzl', 'bltzl', 'bgezl'): return {d['rs']} - {0}
    return {CC} if m.startswith('bc1') else set()

def emit_function(fn, fns, callee_args, protos):
    nodes = fn.nodes; pcs = fn.pcs
    # ---- pass 1: labels
    labels = set()
    nxt_of = {}
    for idx, pc in enumerate(pcs):
        n = nodes[pc]; k = n['kind']
        if (k in BR_COND or k in BR_LIKELY) and n.get('const') is not False: labels.add(n['target'])
        if k == 'j': labels.add(n['target'])
        if (k in BR_COND or k in BR_LIKELY) and n.get('const') is True: nxt = None
        elif k in ('jal', 'jalr') or k in BR_COND or k in BR_LIKELY: nxt = pc + 8
        elif k in ('j', 'jr'): nxt = None
        else: nxt = pc + 4
        nxt_of[pc] = nxt
        if nxt is not None:
            following = pcs[idx + 1] if idx + 1 < len(pcs) else None
            if following != nxt: labels.add(nxt)
    body = []
    def ex(stmt):
        if stmt: body.append('  ' + stmt)
    ret = fn.ret
    def ret_stmt():
        if not ret: return 'return;'
        if ret == {2}: return 'return r2;'
        if ret == {F(0)}: return 'return f0;'
        parts = []
        if 2 in ret: parts.append('(u64)r2')
        if F(0) in ret: parts.append('((u64)BC_U(f0) << 32)')
        if 3 in ret: parts.append('((u64)r3 << 32)')
        return 'return %s;' % ' | '.join(parts)
    def slot(nid):
        if is_el(fn, nid): return None
        return emit_simple(nodes[nid]['d'])
    for idx, pc in enumerate(pcs):
        n = nodes[pc]; d = n['d']; k = n['kind']
        if pc in labels: body.append('L_%08X:' % pc)
        if k == 'n':
            if not is_el(fn, pc): ex(emit_simple(d))
        elif (k in BR_COND or k in BR_LIKELY) and n.get('const') is not None:
            sl = ('S', pc) if k in BR_COND else ('L', pc)
            if n['const']:
                ex(slot(sl)); ex('goto L_%08X;' % n['target'])
            elif k in BR_COND:
                ex(slot(sl))
        elif k in BR_COND:
            c = cond_expr(d); sstmt = slot(('S', pc)); sd = nodes[('S', pc)]['d']
            if not sstmt: ex('if (%s) goto L_%08X;' % (c, n['target']))
            elif ud(sd)[1] & branch_uses(d):
                ex('{ bool c_ = %s; %s if (c_) goto L_%08X; }' % (c, sstmt, n['target']))
            else:
                ex(sstmt); ex('if (%s) goto L_%08X;' % (c, n['target']))
        elif k in BR_LIKELY:
            c = cond_expr(d); sstmt = slot(('L', pc))
            if sstmt: ex('if (%s) { %s goto L_%08X; }' % (c, sstmt, n['target']))
            else: ex('if (%s) goto L_%08X;' % (c, n['target']))
        elif k == 'j':
            ex(slot(('S', pc))); ex('goto L_%08X;' % n['target'])
        elif k == 'jr':
            ex(slot(('S', pc))); ex(ret_stmt())
        elif k in ('jal', 'jalr'):
            if k == 'jalr': ex('{ u32 t_ = %s;' % R(d['rs']))
            ex(slot(('S', pc)))
            if k == 'jal':
                t = n['target']; cf = fns.get(t)
                if cf is not None and not cf.unsupported and not cf.nonconf:
                    a = callee_args.get(t, set()); rt_ = cf.ret
                else:
                    a = callee_args.get(t, {29, 4, 5, 6, 7}) if cf is not None else {29, 4, 5, 6, 7}
                    rt_ = cf.ret if (cf is not None and len(cf.ret) < 3) else {2}
                protos[t] = (a, rt_)
                args = []
                if 29 in a: args.append('r29')
                for r in ARG_INT:
                    if r in a: args.append(R(r))
                for r in ARG_FP:
                    if r in a: args.append(FR(r - 32))
                call = '%s(%s)' % (fname(t), ', '.join(args))
                rtn = ret_type(rt_)
                if rtn == 'void': ex(call + ';')
                elif rtn == 'u32': ex('r2 = %s;' % call)
                elif rtn == 'float': ex('f0 = %s;' % call)
                else:
                    s_ = '{ u64 q_ = %s;' % call
                    if 2 in rt_: s_ += ' r2 = (u32)q_;'
                    if 3 in rt_: s_ += ' r3 = (u32)(q_ >> 32);'
                    if F(0) in rt_: s_ += ' f0 = BC_F((u32)(q_ >> 32));'
                    ex(s_ + ' }')
            else:
                ju = fn.jalr_uses.get(('C', pc), set())
                a4 = ['r29'] + [R(r) if r in ju else '0u' for r in (4, 5, 6, 7)]
                ex('{ u64 q_ = CALL_IND(t_, %s); r2 = (u32)q_; r3 = (u32)(q_ >> 32); } }' % ', '.join(a4))
        nxt = nxt_of[pc]
        if nxt is not None:
            following = pcs[idx + 1] if idx + 1 < len(pcs) else None
            if following != nxt: ex('goto L_%08X;' % nxt)
    # ---- locals
    text = '\n'.join(body)
    import re
    used_r = sorted({int(x) for x in re.findall(r'\br(\d+)\b', text)} - set(int(a[1:]) for a in params(fn.args) if a.startswith('u32 r') for a in [a.split()[1]]))
    used_f = sorted({int(x) for x in re.findall(r'\bf(\d+)\b', text)} - {int(a.split()[1][1:]) for a in params(fn.args) if a.startswith('float')})
    decl = []
    if used_r: decl.append('  u32 ' + ', '.join('r%d = 0' % r for r in used_r) + ';')
    if used_f: decl.append('  float ' + ', '.join('f%d = 0.0f' % r for r in used_f) + ';')
    if 'cc' in text: decl.append('  bool cc = false;')
    if re.search(r'\bhi\b', text): decl.append('  u32 hi = 0, lo = 0;')
    sig = '%s %s(%s)' % (ret_type(ret), fname(fn.start), ', '.join(params(fn.args)))
    return sig + ' {\n' + '\n'.join(decl) + '\n' + text + '\n}\n'

HEADER = r'''// lean generator experiment output (C++20, Cortex-A9 target); generated by experiments/lean_gen.py
#include "lean.h"
'''

def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else 'stats'
    use_el = '--nelide' not in sys.argv
    fns, callee_args, callers = analyze_program(use_el)
    allf = list(fns.values())
    tot_words = sum((f.end - f.start) // 4 for f in allf)
    unsup = collections.Counter(); nonconf = collections.Counter()
    good = []
    for f in allf:
        n = (f.end - f.start) // 4
        if f.unsupported:
            key = f.unsupported.split(' at ')[0].split(' %')[0]
            key = 'jump table / jr reg' if key.startswith('jr $') else ('unsupported opcode' if key.startswith('opcode') else key)
            unsup[key] += n
        elif f.nonconf:
            nonconf[f.nonconf[0].split(' ')[0] + ' ' + f.nonconf[0].split(' ')[1]] += n
        else:
            good.append(f)
    good_words = sum(len(f.pcs) for f in good)
    stats = dict(functions=len(allf), words=tot_words, translatable_functions=len(good), translatable_reachable_words=good_words,
                 skipped_unsupported_words=dict(unsup), skipped_nonconforming_words=dict(nonconf),
                 elided_pairs=sum(len(f.elide_regs) for f in good), elide_instrs=sum(len(f.elide) for f in good))
    arg_hist = collections.Counter(len([r for r in f.args if r != 29]) for f in good)
    stats['arg_count_hist'] = dict(sorted(arg_hist.items()))
    stats['ret_hist'] = dict(collections.Counter(ret_type(f.ret) for f in good))
    stats['sp_param_fraction'] = sum(1 for f in good if 29 in f.args) / max(1, len(good))
    stats['conformance_example_reasons'] = collections.Counter(x.split(' ')[0] + ' ' + x.split(' ')[1] for f in allf if f.nonconf for x in f.nonconf[:1]).most_common(8)
    print(json.dumps(stats, indent=1, default=str))
    if mode == 'emitdiff':
        outdir = sys.argv[2]; maxn = int(sys.argv[3])
        os.makedirs(outdir, exist_ok=True)
        leaf = sorted([f for f in good if not f.calls and 6 <= len(f.pcs) <= 150], key=lambda f: f.start)
        step = max(1, len(leaf) // maxn)
        sel = leaf[::step][:maxn]
        extra = {0x088C9C1C, 0x08826DFC, 0x08833434, 0x08833AAC}
        have = {f.start for f in sel}
        sel += [f for f in leaf if f.start in extra and f.start not in have]
        sel.sort(key=lambda f: f.start)
        protos = {}
        bodies = [emit_function(f, fns, callee_args, protos) for f in sel]
        ptxt = ['%s %s(%s);' % (ret_type(f.ret), fname(f.start), ', '.join(params(f.args, False))) for f in sel]
        with open(os.path.join(outdir, 'lean_diff.cpp'), 'w') as fh:
            fh.write(HEADER + '\n'.join(ptxt) + '\n\n' + '\n'.join(bodies))
        open(os.path.join(outdir, 'difftest_decls.inc'), 'w').write('\n'.join(ptxt) + '\n')
        rows = []
        for f in sel:
            a = []
            if 29 in f.args: a.append('in.sp')
            for r in ARG_INT:
                if r in f.args: a.append('in.r[%d]' % r)
            for r in ARG_FP:
                if r in f.args: a.append('in.f[%d]' % (r - 32))
            call = '%s(%s)' % (fname(f.start), ', '.join(a))
            rt_ = ret_type(f.ret)
            if rt_ == 'void': body = call + ';'
            elif rt_ == 'u32': body = 'out.v0 = %s;' % call
            elif rt_ == 'float': body = 'out.f0 = %s;' % call
            else:
                body = 'u64 q = %s; ' % call
                if 2 in f.ret: body += 'out.v0 = (u32)q; '
                if 3 in f.ret: body += 'out.v1 = (u32)(q >> 32); '
                if F(0) in f.ret: body += 'out.f0 = BC_F((u32)(q >> 32)); '
            rows.append('{0x%08XU, %d, %d, %d, %d, [](const Inp& in, Out& out) { %s}},' % (
                f.start, len(f.pcs), int(2 in f.ret), int(3 in f.ret), int(F(0) in f.ret), body))
        open(os.path.join(outdir, 'difftest_table.inc'), 'w').write('\n'.join(rows) + '\n')
        print('emitdiff', len(sel), 'leaf functions of', len(leaf), 'candidates')
        return
    if mode != 'emit': return
    outdir = sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    maxfn = int(sys.argv[sys.argv.index('--maxfn') + 1]) if '--maxfn' in sys.argv else 10 ** 9
    sel = sorted(good, key=lambda f: f.start)[:maxfn]
    if '--sample' in sys.argv:
        import random
        random.seed(12345)
        k = int(sys.argv[sys.argv.index('--sample') + 1])
        sel = sorted(random.sample(good, min(k, len(good))), key=lambda f: f.start)
    if '--funcs' in sys.argv:
        want = {int(x, 16) for x in sys.argv[sys.argv.index('--funcs') + 1].split(',')}
        sel = sorted([f for f in good if f.start in want], key=lambda f: f.start)
        missing = want - {f.start for f in sel}
        if missing: print('NOT translatable / not found:', [hex(x) for x in missing])
    protos = {}
    chunks = []; cur = []; cur_words = 0
    for f in sel:
        txt = emit_function(f, fns, callee_args, protos)
        cur.append(txt); cur_words += len(f.pcs)
        if cur_words >= 25000:
            chunks.append((cur, cur_words)); cur = []; cur_words = 0
    if cur: chunks.append((cur, cur_words))
    own = {f.start for f in sel}
    ptxt = []
    for t, (a, rt_) in sorted(protos.items()):
        if t in own: continue
        ptxt.append('extern %s %s(%s);' % (ret_type(rt_), fname(t), ', '.join(params(a, False))))
    # own function prototypes (so that order inside a TU does not matter)
    for f in sel:
        ptxt.append('%s %s(%s);' % (ret_type(f.ret), fname(f.start), ', '.join(params(f.args, False))))
    manifest = dict(files=[], functions=len(sel), words=sum(len(f.pcs) for f in sel), guest_words_span=sum((f.end - f.start) // 4 for f in sel))
    for i, (cur, wds) in enumerate(chunks):
        fp = os.path.join(outdir, 'lean_%02d.cpp' % i)
        with open(fp, 'w') as fh:
            fh.write(HEADER + '\n'.join(sorted(set(ptxt))) + '\n\n' + '\n'.join(cur))
        manifest['files'].append(dict(file=fp, words=wds))
    json.dump(manifest, open(os.path.join(outdir, 'manifest.json'), 'w'), indent=1)
    json.dump({'0x%08X' % f.start: dict(words=len(f.pcs), span=(f.end - f.start) // 4) for f in sel}, open(os.path.join(outdir, 'funcs.json'), 'w'))
    print('emitted', manifest['functions'], 'functions,', manifest['words'], 'reachable guest words in', len(chunks), 'files')

if __name__ == '__main__':
    main()
