#!/usr/bin/env python3
"""lean_gen.py - total static recompiler (guest PSP MIPS / Allegrex -> C++), "lean" form.

    python3 recomp/lean_gen.py --profile recomp/infected_profile.json --out private/gen

Writes into --out:
    lean_decls.h        prototypes of every f_ADDR / t_ADDR
    lean_NNN.cpp        translation units (functions in address order)
    lean_table.cpp      g_lean_entry[], g_lean_entry_count, hook switches
    manifest.json       per function: address, size, form, signature, reasons, hooks, TU
    (lean_abi.h / lean_vfpu.h stay in recomp/; compile with -I recomp -I <PortableKit>/include)

Every guest function gets a typed or regfile body plus a t_ADDR thunk and a g_lean_entry slot; a direct call target that
is not a known function would become an explicit pk_guest_call(pc) fallback listed in the manifest (none today).
"""
import io, argparse, collections, json, os, pickle, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.setrecursionlimit(200000)

from leangen import pipeline, analysis, abi, emit
from leangen.abi import params, rset, cpu_ref, ret_type, reg_name
from leangen.mips import F, SP

BASE = 0x08804000

class Emitter:
    def __init__(self, P, L, profile):
        self.P = P; self.L = L; self.profile = profile
        self.stats = collections.Counter()
        self.fallbacks = []
        self.no_vfpu_fast = False
        self.no_alias = False
        self.vfpu_forms = collections.Counter(); self.vfpu_generic = collections.Counter()
        self.vfpu_issues = []
        self.yield_set = {int(x, 0) for x in profile.get('yield', [])}
        self.hooks = {}
        for k, v in profile.get('hooks', {}).items():
            self.hooks[int(k, 0)] = v

    # ------------------------------------------------------------------ hooks
    HOOKS = {'memcpy': ('LEAN_HOOK_MEMCPY', 'lean_hook_memcpy', (4, 5, 6)),
             'memset': ('LEAN_HOOK_MEMSET', 'lean_hook_memset', (4, 5, 6)),
             'strcmp': ('LEAN_HOOK_STRCMP', 'lean_hook_strcmp', (4, 5))}

    def hook_prologue(self, fg):
        h = self.hooks.get(fg.e)
        if not h or not h.get('enabled', True): return []
        flag, fn, regs = self.HOOKS[h['name']]
        if not fg.typed or fg.rs_ret != [analysis.V0]:
            raise SystemExit('hook %s at %08x needs a typed function returning v0 only' % (h['name'], fg.e))
        for r in regs:
            if r not in fg.ps: raise SystemExit('hook %s at %08x: parameter r%d missing from the signature' % (h['name'], fg.e, r))
        for r in regs: fg.R(r)
        fg.R(analysis.V0)
        self.stats['hooks'] += 1
        return ['if (LEAN_LIKELY(!(g_lean_hook_off & %s))) return %s(%s);' % (flag, fn, ', '.join('r%d' % r for r in regs))]

    # ------------------------------------------------------------------ one function
    def function(self, e):
        s = self.P.S[e]
        fg = emit.FuncGen(self, e)
        lines = fg.gen()
        if fg.has_vfpu:
            for pc, kind, st in fg.vissues: self.vfpu_issues.append(dict(func=e, pc=pc, kind=kind, state=[None if x is None else x for x in st]))
        info = dict(addr=e, size=s.size, form='typed' if s.typed else 'regfile', at=bool(s.at), reasons=list(s.why),
                    hook=self.hooks.get(e, {}).get('name') if self.hooks.get(e, {}).get('enabled', True) else None,
                    stmts=sum(1 for l in lines if l.strip()))
        if fg.has_vfpu: info['vfpu'] = dict(fast=fg.vf_fast, generic=fg.vf_gen)
        if s.typed:
            info['signature'] = abi.signature(s, self.L)
            info['params'] = [reg_name(r) for r in params(s)]
            info['returns'] = [reg_name(r) for r in rset(self.L, s)]
            info['thunk'] = self.thunk(e)
        else:
            info['signature'] = 'void t_%08X()' % e
            info['params'] = [reg_name(r) for r in analysis.regs_in(s.A | s.Aw)]
            info['returns'] = [reg_name(r) for r in analysis.regs_in(self.L.rp(s)) if r < 67 and r not in (SP,)]
            info['regfile_loads'] = [reg_name(r) for r in fg.load_regs]
            info['regfile_stores'] = [reg_name(r) for r in fg.store_regs]
        return lines, info, fg

    def thunk(self, e):
        s = self.P.S[e]
        ps = params(s)
        args = ', '.join(cpu_ref(r) if not abi.is_fp(r) else cpu_ref(r) for r in ps)
        call = 'f_%08X(%s)' % (e, args)
        rr = rset(self.L, s)
        n = len(rr)
        out = ['void t_%08X() {' % e]
        if n == 0: out.append('  %s;' % call)
        elif n == 1:
            r = rr[0]
            out.append('  %s = %s;' % (cpu_ref(r), call))
        elif n == 2:
            out.append('  const uint64_t rv = %s;' % call)
            for i, r in enumerate(rr):
                x = 'lean_%s(rv)' % ('lo' if i == 0 else 'hi')
                out.append('  %s = %s;' % (cpu_ref(r), 'lean_u2f(%s)' % x if abi.is_fp(r) else x))
        else:
            out.append('  const lean_r3 rv = %s;' % call)
            out.append('  g_cpu.gpr[2] = lean_lo(rv.a); g_cpu.gpr[3] = lean_hi(rv.a); g_cpu.fpr[0] = lean_u2f(rv.b);')
        out.append('}')
        return '\n'.join(out)

def load_synth(path):
    """--synth FILE.py: FILE defines build() -> (SynthImage, profile dict); the focused tests use this instead of the game image"""
    import importlib.util
    spec = importlib.util.spec_from_file_location('synth_case', path)
    mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
    return mod.build()

def load_analysis(args, profile):
    if args.analysis and os.path.exists(args.analysis):
        t0 = time.time()
        P, L = pickle.load(open(args.analysis, 'rb'))
        print('loaded analysis %.1fs' % (time.time() - t0), file=sys.stderr)
        return P, L
    t0 = time.time()
    prog = pipeline.discover_program(profile, verbose=True, im=getattr(args, 'synth_image', None))
    print('discovery %.1fs: %d functions' % (time.time() - t0, len(prog.funcs)), file=sys.stderr)
    P, L = pipeline.analyze(prog, profile, verbose=True)
    if args.analysis:
        pickle.dump((P, L), open(args.analysis, 'wb'), protocol=4)
    return P, L

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--profile', default=os.path.join(HERE, 'infected_profile.json'))
    ap.add_argument('--out', required=True)
    ap.add_argument('--analysis', help='pickle cache of the solved analysis (speeds up emitter development)')
    ap.add_argument('--words-per-tu', type=int, default=9000)
    ap.add_argument('--only', help='comma separated function addresses (debugging)')
    ap.add_argument('--no-hooks', action='store_true')
    ap.add_argument('--synth', help='python file with build() -> (SynthImage, profile): generate for a synthetic image (focused tests)')
    ap.add_argument('--no-vfpu-fast', action='store_true', help='lower every VFPU instruction through the generic lean_vf:: helpers')
    ap.add_argument('--no-alias', action='store_true', help='compute the masked host address of every memory access separately (no shared base pointer per register)')
    args = ap.parse_args()
    profile = pipeline.load_profile(args.profile)
    args.synth_image = None
    if args.synth:
        args.synth_image, profile = load_synth(args.synth)
    if args.no_hooks: profile['hooks'] = {}
    P, L = load_analysis(args, profile)
    os.makedirs(args.out, exist_ok=True)
    if args.synth_image is not None:
        with open(os.path.join(args.out, 'synth.bin'), 'wb') as fh: fh.write(args.synth_image.img)
    E = Emitter(P, L, profile)
    E.no_vfpu_fast = args.no_vfpu_fast
    E.no_alias = args.no_alias
    entries = sorted(P.S)
    if args.only:
        want = {int(x, 0) for x in args.only.split(',')}
        entries = [e for e in entries if e in want]
    t0 = time.time()
    tus = []; cur = []; cur_words = 0
    manifest = []
    decls = []
    tu_of = {}
    for n, e in enumerate(entries):
        lines, info, fg = E.function(e)
        s = P.S[e]
        if s.typed:
            decls.append(abi.signature(s, L) + ';')
            decls.append('void t_%08X();' % e)
        else:
            decls.append('void t_%08X();' % e)
        text = '\n'.join(lines) + '\n'
        if s.typed: text += info.pop('thunk') + '\n'
        cur.append(text); cur_words += max(s.size, 1)
        info['tu'] = len(tus)
        manifest.append(info)
        if cur_words >= args.words_per_tu:
            tus.append(cur); cur = []; cur_words = 0
        if (n + 1) % 1000 == 0: print('  emitted %d/%d  %.1fs' % (n + 1, len(entries), time.time() - t0), file=sys.stderr)
    if cur: tus.append(cur)
    # fix tu index of the last chunk
    # (info['tu'] was assigned before the chunk closed: recompute from sizes)
    idx = 0; acc = 0
    for info in manifest:
        info['tu'] = idx
        acc += max(info['size'], 1)
        if acc >= args.words_per_tu: idx += 1; acc = 0
    hdr = ['// generated by recomp/lean_gen.py - do not edit', '#pragma once', '#include "lean_abi.h"', '']
    with wopen(os.path.join(args.out, 'lean_decls.h')) as fh: fh.write('\n'.join(hdr + decls) + '\n')
    for i, chunk in enumerate(tus):
        with wopen(os.path.join(args.out, 'lean_%03d.cpp' % i)) as fh:
            fh.write('// generated by recomp/lean_gen.py - do not edit\n#include "lean_decls.h"\n\n')
            fh.write('\n'.join(chunk))
    write_table(args.out, P, entries)
    write_test_meta(args.out, P, L, entries)
    with wopen(os.path.join(args.out, 'manifest.json')) as fh:
        json.dump(dict(functions=manifest, fallbacks=[dict(caller=hex(a), target=hex(b), reason=r) for a, b, r in E.fallbacks],
                       summary=summarize(P, manifest, entries, E), stats=dict(E.stats), vfpu=dict(fast_forms=dict(E.vfpu_forms), generic_kinds=dict(E.vfpu_generic),
                       impure_sites=E.vfpu_issues)), fh, indent=0)
    report(P, L, manifest, E, tus, entries)

def summarize(P, manifest, entries, E):
    """coverage numbers and the unclaimed words of the code range (null table slots: the runtime interprets those)"""
    claimed = sorted((m['addr'], m['addr'] + 4 * m['size']) for m in manifest)
    gaps = []; cur = BASE
    for a, b in claimed:
        if a > cur: gaps.append((cur, a))
        cur = max(cur, b)
    if cur < P.im.stub_end: gaps.append((cur, P.im.stub_end))
    c = collections.Counter(); w = collections.Counter()
    for m in manifest:
        c[m['form']] += 1; w[m['form']] += m['size']
    return dict(functions=len(manifest), typed=c['typed'], regfile=c['regfile'], fallback=len(E.fallbacks),
                words_typed=w['typed'], words_regfile=w['regfile'],
                code_range=[hex(BASE), hex(P.im.stub_end)], table_slots=(P.im.stub_end - BASE) // 4, table_slots_non_null=len(entries),
                unclaimed_words=sum((b - a) // 4 for a, b in gaps), unclaimed_ranges=[[hex(a), hex(b)] for a, b in gaps[:200]],
                unclaimed_ranges_total=len(gaps))

class wopen:
    """write a generated file only when its content changed (keeps timestamps, so incremental builds stay incremental)"""
    def __init__(self, path): self.path = path; self.buf = io.StringIO()
    def write(self, s): return self.buf.write(s)
    def __enter__(self): return self
    def __exit__(self, *a): self.close()
    def close(self):
        txt = self.buf.getvalue()
        try:
            with open(self.path) as f:
                if f.read() == txt: return
        except OSError:
            pass
        with open(self.path, 'w') as f: f.write(txt)

def write_table(out, P, entries):
    words = (P.im.stub_end - BASE) // 4
    ent = {((e - BASE) >> 2): e for e in entries}
    with wopen(os.path.join(out, 'lean_table.cpp')) as fh:
        fh.write('// generated by recomp/lean_gen.py - do not edit\n#include "lean_decls.h"\n#include <stdlib.h>\n#include <string.h>\n\n')
        fh.write('extern "C" {\nconst uint32_t g_lean_entry_count = %du;\n' % words)
        fh.write('uint32_t g_lean_hook_off = 0;\n')
        fh.write('void lean_init_hooks_from_env(void) {\n  const char* v = getenv("LEAN_NO_HOOKS");\n  if (!v) return;\n'
                 '  if (strstr(v, "memcpy") || strstr(v, "all")) g_lean_hook_off |= LEAN_HOOK_MEMCPY;\n'
                 '  if (strstr(v, "memset") || strstr(v, "all")) g_lean_hook_off |= LEAN_HOOK_MEMSET;\n'
                 '  if (strstr(v, "strcmp") || strstr(v, "all")) g_lean_hook_off |= LEAN_HOOK_STRCMP;\n}\n')
        fh.write('void (*const g_lean_entry[%d])() = {\n' % words)
        row = []
        for i in range(words):
            row.append('t_%08X' % ent[i] if i in ent else '0')
            if len(row) == 16:
                fh.write(','.join(row) + ',\n'); row = []
        if row: fh.write(','.join(row) + ',\n')
        fh.write('};\n}\n')

def write_test_meta(out, P, L, entries):
    """rows for the differential harness (recomp/tests): not part of the runtime libraries"""
    from leangen.analysis import regs_in
    with wopen(os.path.join(out, 'lean_test_meta.inc')) as fh:
        fh.write('// generated by recomp/lean_gen.py - do not edit\n')
        fh.write('// addr, form (0 typed, 1 regfile), returned gpr mask, returned fpr mask, address-taken, size in words,\n')
        fh.write('// strongly demanded gpr / fpr masks (v0, v1, f0 are junk pass-through where only forced by jalr read-back),\n')
        fh.write('// parameter gpr / fpr masks (everything else is not part of the calling contract)\n')
        strong = L.strong_rp()
        fh.write('static const uint32_t kCodeBegin = 0x%08XU, kCodeEnd = 0x%08XU;      // guest code: a store here is self-modifying code\n' % (P.im.base, P.im.stub_end))
        fh.write('static const uint32_t kGp = 0x%08XU;      // the constant $gp of the module\n' % P.im.gp)
        fh.write('static const TestRow kRows[] = {\n')
        for e in entries:
            s = P.S[e]
            if s.typed: rr = [r for r in rset(L, s)]
            else: rr = [r for r in regs_in(L.rp(s)) if r < 64 and r != SP]
            gm = sum(1 << r for r in rr if r < 32); fm = sum(1 << (r - 32) for r in rr if 32 <= r < 64)
            st = strong[e]
            sgm = (gm & ~0xC) | sum(1 << r for r in (2, 3) if st >> r & 1)
            sfm = (fm & ~1) | (1 if st >> 32 & 1 else 0)
            ps = params(s) if s.typed else [r for r in regs_in(s.A | s.Aw) if r != SP]
            pgm = sum(1 << r for r in ps if r < 32); pfm = sum(1 << (r - 32) for r in ps if 32 <= r < 64)
            fh.write('{0x%08XU,%d,0x%XU,0x%XU,%d,%d,0x%XU,0x%XU,0x%XU,0x%XU},\n' % (e, 0 if s.typed else 1, gm, fm, 1 if s.at else 0, s.size, sgm, sfm, pgm, pfm))
        fh.write('};\n')
        # per import: the arity every call site really defines (min over sites) -- the fake HLE of the harness hashes
        # only those arguments, because a site that defines fewer registers than the stub's widest site leaves the
        # rest undefined (the oracle sees junk there, lean code sees 0)
        from leangen.live import run_len
        from leangen.analysis import ARG_GPR, ARG_FPR
        amin = {}
        for e, s in P.S.items():
            fl = L.fl[e]
            if not fl.has_call_sites: continue
            md = fl.md if fl.md is not None else fl.compute_md(L.callee_defs)
            A, Aw, _li, _md = L.liveness(e)
            for nd in s.f.nodes:
                if nd.kind in ('cont', 'tail') and nd.tgt in P.imports:
                    have = md[nd.id] or 0          # defined by instructions of this function (parameters may carry junk)
                    ng, nf = run_len(have, ARG_GPR), run_len(have, ARG_FPR)
                    o = amin.get(nd.tgt)
                    amin[nd.tgt] = (ng, nf) if o is None else (min(o[0], ng), min(o[1], nf))
        fh.write('// stub, gpr args marshalled (widest site), fpr args marshalled, gpr args defined at every site, fpr likewise\n')
        fh.write('static const StubRow kStubs[] = {\n')
        for stub in sorted(P.imports):
            ar = L.ar.get(stub, 0)
            ng = sum(1 for i in range(4, 12) if ar >> i & 1)
            nf = sum(1 for i in range(12, 20) if ar >> (32 + i) & 1)
            mg, mf = amin.get(stub, (0, 0))
            fh.write('{0x%08XU,%d,%d,%d,%d},\n' % (stub, ng, nf, min(mg, ng), min(mf, nf)))
        fh.write('};\n')

def report(P, L, manifest, E, tus, entries):
    tot = sum(m['size'] for m in manifest)
    c = collections.Counter(); w = collections.Counter()
    for m in manifest:
        c[m['form']] += 1; w[m['form']] += m['size']
    print('functions: %d  typed %d  regfile %d  fallback %d' % (len(manifest), c['typed'], c['regfile'], len(E.fallbacks)), file=sys.stderr)
    print('guest words claimed: %d (typed %d, regfile %d)' % (tot, w['typed'], w['regfile']), file=sys.stderr)
    print('table slots non-null: %d of %d' % (len(entries), (P.im.stub_end - BASE) // 4), file=sys.stderr)
    print('translation units: %d  statements: %d' % (len(tus), sum(m['stmts'] for m in manifest)), file=sys.stderr)
    print('stats:', dict(E.stats), file=sys.stderr)

if __name__ == '__main__':
    main()
