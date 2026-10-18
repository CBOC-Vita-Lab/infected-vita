"""VFPU lowering, step 2: static tracking of the prefix registers and selection of the specialised forms.

The three prefix registers (S, T, D = g_cpu.vfpu_ctrl[0..2]) are tracked per function over the node graph as a product lattice
(constant | unknown) per register.  At function entry and after every call the state is the architectural default
(0xE4, 0xE4, 0): the guest compiler never leaves a prefix pending across a call or a return, which is checked here for the
functions we can see (calls / tail calls / returns whose in-state is not the default are counted and listed in the manifest) and
dynamically by LEAN_VFPU_ENTRY_CHECK in test builds.

An instruction whose state is fully known and whose kind has a specialised form in lean_vfpu_fast.h is emitted as
lean_vk::form<W, S, T, D>(args); everything else stays on the generic lean_vf:: helper, which reads the live registers.
"""
from . import mips

DEF = (0xE4, 0xE4, 0)
TOP = None

# kinds that touch no prefix register at all (unless Mtv names a control register, see step())
NO_PREFIX = {'Lvs', 'Svs', 'Lvq', 'Svq', 'Mfv', 'Mtv'}
NO_PREFIX_EXTRA = {'lvl.q', 'lvr.q', 'svl.q', 'svr.q', 'll', 'sc', 'vmfvc'}

# kinds with a specialised form (name of the lean_vk template); all of them end by resetting the prefixes (Eat<>)
FAST_STATE = {'VfpuVec3': 'vvec3', 'Vdot': 'vdot', 'Vscl': 'vscl', 'VfpuHorizontal': 'vhoriz', 'VfpuCompare3': 'vcmp3',
              'VfpuUnary': 'vunary', 'Vidt': 'vidt', 'VfpuVectorInit': 'vinit', 'Viim': 'viim', 'Vf2i': 'vf2i', 'Vi2f': 'vi2f',
              'Vmmul': 'vmmul', 'Vtfm': 'vtfm', 'Vmscl': 'vmscl'}
# kinds the generic helper is known (tests/vfpu_test.cpp, model check) to always reset the prefixes after
GENERIC_EATS = set()

def is_vfpu_insn(d):
    return d.vfpu or d.kind in NO_PREFIX

def _join(a, b):
    return tuple(x if x == y else TOP for x, y in zip(a, b))

def fast_name(d):
    """name of the lean_vk template for this instruction, or None"""
    k = d.kind
    if k in FAST_STATE: return FAST_STATE[k]
    if k == 'Vi2x': return 'vi2uc' if ((d.w >> 16) & 31) == 28 else None
    if k == 'Extra' and d.mn == 'vi2c': return 'vi2c'
    return None

def step(d, st):
    """state after instruction d (generic or specialised, they agree) given the state before it"""
    k = d.kind
    if k == 'Vpfx':
        c = (d.w >> 24) & 3
        data = d.w & 0xFFFFF
        if c == 2: data &= 0xFFF
        st = list(st); st[c] = data
        return tuple(st)
    if k == 'Vflush':
        return st if (d.w & 0xFFFF0000) == 0xFFFF0000 else DEF
    if k == 'Mtv':
        r = d.w & 0xFF
        if 128 <= r < 131:
            st = list(st); st[r - 128] = TOP
            return tuple(st)
        return st
    if k in NO_PREFIX: return st
    if k == 'Extra':
        if d.mn in NO_PREFIX_EXTRA: return st
        if d.mn == 'vmtvc': return (TOP, TOP, TOP)
    if fast_name(d) is not None or k in GENERIC_EATS: return DEF
    if st == DEF: return DEF
    return _join(st, DEF)       # a generic compute instruction either consumes the pending prefixes or leaves them

def analyze(f):
    """-> (in_state by node id, issues).  Only nodes that hold a VFPU instruction matter, but the state flows through all."""
    nodes = f.nodes
    ins = {}
    if not any(n.insn is not None and n.kind in ('n', 'slot') and is_vfpu_insn(n.insn) for n in nodes):
        return None, []
    entry = f.by_key[f.entry].id
    ins[entry] = DEF
    work = [entry]
    while work:
        nid = work.pop()
        n = nodes[nid]
        st = ins[nid]
        if n.kind == 'cont': out = DEF
        elif n.kind in ('n', 'slot') and n.insn is not None: out = step(n.insn, st)
        else: out = st
        for sx in n.succ:
            old = ins.get(sx)
            new = out if old is None else _join(old, out)
            if new != old:
                ins[sx] = new
                work.append(sx)
    issues = []
    for nid, st in ins.items():
        n = nodes[nid]
        if st != DEF and n.kind in ('cont', 'tail', 'ret'):
            issues.append((n.pc, n.kind, st))
    return ins, issues

def has_vfpu(f):
    return any(n.insn is not None and n.kind in ('n', 'slot') and is_vfpu_insn(n.insn) for n in f.nodes)

def fast_lines(fg, d, st, nid):
    """C++ statements of the specialised form of d, or None when the generic helper has to be used"""
    k = d.kind
    w = '0x%08Xu' % d.w
    if k == 'Vpfx': return ['lean_vk::vpfx<%s>();' % w]
    if k in ('Lvs', 'Svs', 'Lvq', 'Svq'):
        off = mips.s16(d.w & 0xFFFC)
        a = fg.addr(d.rs, off)
        return ['lean_vk::%s<%s>(%s);' % ({'Lvs': 'lvs', 'Svs': 'svs', 'Lvq': 'lvq', 'Svq': 'svq'}[k], w, a)]
    if k == 'Mtv': return ['lean_vk::mtv<%s>(%s);' % (w, fg.R(d.rt))]
    if k == 'Mfv':
        if d.rt: return ['%s = lean_vk::mfv<%s>();' % (fg.Wr(d.rt), w)]
        return []
    name = fast_name(d)
    if name is None or st is None or TOP in st: return None
    if k == 'VfpuCompare3' and not 5 <= ((d.w >> 23) & 7) <= 7: return None
    return ['lean_vk::%s<%s, 0x%Xu, 0x%Xu, 0x%Xu>();' % (name, w, st[0], st[1], st[2])]
