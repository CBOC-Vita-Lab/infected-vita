"""Per-function control-flow graphs with delay slots, plus whole-program function discovery.

Node model (one node per executed 'instruction instance'):
  kind 'n'     plain instruction                      key pc
  kind 'br'    conditional branch decision            key pc        succ = [slot]            (non-likely branch)
                                                                      succ = [likely-slot, fall]   (likely branch)
  kind 'jmp'   j  (intra-function)                    key pc        succ = [slot]
  kind 'call'  jal/jalr call + slot                   key pc        succ = [slot];  slot -> ('C',pc) -> pc+8
  kind 'jr'    jr rX (return / switch / indirect)     key pc        succ = [slot]
  kind 'slot'  delay-slot instance                    key ('S',pc) or ('L',pc) (likely)
  kind 'cont'  call completion                        key ('C',pc)  succ = [pc+8]
  kind 'ret'   return after delay slot                key ('R',pc)
  kind 'tail'  tail call after delay slot / fallthrough into another function   key ('T',pc[,tgt])
  kind 'switch' jump-table dispatch after delay slot  key ('W',pc)
  kind 'ijmp'  unresolved indirect jump after slot    key ('J',pc)
  kind 'fatal' break / undecodable / runs off          key ('F',pc)
"""
import collections
from . import mips
from .mips import decode, CTRL, BRANCHES, BR_LIKELY

class Node:
    __slots__ = ('id', 'key', 'kind', 'pc', 'insn', 'succ', 'tgt', 'data', 'pred')
    def __init__(self, nid, key, kind, pc, insn):
        self.id = nid; self.key = key; self.kind = kind; self.pc = pc; self.insn = insn
        self.succ = []; self.tgt = None; self.data = None; self.pred = []
    def __repr__(self): return '<N%d %s %s %s>' % (self.id, self.kind, self.key if not isinstance(self.key, int) else '%08x' % self.key, self.insn.mn if self.insn else '')

class Func:
    def __init__(self, entry):
        self.entry = entry
        self.nodes = []             # list of Node, index == id
        self.by_key = {}
        self.issues = []            # structural notes
        self.tables = []            # recovered jump tables: dict(pc, table_addr, entries, bound)
        self.pcs = set()            # all instruction addresses belonging to the function
        self.callees = set()        # direct jal/tail targets
        self.has_ind_call = False
        self.size_words = 0

class Program:
    def __init__(self, im):
        self.im = im
        self.insn_cache = {}
        self.funcs = {}             # entry -> Func
        self.starts = set()
        self.table_words = {}       # addr of table word -> (table pc, index)
        self.case_labels = set()
        self.jal_targets = set()
        self.noreturn = set()       # functions that never return to their call site (calls to them end the path)
        self.reloc32_addrs = {a for (a, t, ps, ts) in im.relocs if t == 2}

    def ins(self, pc):
        d = self.insn_cache.get(pc)
        if d is None:
            if not (self.im.base <= pc < self.im.stub_end) or pc & 3:
                return None
            d = self.insn_cache[pc] = decode(pc, self.im.words[(pc - self.im.base) >> 2])
        return d

# ---------------------------------------------------------------- jump table recovery
def _find_def(prog, pc, reg, limit=14):
    """Search backwards (straight-line, same basic block approximation) for the instruction that defines `reg`
    before `pc`.  Returns the instruction or None.  Stops at branch targets? (we do not know them) -> bounded window."""
    p = pc - 4
    for _ in range(limit):
        d = prog.ins(p)
        if d is None: return None
        # a control instruction (other than the slot of the previous branch) ends the block
        pd = prog.ins(p - 4)
        if reg in d.defs and not d.ctl:
            return d
        if pd is not None and pd.delay and p - 4 >= 0:
            # d is a delay slot of pd; the block boundary is before pd
            if reg in d.defs: return d
            return None
        p -= 4
    return None

def _const_of(prog, pc, reg, depth=0):
    """Compile-time constant held by `reg` just before `pc` (lui / addiu / ori chains), else None."""
    if reg == 0: return 0
    if depth > 4: return None
    d = _find_def(prog, pc, reg, 24)
    if d is None: return None
    k = d.kind
    if k == 'Lui': return (d.imm << 16) & 0xFFFFFFFF
    if k == 'Addiu':
        c = _const_of(prog, d.pc, d.rs, depth + 1)
        return None if c is None else (c + d.simm) & 0xFFFFFFFF
    if k == 'Ori':
        c = _const_of(prog, d.pc, d.rs, depth + 1)
        return None if c is None else c | d.imm
    if k in ('Or', 'Addu') and d.rt == 0:
        return _const_of(prog, d.pc, d.rs, depth + 1)
    return None

def _scaled_index(prog, pc, reg):
    """If reg holds (idx << 2) computed by an `sll` just before pc, return (idx_reg, sll_pc)."""
    d = _find_def(prog, pc, reg, 24)
    if d is not None and d.kind == 'Sll' and d.sa == 2: return d.rt, d.pc
    return None

def recover_jump_table(prog, pc, d):
    """pc: address of `jr rX` (rX != ra).  -> dict(table, entries-less, bound, idx_reg) or None"""
    rs = d.rs
    ld = _find_def(prog, pc, rs)
    if ld is None or ld.kind != 'Lw': return None
    base = ld.rs; off = ld.simm
    tbl = None; idx = None; sll_pc = None
    ad = _find_def(prog, ld.pc, base)
    if ad is not None and ad.kind in ('Addu', 'Add'):
        for c_r, x_r in ((ad.rs, ad.rt), (ad.rt, ad.rs)):
            c = _const_of(prog, ad.pc, c_r)
            si = _scaled_index(prog, ad.pc, x_r)
            if c is not None and si is not None:
                tbl = (c + off) & 0xFFFFFFFF; idx, sll_pc = si
                break
    if tbl is None: return None
    bound = None
    p = sll_pc - 4
    for _ in range(28):
        q = prog.ins(p)
        if q is None: break
        if q.kind == 'Sltiu' and q.rs == idx:
            bound = q.simm & 0xFFFFFFFF; break
        if idx in q.defs and not q.ctl:
            if (q.kind in ('Or', 'Addu') and q.rt == 0) or (q.kind == 'Addiu' and q.simm == 0):
                idx = q.rs
            else:
                break
        p -= 4
    return dict(table=tbl, bound=bound, idx_reg=idx, jr_pc=pc, lw_pc=ld.pc, sll_pc=sll_pc)

def switch_mode(prog, data):
    """Decide how a jump table is lowered.  'idx': `switch (idx)` on the pre-scaling index register (the bound is known,
    equals the number of recovered entries and the register still holds the index at the jr, or holds idx << 2 when
    the sll is in place: data['shifted']).  'val': a value switch on the jr target register (always correct, slower)."""
    idx = data.get('idx_reg'); ents = data['entries']
    data['mode'] = 'val'; data['shifted'] = False
    if not idx or data.get('bound') is None or len(ents) != data['bound']: return
    pc = data['sll_pc'] + 4
    while pc <= data['jr_pc'] + 4:
        q = prog.ins(pc)
        if q is None or idx in q.defs or q.mn in ('jal', 'jalr'): return
        pc += 4
    q0 = prog.ins(data['sll_pc'])
    if q0 is None: return
    data['mode'] = 'idx'
    data['shifted'] = idx in q0.defs

def table_entries(prog, tbl, bound):
    """Read the jump table words.  Entries must carry an R_MIPS_32 relocation and point into code."""
    im = prog.im
    ents = []
    if not (im.base <= tbl < im.base + len(im.img)): return ents
    reloc32 = prog.reloc32_addrs
    n = bound if bound is not None else 4096
    for i in range(n):
        a = tbl + 4 * i
        if bound is None and a not in reloc32: break
        v = im.word(a)
        if not im.in_code(v):
            if bound is None: break
            return None
        ents.append(v)
    return ents

# ---------------------------------------------------------------- CFG construction
def build_function(prog, entry):
    im = prog.im
    f = Func(entry)
    starts = prog.starts
    nodes = f.nodes; by_key = f.by_key

    def mk(key, kind, pc, insn):
        n = by_key.get(key)
        if n is not None: return n
        n = Node(len(nodes), key, kind, pc, insn)
        nodes.append(n); by_key[key] = n
        return n

    work = [entry]
    done_pc = set()

    def pc_node(pc):
        """node for control arriving at address pc (plain flow). May create a tail node instead."""
        if pc != entry and pc in starts:
            return mk(('T', pc), 'tail', pc, None)
        return None

    def target_node(pc):
        t = pc_node(pc)
        if t is not None:
            t.tgt = pc; f.callees.add(pc)
            return t
        work.append(pc)
        return None  # resolved after, by key

    pending = []   # (node, kind, target pc) for edges to plain pc nodes, resolved at the end
    def edge_to(n, pc):
        t = pc_node(pc)
        if t is not None:
            t.tgt = pc; f.callees.add(pc)
            n.succ.append(t.id)
        else:
            work.append(pc)
            pending.append((n, pc))

    while work:
        pc = work.pop()
        if pc in done_pc: continue
        done_pc.add(pc)
        d = prog.ins(pc)
        if d is None or d.kind == 'Unsupported':
            n = mk(pc, 'fatal', pc, d); n.data = 'undecodable'
            continue
        f.pcs.add(pc)
        m = d.mn
        if not d.ctl:
            if d.kind == 'Break':
                n = mk(pc, 'fatal', pc, d); n.data = 'break'; continue
            if d.kind == 'Syscall':
                n = mk(pc, 'fatal', pc, d); n.data = 'syscall'; continue
            n = mk(pc, 'n', pc, d)
            edge_to(n, pc + 4)
            continue
        # control instruction with delay slot
        sd = prog.ins(pc + 4)
        if sd is None or sd.kind == 'Unsupported' or sd.ctl:
            n = mk(pc, 'fatal', pc, d); n.data = 'bad delay slot'; f.issues.append('bad delay slot at %08x' % pc); continue
        f.pcs.add(pc + 4)
        done_pc.add(pc + 4) if False else None
        if m in BRANCHES:
            t = mips.branch_target(pc, d)
            bc = mips.branch_const(d)
            n = mk(pc, 'br', pc, d); n.tgt = t; n.data = dict(const=bc)
            link = m in ('bltzal', 'bgezal', 'bltzall', 'bgezall')
            if link: f.issues.append('branch-and-link at %08x' % pc)
            if d.likely:
                sl = mk(('L', pc), 'slot', pc + 4, sd)
                # taken path: slot then target
                edge_to(sl, t)
                if bc is True:
                    n.succ = [sl.id]
                elif bc is False:
                    edge_to(n, pc + 8)
                else:
                    n.succ = [sl.id]
                    edge_to(n, pc + 8)
            else:
                sl = mk(('S', pc), 'slot', pc + 4, sd)
                n.succ = [sl.id]
                if bc is True: edge_to(sl, t)
                elif bc is False: edge_to(sl, pc + 8)
                else:
                    edge_to(sl, t); edge_to(sl, pc + 8)
            continue
        if m == 'j':
            t = mips.jump_target(pc, d)
            n = mk(pc, 'jmp', pc, d); n.tgt = t
            sl = mk(('S', pc), 'slot', pc + 4, sd); n.succ = [sl.id]
            edge_to(sl, t)
            continue
        if m in ('jal', 'jalr'):
            n = mk(pc, 'call', pc, d)
            sl = mk(('S', pc), 'slot', pc + 4, sd); n.succ = [sl.id]
            c = mk(('C', pc), 'cont', pc, d)
            sl.succ = [c.id]
            if m == 'jal':
                t = mips.jump_target(pc, d)
                c.tgt = t; f.callees.add(t); prog.jal_targets.add(t)
                if t in prog.noreturn:
                    c.data = 'noreturn'
                    continue
            else:
                f.has_ind_call = True
                if d.rd != 31: f.issues.append('jalr with link reg %d at %08x' % (d.rd, pc))
            edge_to(c, pc + 8)
            continue
        if m == 'jr':
            n = mk(pc, 'jr', pc, d)
            sl = mk(('S', pc), 'slot', pc + 4, sd); n.succ = [sl.id]
            if d.rs == 31:
                r = mk(('R', pc), 'ret', pc, d); sl.succ = [r.id]
            else:
                jt = recover_jump_table(prog, pc, d)
                ents = None
                if jt is not None:
                    ents = table_entries(prog, jt['table'], jt['bound'])
                if jt is not None and ents:
                    w = mk(('W', pc), 'switch', pc, d); sl.succ = [w.id]
                    w.data = dict(jt, entries=ents)
                    switch_mode(prog, w.data)
                    f.tables.append(w.data)
                    uniq = []
                    for e in ents:
                        if e not in uniq: uniq.append(e)
                    for e in uniq: edge_to(w, e)
                else:
                    j = mk(('J', pc), 'ijmp', pc, d); sl.succ = [j.id]
                    j.data = dict(why='no table' if jt is None else 'empty table')
            continue
        raise AssertionError(m)
    for n, pc in pending:
        t = by_key.get(pc)
        if t is None:
            raise AssertionError('missing node %08x' % pc)
        n.succ.append(t.id)
    # prune nodes unreachable from the entry (annulled likely slots with constant branches etc.)
    seen = set(); st = [by_key[entry].id]
    while st:
        i = st.pop()
        if i in seen: continue
        seen.add(i); st.extend(nodes[i].succ)
    if len(seen) != len(nodes):
        keep = sorted(seen); remap = {o: k for k, o in enumerate(keep)}
        new = [nodes[o] for o in keep]
        for k, n in enumerate(new):
            n.id = k; n.succ = [remap[x] for x in n.succ if x in remap]
        nodes[:] = new
        f.by_key = by_key = {n.key: n for n in nodes}
    for n in nodes:
        for s in n.succ: nodes[s].pred.append(n.id)
    f.pcs = {n.pc for n in nodes if n.insn is not None and isinstance(n.key, (int,)) or (n.insn is not None and n.kind == 'slot')}
    f.size_words = len(f.pcs)
    return f
