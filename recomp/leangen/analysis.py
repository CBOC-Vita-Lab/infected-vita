"""Whole-program analysis for the lean generator.

Per function:  abstract interpretation (sp / saved registers / return address / stack slots)  ->  modified set MW,
               structural checks, promotable stack slots.
Per program:   fixpoint over the call graph for MW, then liveness (with dead-code elimination) for in-sets A, demand R,
               return sets R' and the typed / regfile decision.

Resource bits: 0..31 GPR, 32..63 FPR, 64 CC, 65 HI, 66 LO, 70+ promoted stack slots / switch index temporaries.
"""
import collections, sys
from . import mips
from .mips import F, HI, LO, CC, RA, SP, CALLEE_SAVED, CALLER_SAVED

T = 'T'
ALLK = type('ALLK', (), {'__contains__': lambda s, k: True})()
NRES = 67
RES_MASK = (1 << NRES) - 1
V0, V1, F0 = 2, 3, F(0)
OUT_ENC = (1 << V0) | (1 << V1) | (1 << F0)
ARG_GPR = list(range(4, 12))
ARG_FPR = [F(i) for i in range(12, 20)]
ARGS_MASK = 0
for _r in ARG_GPR + ARG_FPR + [SP]: ARGS_MASK |= 1 << _r
ARGS_NOSP = ARGS_MASK & ~(1 << SP)
CALLER_SAVED_MASK = 0
for _r in CALLER_SAVED: CALLER_SAVED_MASK |= 1 << _r
CALLEE_SAVED_MASK = 0
for _r in CALLEE_SAVED: CALLEE_SAVED_MASK |= 1 << _r
IMPORT_MW = (1 << V0) | (1 << V1) | (1 << F0)
SLOT_BASE = 70

def bit(r): return 1 << r
def bits_of(rs):
    b = 0
    for r in rs: b |= 1 << r
    return b
def regs_in(mask):
    r = 0; out = []
    while mask:
        if mask & 1: out.append(r)
        mask >>= 1; r += 1
    return out

# ----------------------------------------------------------------------------------------------- abstract state
def sget(regs, r):
    if r == 0: return ('c', 0)
    v = regs.get(r)
    return ('o', r) if v is None else v

def join_states(a, b):
    if a is None: return b
    if b is None: return a
    ra, sa = a; rb, sb = b
    if ra is rb and sa is sb: return a
    out = {}
    for r in ra.keys() | rb.keys():
        va = ra.get(r); vb = rb.get(r)
        if va is None: va = ('o', r)
        if vb is None: vb = ('o', r)
        if va == vb:
            if va != ('o', r): out[r] = va
        else: out[r] = T
    so = {}
    for k in sa.keys() | sb.keys():
        va = sa.get(k); vb = sb.get(k)
        if va is None or vb is None: so[k] = 'P'        # possibly never stored on some path
        elif va == vb: so[k] = va
        else: so[k] = T if (va != 'P' and vb != 'P') else 'P'
    return (out, so)

def setreg(regs, r, v):
    if r == 0: return regs
    regs = dict(regs)
    if v == ('o', r): regs.pop(r, None)
    else: regs[r] = v
    return regs

MEMW = ('Lw', 'Sw', 'Lwc1', 'Swc1')
def memref(d):
    """(is_store, size, base, imm, cls) ; cls: 'w' word, 'p' partial, 'u' unaligned-pair, 'v' vector/VFPU, 'x' ll/sc"""
    k = d.kind
    if k in MEMW: return (k[0] == 'S', 4, d.rs, d.simm, 'w')
    if k in ('Lh', 'Lhu', 'Sh'): return (k[0] == 'S', 2, d.rs, d.simm, 'p')
    if k in ('Lb', 'Lbu', 'Sb'): return (k[0] == 'S', 1, d.rs, d.simm, 'p')
    if k in ('Lwl', 'Lwr', 'Swl', 'Swr'): return (k[0] == 'S', 4, d.rs, d.simm, 'u')
    if k in ('Lvs', 'Svs'): return (k == 'Svs', 4, d.rs, mips.s16(d.w & 0xFFFC), 'v')
    if k in ('Lvq', 'Svq'): return (k == 'Svq', 16, d.rs, mips.s16(d.w & 0xFFFC), 'v')
    if k == 'Extra':
        if d.mn in ('lvl.q', 'lvr.q', 'svl.q', 'svr.q'): return (d.mn[0] == 's', 16, d.rs, mips.s16(d.w & 0xFFFC), 'v')
        if d.mn in ('ll', 'sc'): return (d.mn == 'sc', 4, d.rs, d.simm, 'x')
    return None

def apply_insn(d, st, track, flags):
    """Abstract effect of a non-control instruction. flags: dict collecting 'escape'."""
    regs, slots = st
    k = d.kind
    mr = memref(d)
    if mr is not None:
        is_store, size, base, imm, cls = mr
        bv = sget(regs, base)
        if bv[0] == 's' and is_store:
            key = bv[1] + imm
            if cls == 'w':
                if key in track:
                    slots = dict(slots)
                    slots[key] = sget(regs, d.rt if k == 'Sw' else F(d.rt))
            else:
                lo = key - (3 if cls == 'u' else 0); hi = key + size
                for tk in list(slots):
                    if tk + 4 > lo and tk < hi:
                        slots = dict(slots); slots[tk] = T
    nv = None
    dst = None
    if k == 'Addiu':
        rv = sget(regs, d.rs)
        dst = d.rt
        nv = ('s', rv[1] + d.simm) if rv[0] == 's' else (rv if d.simm == 0 else T)
        if nv[0] == 's' and dst != SP and nv[1] >= 0: flags['escape_up'] = True
    elif k in ('Or', 'Addu') and (d.rt == 0 or d.rs == 0):
        dst = d.rd
        nv = sget(regs, d.rs if d.rt == 0 else d.rt)
    elif k == 'Lw' or k == 'Lwc1':
        dst = d.rt if k == 'Lw' else F(d.rt)
        bv = sget(regs, d.rs)
        if bv[0] == 's':
            key = bv[1] + d.simm
            if key in track:
                nv = slots.get(key)
                if nv is None or nv == 'P':
                    flags['bad'].add(key); nv = T
            else: nv = T
        else: nv = T
    elif k in ('Movz', 'Movn'):
        a = sget(regs, d.rs); b = sget(regs, d.rd)
        dst = d.rd; nv = a if a == b else T
    if dst is not None:
        if dst == 0: return (regs, slots)
        if nv[0] == 's' and dst != SP:
            flags['escape'] = True
            if nv[1] >= 0: flags['escape_up'] = True
        return (setreg(regs, dst, nv), slots)
    # generic instruction: any use of a stack-derived value other than as memory base escapes it
    if d.uses:
        data_reg = None
        if mr is not None and mr[0] and k != 'Swc1': data_reg = d.rt
        for u in d.uses:
            uv = regs.get(u)
            if uv is not None and uv != T and uv[0] == 's':
                if mr is not None and u == mr[2] and u != data_reg:
                    continue
                flags['escape'] = True; flags['escape_up'] = True
    if d.defs:
        regs = dict(regs)
        for r in d.defs:
            if r != 0: regs[r] = T
    return (regs, slots)

def apply_call(st, mw):
    regs, slots = st
    regs = dict(regs)
    m = mw
    r = 0
    while m:
        if m & 1: regs[r] = T
        m >>= 1; r += 1
    regs[RA] = T
    return (regs, slots)

# ----------------------------------------------------------------------------------------------- per-function result
class FA:
    """Analysis state of one function."""
    def __init__(self, f):
        self.f = f
        self.mod = 0                  # registers possibly changed (vs entry) at some return/tail  [own, includes callee effects]
        self.reasons = []             # structural reasons for regfile form
        self.escape = False
        self.promoted = {}            # frame key -> slot index
        self.mem = {}                 # node id -> ('sp', key, slot_idx|None)
        self.ret_nodes = []
        self.sp_dynamic = False
        self.variadic = False
        self.dead = set()
        self.live_out = {}            # node id -> mask (only for nodes of interest)
        self.A = 0
        self.nstatic = {}             # static (uses, defs) per node id

def callee_mw(node, S, mw_ind, hooks):
    """Registers the callee of cont/tail node may modify (call-visible effects)."""
    t = node.tgt
    if node.kind == 'cont' and node.insn.mn == 'jalr':
        return mw_ind
    if t is None: return mw_ind
    return S.mw_of(t)

INF = 1 << 30

def analyze_function_abs(f, S, mw_ind, keep=False):
    fa = FA(f)
    nodes = f.nodes
    track = ALLK
    for _round in range(6):
        flags = {'escape': False, 'escape_up': False, 'bad': set()}
        sin = [None] * len(nodes)
        start = f.by_key[f.entry].id
        sin[start] = ({SP: ('s', 0)}, {})
        work = [start]; inwork = {start}
        while work:
            nid = work.pop(); inwork.discard(nid)
            n = nodes[nid]; st = sin[nid]; k = n.kind
            if k in ('n', 'slot'):
                out = apply_insn(n.insn, st, track, flags)
            elif k == 'cont':
                out = apply_call(st, callee_mw(n, S, mw_ind, None))
            else:
                out = st
            for s in n.succ:
                j = join_states(sin[s], out)
                if sin[s] is None or j != sin[s]:
                    sin[s] = j
                    if s not in inwork: inwork.add(s); work.append(s)
        # classify accesses
        acc = collections.defaultdict(lambda: dict(loads=0, stores=0, bad=False, saveonly=True))
        partial = []
        wext = 0
        for n in nodes:
            if n.kind not in ('n', 'slot') or sin[n.id] is None: continue
            mr = memref(n.insn)
            if mr is None: continue
            is_store, size, base, imm, cls = mr
            bv = sget(sin[n.id][0], base)
            if bv[0] != 's': continue
            key = bv[1] + imm
            if key + size > 0: wext = max(wext, key + size)
            if cls == 'w' and n.insn.kind != 'Extra':
                a = acc[key]
                if is_store:
                    a['stores'] += 1
                    v = sget(sin[n.id][0], n.insn.rt if n.insn.kind == 'Sw' else F(n.insn.rt))
                    if not (v != T and v[0] == 'o' and (v[1] in CALLEE_SAVED or v[1] == RA) and v[1] != 0): a['saveonly'] = False
                else: a['loads'] += 1
                if key & 3: a['bad'] = True
            else:
                lo = key - (3 if cls == 'u' else 0); hi = key + size
                partial.append((lo, hi))
        keys = sorted(acc)
        P = {}
        for key, a in acc.items():
            if key >= 0 or a['bad'] or a['loads'] == 0 or a['stores'] == 0 or key in flags['bad']: continue
            if flags['escape'] and not a['saveonly']: continue
            if any(lo < key + 4 and hi > key for lo, hi in partial): continue
            if any(k2 != key and k2 < key + 4 and k2 + 4 > key for k2 in keys): continue
            P[key] = True
        newtrack = set(P)
        saveonly_keys = {k for k in P if acc[k]['saveonly']}
        if track is not ALLK:
            newtrack &= track
            if newtrack == track: break
        track = newtrack
    else:
        # did not stabilise: give up promotion altogether (sound: untracked loads are T)
        track = set(); saveonly_keys = set()
        flags = {'escape': True, 'escape_up': True, 'bad': set()}
        sin = [None] * len(nodes)
        start = f.by_key[f.entry].id
        sin[start] = ({SP: ('s', 0)}, {})
        work = [start]; inwork = {start}
        while work:
            nid = work.pop(); inwork.discard(nid)
            n = nodes[nid]; st = sin[nid]; k = n.kind
            if k in ('n', 'slot'): out = apply_insn(n.insn, st, track, flags)
            elif k == 'cont': out = apply_call(st, callee_mw(n, S, mw_ind, None))
            else: out = st
            for s2 in n.succ:
                j = join_states(sin[s2], out)
                if sin[s2] is None or j != sin[s2]:
                    sin[s2] = j
                    if s2 not in inwork: inwork.add(s2); work.append(s2)
    fa.escape = flags['escape']
    if keep: fa.sin = sin
    fa.W = INF if flags['escape_up'] else wext
    fa.tracked = set(track)
    # pinned keys: slots a callee may read through its incoming-argument area
    pinned_all = False; pin_ranges = []
    for n in nodes:
        if n.kind != 'cont' or sin[n.id] is None: continue
        # state *before* the call completes is sin of the cont node
        regs = sin[n.id][0]
        sv = sget(regs, SP)
        if n.insn.mn == 'jalr': W = S.W_ind
        elif n.tgt in S.imports: W = 0
        else: W = S.W_of(n.tgt)
        if W == 0: continue
        if sv == T or sv[0] != 's': pinned_all = True; continue
        pin_ranges.append((sv[1], sv[1] + W))
    promoted = set()
    for key in track:
        if key in saveonly_keys:
            promoted.add(key); continue          # callee-saved / ra save slots are never an argument area
        if pinned_all: continue
        if any(lo <= key + 3 and key < hi for lo, hi in pin_ranges): continue
        promoted.add(key)
    idx = {}
    for i, key in enumerate(sorted(promoted)): idx[key] = i
    fa.promoted = idx
    mod = 0
    for n in nodes:
        st = sin[n.id]
        if st is None: continue
        regs = st[0]
        if n.kind in ('n', 'slot'):
            mr = memref(n.insn)
            if mr is not None:
                bv = sget(regs, mr[2])
                if bv[0] == 's':
                    key = bv[1] + mr[3]
                    fa.mem[n.id] = ('sp', key, idx.get(key) if (mr[4] == 'w' and n.insn.kind != 'Extra') else None)
        elif n.kind in ('ret', 'tail'):
            fa.ret_nodes.append(n.id)
            jr = n.insn
            if n.kind == 'ret':
                rv = sget(regs, jr.rs)
                if rv != ('o', RA): fa.reasons.append('return through non-ra value at %08x' % n.pc)
            sv = sget(regs, SP)
            if sv != ('s', 0):
                if sv == T: fa.sp_dynamic = True
                fa.reasons.append('sp not balanced at %s %08x' % (n.kind, n.pc))
            for r in range(1, NRES):
                if r == SP or r == RA: continue
                v = sget(regs, r)
                if v != ('o', r): mod |= 1 << r
            if sget(regs, RA) != ('o', RA) and n.kind == 'tail':
                fa.reasons.append('ra not restored at tail %08x' % n.pc)
            if n.kind == 'tail':
                mod |= S.mw_of(n.tgt)
                fa.W = max(fa.W, S.W_of(n.tgt))
    fa.mod = mod
    # va_start pattern: the prologue spills several incoming argument registers (a2..a7) with their entry values
    spilled = set()
    for n in nodes:
        if n.kind not in ('n', 'slot') or sin[n.id] is None: continue
        d = n.insn
        if d.kind == 'Sw' and 6 <= d.rt <= 11:
            if sget(sin[n.id][0], d.rt) == ('o', d.rt): spilled.add(d.rt)
    fa.variadic = len(spilled) >= 4
    return fa


# ================================================================================================= program level
class Summary:
    __slots__ = ('Aw', 'variadic', 'why', 'W', 'entry', 'f', 'fa', 'mw', 'A', 'R', 'typed', 'reasons', 'size', 'rset', 'ret_use', 'at', 'hook',
                 'a_reasons', 'form_locked')
    def __init__(self, f):
        self.entry = f.entry; self.f = f; self.fa = None; self.W = 0; self.variadic = False; self.Aw = 0
        self.mw = 0; self.A = 0; self.R = 1 << V0; self.typed = True; self.reasons = []
        self.size = f.size_words; self.rset = 0; self.ret_use = 0; self.at = False; self.hook = None
        self.a_reasons = []; self.form_locked = False

class Program2:
    """Container for the whole-program analysis results."""
    def __init__(self, prog):
        self.prog = prog
        self.im = prog.im
        self.S = {e: Summary(f) for e, f in prog.funcs.items()}
        self.imports = prog.im.imports
        self.mw_ind = IMPORT_MW | CALLER_SAVED_MASK | (1 << 1)
        self.W_ind = 0
        self.argunion = 0                   # arguments any address-taken function reads
        self.J = 0                          # registers live after any indirect call
        self.at_set = set()
        ptrs = set(prog.hilo) | set(prog.data32)
        for e in self.S:
            if e in ptrs or e == prog.im.entry: self.at_set.add(e); self.S[e].at = True
        self.callers = collections.defaultdict(set)
        for e, f in prog.funcs.items():
            for t in f.callees: self.callers[t].add(e)
        self.jalr_funcs = {e for e, f in prog.funcs.items() if f.has_ind_call}

    # --- queries used by the abstract interpretation
    def W_of(self, t):
        s = self.S.get(t)
        return INF if s is None else s.W

    def mw_of(self, t):
        if t in self.imports: return IMPORT_MW
        s = self.S.get(t)
        if s is None: return RES_MASK & ~(1 << SP)
        return s.mw

def postorder(P):
    """callee-first order of function entries (iterative DFS over direct call edges)."""
    seen = set(); order = []
    for root in sorted(P.S):
        if root in seen: continue
        stack = [(root, iter(sorted(P.prog.funcs[root].callees)))]
        seen.add(root)
        while stack:
            e, it = stack[-1]
            for t in it:
                if t in P.S and t not in seen:
                    seen.add(t); stack.append((t, iter(sorted(P.prog.funcs[t].callees)))); break
            else:
                order.append(e); stack.pop()
    return order

def phase1(P, verbose=True):
    """MW fixpoint over the call graph (abstract interpretation of every function)."""
    order = postorder(P)
    queue = collections.deque(order); inq = set(order)
    n_runs = 0
    while queue:
        e = queue.popleft(); inq.discard(e)
        s = P.S[e]
        fa = analyze_function_abs(s.f, P, P.mw_ind)
        n_runs += 1
        old = s.mw
        s.fa = fa
        s.reasons = list(fa.reasons)
        s.variadic = fa.variadic
        new = old | fa.mod
        if fa.W > s.W:
            s.W = fa.W
            for c in P.callers.get(e, ()):
                if c not in inq: queue.append(c); inq.add(c)
            if s.at and s.W > P.W_ind:
                P.W_ind = s.W
                for c in P.jalr_funcs:
                    if c not in inq: queue.append(c); inq.add(c)
        if new != old:
            s.mw = new
            for c in P.callers.get(e, ()):
                if c not in inq: queue.append(c); inq.add(c)

        if verbose and n_runs % 2000 == 0: print('  phase1 runs', n_runs, 'queue', len(queue), file=sys.stderr)
    return n_runs
