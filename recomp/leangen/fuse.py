"""Fusion of unaligned access pairs.

`lwr rt,o(b) ... lwl rt,o+3(b)` (either order) is one unaligned word load; `swr/swl` likewise one unaligned store.  The
interpreter executes the halves separately.  Fusing is valid when the second half is reached from the first along a
single straight-line chain of plain instructions (each with exactly one predecessor) that neither read nor write `rt`,
nor write the base `b`, and that cannot change the memory the pair touches (no stores between a load pair; no memory
access at all between a store pair), and rt != base.

For loads the data flow changes: the pair reads only `b` and defines `rt` (the halves' mutual read of `rt` is not a real
input).  The first half becomes a no-op and the second half performs the whole access.
Marks `Insn.fuse` (1 = first half, 2 = second half, which does the work).
"""
LOAD = {'Lwl', 'Lwr'}
STORE = {'Swl', 'Swr'}
WINDOW = 10

def _mem_kind(d):
    """'st' stores / unknown side effects, 'ld' loads, None otherwise."""
    if d.mem == 'st': return 'st'
    if d.mem == 'ld': return 'ld'
    return None

def apply(prog):
    cand = {}          # first pc -> (d, e) ; only if valid in every function that contains the first node
    bad = set()
    for f in prog.funcs.values():
        nodes = f.nodes
        for n in nodes:
            d = n.insn
            if d is None or d.kind not in LOAD | STORE: continue
            if n.kind != 'n' or not isinstance(n.key, int):
                bad.add(d.pc); continue
            is_load = d.kind in LOAD
            same = LOAD if is_load else STORE
            cur = n; found = None
            for _ in range(WINDOW):
                if len(cur.succ) != 1: break
                nx = nodes[cur.succ[0]]
                if nx.kind != 'n' or not isinstance(nx.key, int) or nx.insn is None or nx.pred != [cur.id]: break
                e = nx.insn
                if e.kind in same and e.kind != d.kind and e.rt == d.rt and e.rs == d.rs:
                    found = e; break
                # intermediate instruction must be transparent
                if e.ctl or e.kind in ('Break', 'Syscall'): break
                if d.rt in e.uses or d.rt in e.defs or d.rs in e.defs: break
                if e.kind in ('Lwl', 'Lwr', 'Swl', 'Swr'): break
                mk = _mem_kind(e)
                if mk == 'st' or (not is_load and mk is not None): break
                if e.vfpu and not is_load: break
                if e.vfpu and e.kind not in ('Lvs', 'Lvq', 'Vpfx', 'Mtv', 'Mfv') and is_load: pass
                cur = nx
            if found is None: bad.add(d.pc); continue
            e = found
            if d.rt == 0 or d.rs == d.rt: bad.add(d.pc); continue
            lr = d if d.kind in ('Lwr', 'Swr') else e
            ll = e if lr is d else d
            if ll.simm != lr.simm + 3: bad.add(d.pc); continue
            prev = cand.get(d.pc)
            if prev is not None and prev[1] is not e: bad.add(d.pc)
            cand[d.pc] = (d, e)
    pairs = []
    used = set()
    for pc in sorted(cand):
        if pc in bad: continue
        d, e = cand[pc]
        if pc in used or e.pc in used or e.pc in bad and False: continue
        # the second half must not itself be a first half of another pair
        if e.pc in cand and e.pc not in bad: continue
        used.add(pc); used.add(e.pc)
        pairs.append((d, e))
    for d, e in pairs:
        d.fuse = 1; e.fuse = 2
        if d.kind in LOAD:
            d.uses = frozenset(); d.defs = frozenset()
            e.uses = frozenset({e.rs} - {0}); e.defs = frozenset({e.rt} - {0})
        else:
            d.uses = frozenset()
    return len(pairs)
