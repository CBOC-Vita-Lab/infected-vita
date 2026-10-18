"""Whole-program function discovery."""
import collections
from . import mips
from .cfg import Program, build_function

def prologue_seeds(im):
    """gcc-style prologues `addiu sp, sp, -N` right after a `jr ra` + delay slot (+ nop padding) or a `j` + slot."""
    W = im.words; B = im.base; out = set()
    for i in range(2, len(W) - 2):
        w = W[i]
        if (w >> 26) == 9 and ((w >> 16) & 31) == 29 and ((w >> 21) & 31) == 29 and (w & 0x8000):
            j = i - 1
            while j > 0 and W[j] == 0: j -= 1
            if j >= 1 and (W[j - 1] == 0x03E00008 or W[j] == 0x03E00008 or (W[j - 1] >> 26) == 2):
                out.add(B + 4 * i)
    return out

def discover(im, verbose=False, profile=None):
    prog = Program(im)
    prog.profile = profile or {}
    prog.noreturn = {int(x, 0) for x in prog.profile.get('noreturn', [])}
    prog.starts = {im.entry}
    prog.start_reasons = collections.defaultdict(set)
    cp = im.code_pointers()
    hilo = {a for a, k in cp.items() if 'hilo' in k}
    data32 = {a for a, k in cp.items() if 'data32' in k or 'text32' in k}
    prog.starts |= hilo
    # R_MIPS_26 jal targets declared by relocation + decoded jal instructions
    for (a, t, ps, ts) in im.relocs:
        if t == 4 and a < im.stub_end:
            w = im.word(a)
            if (w >> 26) == 3:
                tg = (im.base & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                if im.in_code(tg): prog.starts.add(tg); prog.start_reasons[tg].add('jal26')
    prog.starts |= prologue_seeds(im)
    prog.starts.add(im.base)
    # imports are not functions to translate
    for a in im.imports: prog.starts.discard(a)
    prog.data32 = data32
    prog.hilo = hilo
    prog.start_reasons[im.entry].add('entry')
    for a in hilo: prog.start_reasons[a].add('hilo')
    for a in prologue_seeds(im): prog.start_reasons[a].add('prologue')
    prog.start_reasons[im.base].add('base')
    return prog

def build_all(prog, verbose=False):
    """Build CFGs for every start; iterate until no new function starts (tail targets, jal targets, unclaimed pointers)."""
    im = prog.im
    built = {}
    rounds = 0
    pending_ptrs = set(prog.data32)
    while True:
        rounds += 1
        new = [s for s in sorted(prog.starts) if s not in built and not im.is_stub(s)]
        if not new and not pending_ptrs:
            break
        for s in new:
            built[s] = build_function(prog, s)
        prog.funcs = built
        # jump-table claimed words
        claimed = set()
        for f in built.values():
            for tb in f.tables:
                for e in tb['entries']: claimed.add(e)
        # data32 pointers not claimed by any table become function starts
        added = False
        for p in sorted(pending_ptrs):
            if p in claimed or p in prog.starts or im.is_stub(p): continue
            prog.starts.add(p); added = True
        pending_ptrs = set()
        # newly seen jal targets
        for t in sorted(prog.jal_targets):
            if t not in prog.starts and not im.is_stub(t):
                prog.starts.add(t); added = True
        # an edge classification depends on `starts`: rebuild functions whose bodies contain a newly added start
        if added:
            for s in list(built):
                f = built[s]
                if any((s2 != s and s2 in f.pcs) for s2 in prog.starts if s2 not in f.callees and s2 not in built):
                    pass
        if verbose: print('round', rounds, 'starts', len(prog.starts), 'built', len(built))
    # rebuild functions that now contain another start inside their body (those edges must become tail edges)
    for it in range(4):
        redo = []
        for s, f in built.items():
            for s2 in prog.starts:
                if s2 != s and s2 in f.pcs:
                    redo.append(s); break
        if not redo: break
        for s in redo:
            built[s] = build_function(prog, s)
        prog.funcs = built
        # new starts can appear again
        nn = [s for s in sorted(prog.jal_targets) if s not in prog.starts and not im.is_stub(s)]
        for s in nn: prog.starts.add(s)
        for s in sorted(prog.starts):
            if s not in built and not im.is_stub(s): built[s] = build_function(prog, s)
        prog.funcs = built
    prune_table_labels(prog, built)
    infer_noreturn(prog, built)
    return built

def returns_somewhere(f, noret):
    """does any ret / tail node stay reachable when calls to noreturn functions end their path?"""
    nodes = f.nodes
    seen = [False] * len(nodes)
    st = [f.by_key[f.entry].id]; seen[st[0]] = True
    while st:
        i = st.pop(); n = nodes[i]
        if n.kind == 'ret': return True
        if n.kind == 'tail' and n.tgt not in noret: return True
        if n.kind == 'cont' and n.tgt in noret: continue
        for s in n.succ:
            if not seen[s]: seen[s] = True; st.append(s)
    return False

def infer_noreturn(prog, built, profile_noreturn=()):
    """Functions with no reachable return never come back to the caller; the code after a call to them is dead."""
    noret = set(profile_noreturn) | {e for e in prog.noreturn}
    for it in range(8):
        new = {e for e, f in built.items() if e not in noret and not returns_somewhere(f, noret)}
        if not new: break
        noret |= new
    prog.noreturn = noret
    redo = [e for e, f in built.items() if any(n.kind == 'cont' and n.tgt in noret and n.data != 'noreturn' for n in f.nodes)]
    for e in redo: built[e] = build_function(prog, e)
    prog.funcs = built
    return noret

def table_word_set(built):
    tw = set()
    for f in built.values():
        for tb in f.tables:
            for i in range(len(tb['entries'])): tw.add(tb['table'] + 4 * i)
    return tw

def prune_table_labels(prog, built, verbose=False):
    """Starts that exist only because jump-table words (R_MIPS_32) point at them are case labels, not functions."""
    im = prog.im
    for it in range(6):
        tw = table_word_set(built)
        drop = []
        for s in list(prog.starts):
            if s not in built: continue
            r = prog.start_reasons.get(s, set())
            if r or s in prog.jal_targets: continue
            srcs = im.ptr_src.get(s, [])
            if srcs and all(a in tw for a in srcs):
                drop.append(s)
        if verbose: print('prune round', it, 'drop', len(drop))
        if not drop: break
        for s in drop:
            prog.starts.discard(s); built.pop(s, None)
        dropset = set(drop)
        redo = [e for e, f in built.items() if f.callees & dropset]
        for e in redo: built[e] = build_function(prog, e)
        prog.funcs = built
    return built
