"""Run discovery + whole-program analysis and return the solved Program2 / Solver (shared by lean_gen.py and the debug scripts)."""
import sys, time, json, pickle, os
from . import image, discover, analysis, live, fuse

def load_profile(path):
    return json.load(open(path)) if path and os.path.exists(path) else {}

def discover_program(profile, verbose=False, im=None):
    im = im or image.Image()
    prog = discover.discover(im, profile=profile)
    discover.build_all(prog)
    n = fuse.apply(prog)
    if verbose: print('fused unaligned pairs:', n, file=sys.stderr)
    return prog

def analyze(prog, profile, verbose=False):
    live.set_weak_args(int(profile.get('weak_args', 4)))
    live.set_jalr_weak(profile.get('jalr_weak_all', True))
    P = analysis.Program2(prog)
    t0 = time.time()
    analysis.phase1(P, verbose=verbose)
    if verbose: print('phase1 %.1fs' % (time.time() - t0), file=sys.stderr)
    L = live.Solver(P)
    for k, v in profile.get('import_arity', {}).items():
        L.import_arity[int(k, 0)] = (int(v.get('gpr', 0)), int(v.get('fpr', 0)))
    for k, v in profile.get('extra_inputs', {}).items():
        L.extra_inputs[int(k, 0)] = live.mask_of([int(x) for x in v])
    t0 = time.time()
    L.solve(verbose=verbose)
    cnt = L.decide()
    if verbose: print('solve %.1fs' % (time.time() - t0), cnt, file=sys.stderr)
    return P, L
