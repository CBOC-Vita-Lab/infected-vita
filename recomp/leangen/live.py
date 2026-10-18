"""Liveness with dead-code elimination, interprocedural live-in sets A, demand R, return sets R', and the typed/regfile
decision.

Resources: bits 0..66 registers (see analysis.py), SLOT_BASE+i promoted stack slots, VTGT (jalr target temp).
"""
import collections, sys
from . import mips
from .mips import F, HI, LO, CC, RA, SP, GP
from .analysis import (V0, V1, F0, OUT_ENC, ARG_GPR, ARG_FPR, ARGS_MASK, ARGS_NOSP, IMPORT_MW, SLOT_BASE, RES_MASK,
                       CALLEE_SAVED_MASK, CALLER_SAVED_MASK, memref, regs_in, INF, postorder)

VTGT = 1 << 400
SPB = 1 << SP
RAB = 1 << RA
GPB = 1 << GP
MAX_PARAMS = 24
SIDE_KINDS = {'Add', 'Sub', 'Ctc1'}
ALL_FP_ARGS = 0
for _r in ARG_FPR: ALL_FP_ARGS |= 1 << _r

def mask_of(regs):
    m = 0
    for r in regs: m |= 1 << r
    return m

def insn_ud(d, mem_info):
    """-> (uses, defs, side) for a plain instruction instance, honouring promoted stack slots."""
    k = d.kind
    if mem_info is not None and mem_info[2] is not None:
        slot = 1 << (SLOT_BASE + mem_info[2])
        if k in ('Lw', 'Lwc1'):
            return slot, mask_of(d.defs), False
        return 1 << (d.rt if k == 'Sw' else F(d.rt)), slot, False
    uses = mask_of(d.uses) & ~GPB
    defs = mask_of(d.defs)
    side = bool(d.mem == 'st' or d.vfpu or k in SIDE_KINDS or k == 'Extra')
    return uses, defs, side

class FL:
    """Per-function static facts for the data-flow passes."""
    def __init__(self, f, fa):
        self.f = f; self.fa = fa
        nodes = f.nodes
        n = len(nodes)
        self.use = [0] * n; self.defs = [0] * n; self.side = [False] * n
        for nd in nodes:
            k = nd.kind
            i = nd.id
            if k in ('n', 'slot'):
                self.use[i], self.defs[i], self.side[i] = insn_ud(nd.insn, fa.mem.get(i))
            elif k == 'br':
                d = nd.insn
                self.use[i] = mask_of(d.uses) & ~GPB
                if d.mn.startswith('bc1'): self.use[i] |= 1 << CC
                self.defs[i] = mask_of(d.defs)
                self.side[i] = True
            elif k == 'call':
                d = nd.insn
                if d.mn == 'jal':
                    self.defs[i] = RAB
                else:
                    self.use[i] = (1 << d.rs) & ~GPB
                    self.defs[i] = (1 << d.rd) | VTGT
            elif k == 'switch':
                ir = nd.data.get('idx_reg') if nd.data.get('mode') == 'idx' else nd.insn.rs
                self.use[i] = ((1 << ir) & ~GPB) if ir else 0
                self.side[i] = True
        self.md = None
        self.order = None
        self.has_call_sites = any(nd.kind in ('cont', 'tail') for nd in nodes)

    def postorder(self):
        if self.order is None:
            nodes = self.f.nodes
            seen = [False] * len(nodes); order = []
            s0 = self.f.by_key[self.f.entry].id
            stack = [(s0, 0)]; seen[s0] = True
            while stack:
                i, j = stack[-1]
                su = nodes[i].succ
                if j < len(su):
                    stack[-1] = (i, j + 1)
                    s = su[j]
                    if not seen[s]: seen[s] = True; stack.append((s, 0))
                else:
                    order.append(i); stack.pop()
            self.order = order
        return self.order

    def compute_md(self, callee_defs):
        """forward may-defined registers at node entry (instruction and call results)."""
        nodes = self.f.nodes
        md = [None] * len(nodes)
        start = self.f.by_key[self.f.entry].id
        md[start] = 0
        work = collections.deque([start]); inq = {start}
        while work:
            i = work.popleft(); inq.discard(i)
            nd = nodes[i]; cur = md[i]
            k = nd.kind
            if k in ('n', 'slot', 'br', 'call'): out = cur | self.defs[i]
            elif k == 'cont':
                dc = callee_defs(nd)
                out = (cur & ~dc) | (dc & OUT_ENC)
            else: out = cur
            for s in nd.succ:
                old = md[s]
                new = out if old is None else (old | out)
                if new != old:
                    md[s] = new
                    if s not in inq: inq.add(s); work.append(s)
        self.md = md
        return md

def run_len(mask, regs):
    k = 0
    for r in regs:
        if mask >> r & 1: k += 1
        else: break
    return k

WSH = 512
STRONG = (1 << WSH) - 1
NOT_OUT = ~OUT_ENC & ((1 << 67) - 1)
# Registers whose value after a call can be a real dependency on the callee: its outputs and the callee-saved registers
# (an ABI violator that clobbers them on purpose).  A read of any other caller-saved register after a call is junk
# (the call clobbered it by the ABI); letting it create demand would infect whole call chains (see LOG.md).
DEMAND_NOT_OUT = NOT_OUT & CALLEE_SAVED_MASK
WEAK_ENTRY = 0
for _r in (4, 5, 6, 7, F(12), F(13), F(14), F(15)): WEAK_ENTRY |= 1 << _r

def set_weak_args(n):
    """how many argument registers (4 or 8) a weak use (import / jalr marshalling) may take as optional parameters"""
    global WEAK_ENTRY
    WEAK_ENTRY = 0
    for _r in range(4, 4 + n): WEAK_ENTRY |= 1 << _r
    for _r in range(12, 12 + n): WEAK_ENTRY |= 1 << F(_r)

JALR_WEAK_ALL = True

def set_jalr_weak(all_args):
    """an indirect call may take any of the weak argument registers (a0..a3 / f12..f15, or 8 of each) from the caller's own
    incoming values (thin wrappers that adjust `this` and jump through a vtable forward the rest untouched); False restricts the
    marshalling to the registers the function defines itself"""
    global JALR_WEAK_ALL
    JALR_WEAK_ALL = bool(all_args)

class Solver:
    def __init__(self, P, full_returns=False):
        self.P = P
        self.S = P.S
        self.imports = P.imports
        self.full = full_returns
        self.force_regs = 1 << V0       # returned by every address-taken function; grown by what jalr sites read back
        self.jalr_seen = 0
        self.fl = {e: FL(s.f, s.fa) for e, s in P.S.items()}
        self.ar = {}                         # import stub -> mask of marshalled arg registers
        self.import_arity = {}               # import stub -> (integer argument registers, float argument registers) from the profile (the API's prototype)
        self.J = 0                           # live after any indirect call (and its callee-visible subset)
        self.stats = collections.Counter()
        self.extra_inputs = {}
        for fl in self.fl.values():
            fl.dk = [d | (d << WSH) for d in fl.defs]
        for e, s in P.S.items():
            s.A = 0; s.Aw = 0; s.R = 0

    # ------------------------------------------------------------------ call model
    def callee_defs(self, nd):
        P = self.P
        if nd.kind == 'cont' and nd.insn.mn == 'jalr': return P.mw_ind
        t = nd.tgt
        if t is None: return P.mw_ind
        return P.mw_of(t)

    def call_uses(self, nd, md):
        """(strong, weak) register uses of a call-completion node (argument marshalling).
        Weak uses keep the defining instructions alive but never create return demand or entry parameters (beyond a0..a3)."""
        if md is None: md = ARGS_MASK
        if nd.kind == 'cont' and nd.insn.mn == 'jalr':
            return SPB, ARGS_NOSP & ((md | WEAK_ENTRY) if JALR_WEAK_ALL else md)
        t = nd.tgt
        s = self.S.get(t)
        if s is not None:
            if s.variadic: return s.A & SPB, (s.A | s.Aw) & ~SPB
            return s.A, s.Aw
        if t in self.imports:
            return SPB, ARGS_NOSP & (md | self.ar.get(t, 0))
        return SPB, ARGS_NOSP & md

    def prepare(self):
        """import argument arity from the call sites."""
        P = self.P
        arity = {}
        for e, fl in self.fl.items():
            if not fl.has_call_sites: continue
            md = fl.compute_md(self.callee_defs)
            sites = [nd for nd in fl.f.nodes if nd.kind in ('cont', 'tail') and nd.tgt in self.imports]
            for nd in sites:
                m = md[nd.id]
                if m is None: continue
                ni = run_len(m, ARG_GPR)
                nf = run_len(m, ARG_FPR)
                a, b = arity.get(nd.tgt, (0, 0))
                arity[nd.tgt] = (max(a, ni), max(b, nf))
        # The prototype of the syscall wins where it takes more than any call site defines: a thin wrapper that forwards
        # its own incoming arguments (the callee's second argument is the wrapper's a1) defines fewer registers than the
        # call takes, and an argument that is not marshalled reaches the HLE as stale g_cpu contents.
        for t, (ni, nf) in self.import_arity.items():
            if t not in self.imports: continue
            a, b = arity.get(t, (0, 0))
            if ni > a or nf > b: self.stats['import_arity_raised'] += 1
            arity[t] = (max(a, ni), max(b, nf))
        for t, (ni, nf) in arity.items():
            m = 0
            for r in ARG_GPR[:ni]: m |= 1 << r
            for r in ARG_FPR[:nf]: m |= 1 << r
            self.ar[t] = m
        self.arity = arity

    def param_mask(self, e):
        """registers a function may take as parameters: the argument registers plus profile-listed hidden inputs."""
        return ARGS_MASK | self.extra_inputs.get(e, 0)

    # ------------------------------------------------------------------ liveness
    def force_mask(self, s):
        if self.full: return OUT_ENC
        return self.force_regs if s.at else 0

    def rp(self, s):
        return (s.R | self.force_mask(s)) & s.mw

    def liveness(self, e, ret_extra=0):
        """Backward live-variable analysis with DCE.  Returns (A, live_in, md)."""
        S = self.S; s = S[e]; fl = self.fl[e]; f = s.f
        nodes = f.nodes
        N = len(nodes)
        rpself = self.rp(s)
        use = fl.use; dk = fl.dk; side = fl.side
        md = fl.md
        if md is None and fl.has_call_sites:
            md = fl.compute_md(self.callee_defs)
        live_in = [0] * N
        order = fl.postorder()
        work = collections.deque(order); inq = [False] * N
        for i in order: inq[i] = True
        preds = fl_preds(fl)
        kinds = [nd.kind for nd in nodes]
        ret_live = (rpself & OUT_ENC) | ((rpself & NOT_OUT) << WSH) | ret_extra
        call_uses = self.call_uses; callee_defs = self.callee_defs
        while work:
            i = work.popleft(); inq[i] = False
            nd = nodes[i]; k = kinds[i]
            out = 0
            for sx in nd.succ: out |= live_in[sx]
            if k == 'n' or k == 'slot':
                d_ = dk[i]
                if not side[i] and not (d_ & out):
                    new = out
                else:
                    new = (out & ~d_) | use[i]
            elif k == 'cont':
                us, uw = call_uses(nd, md[i] if md is not None else None)
                d0 = callee_defs(nd); d_ = d0 | (d0 << WSH)
                new = (out & ~d_) | us | (uw << WSH) | (out & d0 & DEMAND_NOT_OUT & STRONG)
            elif k == 'ret':
                new = ret_live
            elif k == 'tail':
                us, uw = call_uses(nd, md[i] if md is not None else None)
                d0 = callee_defs(nd); d_ = d0 | (d0 << WSH)
                new = (ret_live & ~d_) | us | (uw << WSH) | ((rpself & d0 & DEMAND_NOT_OUT) << WSH)
            elif k == 'fatal' or k == 'ijmp':
                new = 0
            elif k == 'br' or k == 'call' or k == 'switch':
                new = (out & ~dk[i]) | use[i]
            else:   # jmp, jr
                new = out
            if new != live_in[i]:
                live_in[i] = new
                for p in preds[i]:
                    if not inq[p]: inq[p] = True; work.append(p)
        li = live_in[f.by_key[f.entry].id]
        A = li & self.param_mask(e)
        Aw = ((li >> WSH) & WEAK_ENTRY) & ~A
        return A, Aw, live_in, md

    def run_function(self, e):
        """one liveness pass; propagate A to callers and demand to callees. Returns (A_changed, [callees whose R grew])."""
        S = self.S; s = S[e]
        A, Aw, live_in, md = self.liveness(e)
        changed = (A != s.A) or (Aw != s.Aw)
        s.A = A; s.Aw = Aw
        grew = []
        nodes = s.f.nodes
        rpself = self.rp(s)
        for nd in nodes:
            k = nd.kind
            if k != 'cont' and k != 'tail': continue
            if k == 'cont':
                out = 0
                for sx in nd.succ: out |= live_in[sx]
                out &= STRONG
            else:
                out = rpself
            if k == 'cont' and nd.insn.mn == 'jalr':
                self.jalr_seen |= out & OUT_ENC
                continue         # indirect callees return v0/v1/f0 only (contract); no demand
            t = nd.tgt
            ts = S.get(t)
            if ts is None: continue
            dem = out & ts.mw & (OUT_ENC | DEMAND_NOT_OUT)
            if dem & ~ts.R:
                ts.R |= dem; grew.append(t)
        return changed, grew

    def solve(self, verbose=True):
        """fixpoint of liveness / demand; address-taken functions return v0 plus whatever any jalr site reads back"""
        self.prepare()
        total = self._solve_once(verbose)
        while True:
            need = self.jalr_seen & OUT_ENC & ~self.force_regs
            if not need: break
            self.force_regs |= need
            total += self._solve_once(verbose)
        self.runs = total
        return total

    def _solve_once(self, verbose):
        P = self.P; S = self.S
        order = postorder(P)
        wl = collections.deque(order); inq = set(order)
        runs = 0
        while wl:
            e = wl.popleft(); inq.discard(e)
            ch, grew = self.run_function(e)
            runs += 1
            if ch:
                for c in P.callers.get(e, ()):
                    if c not in inq: inq.add(c); wl.append(c)
            for g in grew:
                if g not in inq: inq.add(g); wl.append(g)
            if verbose and runs % 3000 == 0: print('  live runs', runs, 'queue', len(wl), file=sys.stderr)
        return runs

    def strong_rp(self):
        """returned v0/v1/f0 per function when nothing is forced (no address-taken forcing): the registers some strong
        reader depends on.  Used by the differential harness to tell defined results from pass-through junk.
        State is restored afterwards."""
        S = self.S
        saved = {e: (s.R, s.A, s.Aw) for e, s in S.items()}
        keep = (self.force_regs, self.jalr_seen, self.full)
        for s in S.values(): s.R = 0
        self.force_regs = 0; self.full = False
        self._solve_once(False)
        out = {e: self.rp(s) & OUT_ENC for e, s in S.items()}
        for e, (r, a, aw) in saved.items():
            s = S[e]; s.R = r; s.A = a; s.Aw = aw
        self.force_regs, self.jalr_seen, self.full = keep
        return out

    # ------------------------------------------------------------------ decision
    def decide(self):
        S = self.S
        cnt = collections.Counter()
        srp = self.strong_rp()          # v0 / v1 / f0 some caller really reads (not merely forced by an indirect call)
        for e, s in S.items():
            why = []
            if s.reasons: why.append('structural: ' + s.reasons[0].split(' at ')[0])
            rp = self.rp(s)
            s.ret_use = rp
            if rp & ~OUT_ENC:
                why.append('returns ' + ','.join('r%d' % r for r in regs_in(rp & ~OUT_ENC)))
            elif srp[e]:
                # a path that returns the caller's untouched v0 / v1 / f0 while a caller reads it: only the register file form can
                # hand the incoming value back (the typed form would return its zero-initialised local)
                A_, Aw_, li_, md_ = self.liveness(e)
                x = li_[s.f.by_key[s.f.entry].id] & srp[e]
                if x: why.append('pass-through of ' + ','.join('r%d' % r for r in regs_in(x)))
            nparam = bin(s.A).count('1')
            if nparam > MAX_PARAMS: why.append('too many params')
            if s.f.issues and any(x.startswith('branch-and-link') for x in s.f.issues): why.append('branch-and-link')
            s.typed = not why
            s.why = why
            cnt['typed' if s.typed else 'regfile'] += 1
            for w in why: cnt['why ' + w.split(' ')[0] + ' ' + (w.split(':')[1].strip().split(' ')[0] if ':' in w else '')] += 1
        return cnt

def fl_preds(fl):
    p = getattr(fl, '_preds', None)
    if p is None:
        nodes = fl.f.nodes
        p = [[] for _ in nodes]
        for nd in nodes:
            for s in nd.succ: p[s].append(nd.id)
        fl._preds = p
    return p
