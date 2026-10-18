"""VFPU cases: every instruction form with random operand registers, vector lengths and prefix states, plus programs that
exercise the generator's prefix tracking (straight line, joins, loops, calls, mtv to a prefix register).

    python3 lean_gen.py --synth tests/synth/cases_vfpu.py --out private/synth_vfpu            (specialised forms)
    python3 lean_gen.py --synth tests/synth/cases_vfpu.py --out private/synth_vfpu_g --no-vfpu-fast   (generic helpers)

Environment: LEAN_SYNTH_SEED (default 1), LEAN_VFPU_VARIANTS (variants per form, default 6), LEAN_VFPU_PROGRAMS (default 400).
"""
import os, random, sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..'))
from leangen.synth import Asm

def szbits(n):
    n -= 1
    return ((n & 1) << 7) | (((n >> 1) & 1) << 15)

PFX_S, PFX_T, PFX_D = 0xDC000000, 0xDD000000, 0xDE000000

class Rnd:
    def __init__(self, seed): self.r = random.Random(seed)

    def reg(self, n, bias=True):
        """a 7-bit VFPU register number; biased toward the encodings the game uses (column vectors, row vectors, matrix quads)"""
        k = self.r.random()
        if bias and k < 0.45:                      # a column of a matrix (contiguous in storage)
            return (self.r.randrange(8) << 2) | self.r.randrange(4)
        if bias and k < 0.65:                      # a row
            return 0x20 | (self.r.randrange(8) << 2) | self.r.randrange(4)
        return self.r.randrange(128)

    def mreg(self):
        k = self.r.random()
        if k < 0.55: return self.r.randrange(8) << 2                      # M(m)00
        if k < 0.8: return 0x20 | (self.r.randrange(8) << 2)              # E(m)00 (transposed)
        return self.r.randrange(128)

    def swz(self):
        k = self.r.random()
        if k < 0.3: return 0xE4
        if k < 0.5: return self.r.choice([0x00, 0x55, 0xAA, 0xFF, 0x1B, 0xB1, 0x4E, 0xD2, 0xC9])
        return self.r.randrange(256)

    def sprefix(self):
        """a 20-bit source prefix"""
        sw = self.swz()
        ab = self.r.choice([0, 0, 0, 1, 2, 4, 8, 15, self.r.randrange(16)])
        cs = self.r.choice([0, 0, 0, 0, 1, 2, 4, 8, self.r.randrange(16)])
        ng = self.r.choice([0, 0, 0, 1, 2, 4, 8, 15, self.r.randrange(16)])
        return sw | (ab << 8) | (cs << 12) | (ng << 16)

    def dprefix(self):
        sat = 0
        for i in range(4):
            sat |= self.r.choice([0, 0, 0, 1, 3, 2]) << (2 * i)
        mask = self.r.choice([0, 0, 0, 1, 2, 4, 8, 3, 12, 5, 10, self.r.randrange(16)])
        return sat | (mask << 8)

def w3(op, o, vd, vs, vt, n):
    return (op << 26) | (o << 23) | (vt << 16) | (vs << 8) | vd | szbits(n)

def wgrp(group, opn, vd, vs, n):
    return (0x34 << 26) | (group << 21) | (opn << 16) | (vs << 8) | vd | szbits(n)

def wmat(group, vd, vs, vt, n):
    return (0x3C << 26) | (group << 21) | (vt << 16) | (vs << 8) | vd | szbits(n)

# ----------------------------------------------------------------------------- the forms
# each entry: (name, builder(rnd) -> (word, uses_s, uses_t, uses_d))  -- the prefixes are drawn only for the registers used
def forms(R):
    r = R.r
    F = []
    def n_(lo=1, hi=4): return r.randint(lo, hi)
    def vec3(name, op, o, sizes=(1, 2, 3, 4)):
        def b():
            n = r.choice(sizes); return w3(op, o, R.reg(n), R.reg(n), R.reg(n), n), 1, 1, 1
        F.append((name, b))
    vec3('vadd', 0x18, 0); vec3('vsub', 0x18, 1); vec3('vdiv', 0x18, 7)
    vec3('vmul', 0x19, 0); vec3('vdot', 0x19, 1); vec3('vhdp', 0x19, 4); vec3('vsbn', 0x18, 2)
    def b_vscl():
        n = n_(); return w3(0x19, 2, R.reg(n), R.reg(n), R.reg(1), n), 1, 1, 1
    F.append(('vscl', b_vscl))
    F.append(('vcrs', lambda: (w3(0x19, 5, R.reg(3), R.reg(3), R.reg(3), 3), 1, 1, 1)))
    F.append(('vdet', lambda: (w3(0x19, 6, R.reg(2), R.reg(2), R.reg(2), 2), 1, 1, 1)))
    vec3('vmin', 0x1B, 2); vec3('vmax', 0x1B, 3); vec3('vscmp', 0x1B, 5); vec3('vsge', 0x1B, 6); vec3('vslt', 0x1B, 7)
    def b_vcmp():
        n = n_(); return (0x1B << 26) | (0 << 23) | (R.reg(n) << 16) | (R.reg(n) << 8) | r.randrange(16) | szbits(n), 1, 1, 0
    F.append(('vcmp', b_vcmp))
    # unary group
    for opn, nm in [(0, 'vmov'), (1, 'vabs'), (2, 'vneg'), (4, 'vsat0'), (5, 'vsat1'), (16, 'vrcp'), (17, 'vrsq'), (18, 'vsin'), (19, 'vcos'),
                    (20, 'vexp2'), (21, 'vlog2'), (22, 'vsqrt'), (23, 'vasin'), (24, 'vnrcp'), (26, 'vnsin'), (28, 'vrexp2')]:
        F.append((nm, (lambda opn: lambda: (lambda n: (wgrp(0, opn, R.reg(n), R.reg(n), n), 1, 0, 1))(n_()))(opn)))
    F.append(('vidt', lambda: (lambda n: (wgrp(0, 3, R.reg(n), 0, n), 0, 0, 1))(r.choice([2, 3, 4]))))
    F.append(('vzero', lambda: (lambda n: (wgrp(0, 6, R.reg(n), 0, n), 0, 0, 1))(n_())))
    F.append(('vone', lambda: (lambda n: (wgrp(0, 7, R.reg(n), 0, n), 0, 0, 1))(n_())))
    F.append(('vfad', lambda: (lambda n: (wgrp(2, 6, R.reg(1), R.reg(n), n), 1, 0, 1))(n_(2, 4))))
    F.append(('vavg', lambda: (lambda n: (wgrp(2, 7, R.reg(1), R.reg(n), n), 1, 0, 1))(n_(2, 4))))
    F.append(('vsgn', lambda: (lambda n: (wgrp(2, 10, R.reg(n), R.reg(n), n), 1, 0, 1))(n_())))
    F.append(('vocp', lambda: (lambda n: (wgrp(2, 4, R.reg(n), R.reg(n), n), 1, 0, 1))(n_())))
    F.append(('vsocp', lambda: (lambda n: (wgrp(2, 5, R.reg(n), R.reg(n), n), 1, 0, 1))(n_(1, 2))))
    F.append(('vbfy1', lambda: (lambda n: (wgrp(2, 2, R.reg(n), R.reg(n), n), 1, 0, 1))(r.choice([2, 4]))))
    F.append(('vbfy2', lambda: (lambda n: (wgrp(2, 3, R.reg(n), R.reg(n), n), 1, 0, 1))(4)))
    for opn, nm in [(0, 'vsrt1'), (1, 'vsrt2'), (8, 'vsrt3'), (9, 'vsrt4')]:
        F.append((nm, (lambda opn: lambda: (wgrp(2, opn, R.reg(4), R.reg(4), 4), 1, 0, 1))(opn)))
    F.append(('vt4444', lambda: (wgrp(2, 25, R.reg(2), R.reg(4), 4), 1, 0, 1)))
    F.append(('vt5551', lambda: (wgrp(2, 26, R.reg(2), R.reg(4), 4), 1, 0, 1)))
    F.append(('vt5650', lambda: (wgrp(2, 27, R.reg(2), R.reg(4), 4), 1, 0, 1)))
    F.append(('vcst', lambda: (lambda n: (wgrp(3, r.randrange(20), R.reg(n), 0, n), 0, 0, 1))(n_())))
    F.append(('vrnds', lambda: (wgrp(1, 0, 0, R.reg(1), 1), 1, 0, 0)))
    for opn, nm in [(1, 'vrndi'), (2, 'vrndf1'), (3, 'vrndf2')]:
        F.append((nm, (lambda opn: lambda: (lambda n: (wgrp(1, opn, R.reg(n), 0, n), 0, 0, 1))(n_()))(opn)))
    F.append(('vf2h', lambda: (lambda n: (wgrp(1, 18, R.reg(n // 2 + 1 if n > 1 else 1), R.reg(n), n), 1, 0, 1))(r.choice([2, 4]))))
    F.append(('vh2f', lambda: (lambda n: (wgrp(1, 19, R.reg(n), R.reg(n // 2 if n > 1 else 1), n), 1, 0, 1))(r.choice([2, 4]))))
    F.append(('vsbz', lambda: (wgrp(1, 22, R.reg(1), R.reg(1), 1), 1, 0, 1)))
    F.append(('vlgb', lambda: (wgrp(1, 23, R.reg(1), R.reg(1), 1), 1, 0, 1)))
    for opn, nm in [(24, 'vuc2i'), (25, 'vc2i'), (26, 'vus2i'), (27, 'vs2i')]:
        F.append((nm, (lambda opn: lambda: (lambda n: (wgrp(1, opn, R.reg(4), R.reg(n), n), 1, 0, 1))(r.choice([1, 2, 4])))(opn)))
    F.append(('vi2uc', lambda: (wgrp(1, 28, R.reg(1), R.reg(4), 4), 1, 0, 1)))
    F.append(('vi2c', lambda: (lambda n: (wgrp(1, 29, R.reg(1), R.reg(n), n), 1, 0, 1))(n_(2, 4))))
    F.append(('vi2us', lambda: (lambda n: (wgrp(1, 30, R.reg(n // 2 or 1), R.reg(n), n), 1, 0, 1))(r.choice([2, 4]))))
    F.append(('vi2s', lambda: (lambda n: (wgrp(1, 31, R.reg(n // 2 or 1), R.reg(n), n), 1, 0, 1))(r.choice([2, 4]))))
    for g, nm in [(16, 'vf2in'), (17, 'vf2iz'), (18, 'vf2iu'), (19, 'vf2id')]:
        F.append((nm, (lambda g: lambda: (lambda n: (wgrp(g, r.choice([0, 0, 1, 7, 8, 15, 16, 23, 31, r.randrange(32)]), R.reg(n), R.reg(n), n), 1, 0, 1))(n_()))(g)))
    F.append(('vi2f', lambda: (lambda n: (wgrp(20, r.choice([0, 0, 1, 7, 8, 15, 16, 23, 31, r.randrange(32)]), R.reg(n), R.reg(n), n), 1, 0, 1))(n_())))
    F.append(('vcmov', lambda: (lambda n: (wgrp(21, r.randrange(8) | (r.randrange(2) << 3), R.reg(n), R.reg(n), n), 1, 0, 1))(n_())))
    F.append(('vwbn', lambda: (wgrp(24 + r.randrange(8), r.randrange(256), R.reg(1), R.reg(1), 1), 1, 0, 1)))
    # matrices
    F.append(('vmmul', lambda: (lambda n: (wmat(r.randrange(4), R.mreg(), R.mreg(), R.mreg(), n), 0, 0, 0))(r.choice([2, 3, 4, 4, 4]))))
    F.append(('vtfm', lambda: (lambda n: (wmat(4 + 4 * (n - 2) + r.randrange(3), R.reg(n), R.mreg(), R.reg(n), n), 1, 1, 1))(r.choice([2, 3, 4]))))
    F.append(('vhtfm', lambda: (lambda n: (wmat(4 + 4 * (n - 2) + r.randrange(3), R.reg(n), R.mreg(), R.reg(n), n), 1, 1, 1))(r.choice([2, 3, 4]))))
    F.append(('vmscl', lambda: (lambda n: (wmat(16 + r.randrange(4), R.mreg(), R.mreg(), R.reg(1), n), 1, 1, 1))(r.choice([2, 3, 4]))))
    F.append(('vcrsp', lambda: (wmat(20 + r.randrange(4), R.reg(3), R.reg(3), R.reg(3), 3), 1, 1, 1)))
    F.append(('vqmul', lambda: (wmat(20 + r.randrange(4), R.reg(4), R.reg(4), R.reg(4), 4), 1, 1, 1)))
    F.append(('vmmov', lambda: (lambda n: (wmat(28, R.mreg(), R.mreg(), 0, n), 0, 0, 0))(r.choice([2, 3, 4]))))
    F.append(('vmidt', lambda: (lambda n: (wmat(28, R.mreg(), 0, 3, n), 0, 0, 0))(r.choice([2, 3, 4]))))
    F.append(('vmzero', lambda: (lambda n: (wmat(28, R.mreg(), 0, 6, n), 0, 0, 0))(r.choice([2, 3, 4]))))
    F.append(('vmone', lambda: (lambda n: (wmat(28, R.mreg(), 0, 7, n), 0, 0, 0))(r.choice([2, 3, 4]))))
    F.append(('vrot', lambda: (lambda n: (wmat(29, R.reg(n), R.reg(1), r.randrange(32), n), 1, 0, 1))(r.choice([2, 3, 4]))))
    # immediates
    F.append(('viim', lambda: ((0x37 << 26) | (6 << 23) | (R.reg(1) << 16) | r.randrange(65536), 0, 0, 1)))
    F.append(('vfim', lambda: ((0x37 << 26) | (7 << 23) | (R.reg(1) << 16) | r.randrange(65536), 0, 0, 1)))
    return F

# memory forms: (asm text builder)
def mem_forms(R):
    r = R.r
    out = []
    def off(): return r.choice([0, 4, 8, 16, 32, 48, 64, 128])
    out.append(('lv_s', lambda: 'lv.s %d, %d(a0)' % (R.reg(1, False), off())))
    out.append(('sv_s', lambda: 'sv.s %d, %d(a0)' % (R.reg(1, False), off())))
    out.append(('lv_q', lambda: 'lv.q %d, %d(a0)' % (R.reg(4) & 0x3F, off())))
    out.append(('sv_q', lambda: 'sv.q %d, %d(a0)' % (R.reg(4) & 0x3F, off())))
    out.append(('lvl_q', lambda: 'lvl.q %d, %d(a0)' % (R.reg(4) & 0x3F, off() + r.choice([0, 4, 8, 12]))))
    out.append(('lvr_q', lambda: 'lvr.q %d, %d(a0)' % (R.reg(4) & 0x3F, off() + r.choice([0, 4, 8, 12]))))
    out.append(('svl_q', lambda: 'svl.q %d, %d(a0)' % (R.reg(4) & 0x3F, off() + r.choice([0, 4, 8, 12]))))
    out.append(('svr_q', lambda: 'svr.q %d, %d(a0)' % (R.reg(4) & 0x3F, off() + r.choice([0, 4, 8, 12]))))
    return out

def build():
    seed = int(os.environ.get('LEAN_SYNTH_SEED', '1'))
    nvar = int(os.environ.get('LEAN_VFPU_VARIANTS', '6'))
    nprog = int(os.environ.get('LEAN_VFPU_PROGRAMS', '400'))
    R = Rnd(seed)
    r = R.r
    a = Asm()
    A = a.asm
    A('''
    .func main_entry
        jr ra
        nop
    ''')
    FM = forms(R)
    MF = mem_forms(R)
    cnt = [0]

    def prefixes(uses):
        """prefix instructions (as words) for the registers an instruction uses; random subset, random order"""
        us, ut, ud = uses
        out = []
        if us and r.random() < 0.5: out.append(PFX_S | R.sprefix())
        if ut and r.random() < 0.5: out.append(PFX_T | R.sprefix())
        if ud and r.random() < 0.4: out.append(PFX_D | R.dprefix())
        if r.random() < 0.15 and not out and (us or ut or ud):
            out.append(r.choice([PFX_S, PFX_T]) | R.sprefix())      # a prefix the instruction may not even read
        r.shuffle(out)
        return out

    # ---- one function per variant of every form
    for name, bld in FM:
        for v in range(nvar):
            w, us, ut, ud = bld()
            pf = prefixes((us, ut, ud))
            cnt[0] += 1
            lines = ['.func v_%s_%d' % (name, v)] + ['.word 0x%08X' % x for x in pf] + ['.word 0x%08X' % w, 'jr ra', 'nop']
            A('\n'.join(lines))
    for name, bld in MF:
        for v in range(nvar):
            lines = ['.func m_%s_%d' % (name, v)] + (['.word 0x%08X' % x for x in prefixes((1, 1, 1))] if r.random() < 0.3 else [])
            lines += [bld(), 'jr ra', 'nop']
            A('\n'.join(lines))
    # forms with a NEON path (contiguous column quads, default prefixes), several of each, plus the same with one prefix set
    def col(): return (r.randrange(8) << 2) | r.randrange(4)
    def mat(): return r.randrange(8) << 2
    neon = [('vadd', lambda: w3(0x18, 0, col(), col(), col(), 4)), ('vsub', lambda: w3(0x18, 1, col(), col(), col(), 4)),
            ('vmul', lambda: w3(0x19, 0, col(), col(), col(), 4)), ('vdot', lambda: w3(0x19, 1, col(), col(), col(), 4)),
            ('vscl', lambda: w3(0x19, 2, col(), col(), col(), 4)), ('vmmul', lambda: wmat(r.randrange(4), mat(), mat(), mat(), 4))]
    for name, bld in neon:
        for v in range(nvar * 4):
            lines = ['.func n_%s_%d' % (name, v)]
            k = v % 4
            if k == 1: lines.append('.word 0x%08X' % (PFX_S | R.sprefix()))
            elif k == 2: lines.append('.word 0x%08X' % (PFX_D | R.dprefix()))
            elif k == 3: lines.append('.word 0x%08X' % (PFX_T | R.sprefix()))
            lines += ['.word 0x%08X' % bld(), 'jr ra', 'nop']
            A('\n'.join(lines))
    for v in range(nvar * 2):
        A('\n'.join(['.func n_lvq_%d' % v, 'lv.q %d, %d(a0)' % (col(), r.choice([0, 16, 32, 64])), 'lv.q %d, %d(a0)' % (col(), r.choice([0, 16, 32, 64])),
                     'sv.q %d, %d(a0)' % (col(), r.choice([96, 112, 128, 144])), 'jr ra', 'nop']))
    # mtv / mfv, including the control registers; vflush
    for v in range(nvar * 2):
        reg = r.choice([r.randrange(128), r.randrange(128), 128, 129, 130, 131, 132 + r.randrange(12)])
        A('\n'.join(['.func s_mtv_%d' % v, '.word 0x%08X' % ((0x12 << 26) | (7 << 21) | (4 << 16) | reg),
                     '.word 0x%08X' % ((0x12 << 26) | (3 << 21) | (2 << 16) | r.choice([reg, r.randrange(128)])),
                     'jr ra', 'nop']))
    for v in range(nvar):
        A('\n'.join(['.func s_flush_%d' % v] + ['.word 0x%08X' % x for x in prefixes((1, 1, 1))] +
                    ['.word 0x%08X' % r.choice([0xFFFF0000, 0xFFFF0000, 0xFFFE0000, 0xFFFF0400]), 'jr ra', 'nop']))
    # branch on the VFPU condition code
    for v in range(nvar):
        n = r.choice([2, 4]); cond = r.randrange(16)
        bt = r.choice(['bvf', 'bvt', 'bvfl', 'bvtl']); cc = r.randrange(8)
        A('\n'.join(['.func s_bv_%d' % v] + ['.word 0x%08X' % x for x in prefixes((1, 1, 0))] +
                    ['.word 0x%08X' % ((0x1B << 26) | (R.reg(n) << 16) | (R.reg(n) << 8) | cond | szbits(n)),
                     '%s %d, sbv_%d' % (bt, cc, v), 'addiu v0, zero, 1', 'jr ra', 'nop', 'sbv_%d:' % v, 'addiu v0, zero, 2', 'jr ra', 'nop']))

    # ---- programs: random sequences of forms with control flow, to exercise the prefix-state tracking
    def draw():
        if r.random() < 0.15:
            return None, r.choice(MF)[1]()
        name, bld = r.choice(FM)
        w, us, ut, ud = bld()
        return (w, (us, ut, ud)), None
    def emit_op(lines, item):
        op, memline = item
        if memline is not None:
            lines.append(memline); return
        w, uses = op
        for x in prefixes(uses): lines.append('.word 0x%08X' % x)
        lines.append('.word 0x%08X' % w)
    for p in range(nprog):
        kind = r.choice(['line', 'line', 'join', 'loop', 'call', 'mtvpfx', 'condop', 'cmpbr'])
        lines = ['.func p_%s_%d' % (kind, p)]
        if kind == 'line':
            for _ in range(r.randint(2, 7)): emit_op(lines, draw())
        elif kind == 'join':
            # a prefix set on one path only; the instruction after the join sees an unknown state
            lines += ['beq a1, zero, pj_%d' % p, 'nop']
            pre = r.choice([PFX_S, PFX_T, PFX_D])
            lines.append('.word 0x%08X' % (pre | (R.dprefix() if pre == PFX_D else R.sprefix())))
            lines += ['pj_%d:' % p]
            for _ in range(r.randint(1, 3)): emit_op(lines, draw())
        elif kind == 'loop':
            lines += ['andi t0, a1, 3', 'addiu t0, t0, 1', 'pl_%d:' % p]
            for _ in range(r.randint(1, 3)): emit_op(lines, draw())
            lines += ['addiu t0, t0, -1', 'bgtz t0, pl_%d' % p, 'nop']
        elif kind == 'call':
            lines += ['addiu sp, sp, -16', 'sw ra, 12(sp)']
            for _ in range(r.randint(1, 2)): emit_op(lines, draw())
            lines += ['.word 0xFFFE0000', 'jal %s' % r.choice(['v_vadd_0', 'v_vdot_0', 'v_vmul_1', 'v_vmmul_0']), 'nop']      # no prefix pending at a call
            for _ in range(r.randint(1, 2)): emit_op(lines, draw())
            lines += ['lw ra, 12(sp)', 'addiu sp, sp, 16']
        elif kind == 'mtvpfx':
            lines += ['.word 0x%08X' % ((0x12 << 26) | (7 << 21) | (5 << 16) | (128 + r.randrange(3)))]       # mtv a1, <prefix register>
            for _ in range(r.randint(1, 3)): emit_op(lines, draw())
        elif kind == 'condop':
            # an instruction that either runs or not; the state after is the join of "prefix pending" and "default"
            op = draw()
            lines += ['beq a1, zero, pc_%d' % p, 'nop']
            emit_op(lines, op)
            lines += ['pc_%d:' % p]
            for _ in range(r.randint(1, 2)): emit_op(lines, draw())
        else:
            # vcmp then branch on the VFPU condition code
            n = r.choice([2, 4])
            lines.append('.word 0x%08X' % ((0x1B << 26) | (R.reg(n) << 16) | (R.reg(n) << 8) | r.randrange(16) | szbits(n)))
            cc = r.randrange(8); bt = r.choice(['bvf', 'bvt', 'bvfl', 'bvtl'])
            lines += ['%s %d, bv_%d' % (bt, cc, p), 'nop']
            emit_op(lines, draw())
            lines += ['bv_%d:' % p]
            emit_op(lines, draw())
        lines += ['jr ra', 'nop']
        A('\n'.join(lines))
    im = a.image()
    profile = {'noreturn': [], 'weak_args': 4, 'yield': [], 'hooks': {}, 'extra_inputs': {}}
    return im, profile
