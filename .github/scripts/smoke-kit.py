#!/usr/bin/env python3
"""Smoke test of an unpacked builder kit, with no game disc: runs every step of a build on a tiny synthetic corpus.

    python3 -I smoke-kit.py KIT_DIR WORK_DIR OUT_DIR [--jobs N] [--corpus DIR] [--keep]

It follows kit.json the way the builder does: compile (cc1plus, -flto) -> archive (gcc-ar with the LTO plugin) -> LTO link of
the real application objects with the synthetic game library (lto-wrapper, lto1, collect2, as, ld) -> nm check -> room check
for vita-elf-create (and the pad relink when needed) -> vita-elf-create -> vita-make-fself -> infected.vpk.
It runs in a clean environment (empty PATH: no make, no VitaSDK, nothing of the host's compilers), like a player's computer.

Prints "segments-sha256" (the program without debug info: equal on every host), "eboot-sha256" and "vpk-sha256" at the end. Nothing here is game data: the corpus is .github/smoke/gen.
"""
import concurrent.futures, glob, hashlib, json, os, re, shutil, subprocess, sys, time, zipfile

def log(*a):
    print(*a, flush=True)

def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()

def segments_sha256(path):
    """sha256 over the program headers and the bytes of every segment of an ELF32 little-endian file: the program itself,
    without the debug sections (which carry the build folder's name) -- what must be the same on every host."""
    import struct
    d = open(path, 'rb').read()
    phoff, = struct.unpack_from('<I', d, 28)
    phentsize, phnum = struct.unpack_from('<HH', d, 42)
    h = hashlib.sha256()
    for i in range(phnum):
        t, off, va, pa, fsz, msz, fl, al = struct.unpack_from('<8I', d, phoff + i * phentsize)
        h.update(struct.pack('<8I', t, 0, va, pa, fsz, msz, fl, al))
        h.update(d[off:off + fsz])
        log('segment %d type=%#x vaddr=%#x filesz=%d memsz=%d flags=%#x sha256=%s' % (i, t, va, fsz, msz, fl, hashlib.sha256(d[off:off + fsz]).hexdigest()[:16]))
    return h.hexdigest()

def strip_velf(path):
    """kit.json elfCreate.stripSections (builder/cmd/infected-build/velf.go does the same): keep the ELF header, the program headers and
    the bytes of the segments, zero everything else, cut the file after the last segment. vita-make-fself hashes the whole file into the
    module NID, and the symbol table inside it is in another order when ld ran on Windows."""
    import struct
    d = open(path, 'rb').read()
    assert d[:6] == b'\x7fELF\x01\x01', 'not a 32-bit little-endian ELF'
    phoff, = struct.unpack_from('<I', d, 28)
    phentsize, phnum = struct.unpack_from('<HH', d, 42)
    keep = [(0, 52), (phoff, phoff + phnum * phentsize)]
    for i in range(phnum):
        off, = struct.unpack_from('<I', d, phoff + i * phentsize + 4)
        fsz, = struct.unpack_from('<I', d, phoff + i * phentsize + 16)
        if fsz:
            keep.append((off, off + fsz))
    out = bytearray(max(b for _, b in keep))
    for a, b in keep:
        out[a:b] = d[a:b]
    struct.pack_into('<I', out, 32, 0)
    struct.pack_into('<HHH', out, 46, 0, 0, 0)
    open(path, 'wb').write(bytes(out))

def main():
    args = sys.argv[1:]
    keep = False
    jobs, corpus = 2, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'smoke', 'gen')
    pos = []
    while args:
        a = args.pop(0)
        if a == '--jobs': jobs = int(args.pop(0))
        elif a == '--corpus': corpus = args.pop(0)
        elif a == '--keep': keep = True
        else: pos.append(a)
    if len(pos) != 3:
        sys.exit(__doc__)
    kit, work, out = (os.path.abspath(p) for p in pos)
    corpus = os.path.abspath(corpus)
    r = json.load(open(os.path.join(kit, 'kit.json')))
    tool = lambda n: os.path.join(kit, *r['tools'][n].split('/'))
    for n in ('cxx', 'ar', 'nm', 'readelf', 'elfCreate', 'fself'):
        if not os.path.isfile(tool(n)):
            sys.exit('kit is incomplete: %s' % tool(n))

    shutil.rmtree(work, ignore_errors=True)
    for d in ('inc', 'gen', 'obj', 'tmp', 'emptybin'):
        os.makedirs(os.path.join(work, d))
    os.makedirs(out, exist_ok=True)
    shutil.copytree(os.path.join(kit, 'game', 'include'), os.path.join(work, 'inc'), dirs_exist_ok=True)
    shutil.copytree(corpus, os.path.join(work, 'gen'), dirs_exist_ok=True)

    env = {'PATH': os.path.join(work, 'emptybin'), 'TMPDIR': os.path.join(work, 'tmp'), 'TEMP': os.path.join(work, 'tmp'),
           'TMP': os.path.join(work, 'tmp'), 'LC_ALL': 'C'}
    if sys.platform.startswith('linux'):
        env['PWD'] = '/proc/self/cwd'      # as the builder does: GCC streams the working directory into LTO objects, from $PWD when that names it
    for k in ('SYSTEMROOT', 'SystemRoot', 'windir', 'COMSPEC', 'HOME'):      # Windows needs these to start any program
        if k in os.environ: env[k] = os.environ[k]

    fw = lambda p: p.replace('\\', '/')
    # {kit} and {work} are the prefix maps' folders (forward slashes: GCC on Windows spells its own paths that way, and the maps are text)
    fill = lambda x: x.replace('{kit}', fw(kit)).replace('{work}', fw(work))

    def run(label, argv, cwd, capture=False):
        t = time.time()
        p = subprocess.run(argv, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
        if p.returncode != 0:
            log('FAILED: %s (exit %d)\n%s' % (label, p.returncode, p.stdout))
            sys.exit(1)
        if p.stdout.strip() and not capture:
            log(p.stdout.rstrip()[-2000:])
        log('   %s: %.1f s' % (label, time.time() - t))
        return p.stdout

    # ---- compile
    log('== compile')
    sources = []
    for pat in r['compile']['sources']:
        sources += sorted(glob.glob(os.path.join(work, 'gen', pat)))
    stems = [os.path.basename(s)[:-4] for s in sources]
    log('   %d files' % len(stems))
    def compile_one(stem):
        a = [fill(x).replace('{stem}', stem) for x in r['compile']['args']]
        run('compile ' + stem, [tool('cxx')] + a, work)
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        list(ex.map(compile_one, stems))

    # ---- archive
    log('== archive')
    lib = os.path.join(work, 'liblean_vita_lto.a')
    objs = []
    for pat in r['archive']['order']:
        objs += sorted(glob.glob(os.path.join(work, 'obj', pat)))
    run('archive', [tool('ar'), 'rcs', lib] + objs, kit)

    # ---- link
    elf = os.path.join(work, 'infected.elf')
    link_args = [l for l in open(os.path.join(kit, 'game', 'link', 'link.args')).read().split('\n') if l != '']
    link_args += r['link'].get('extraArgs', [])      # the prefix maps and -g0, after link.args, as the builder does
    def link(label, pad_obj=None):
        a = []
        for l in link_args:
            l = fill(l)
            for k, v in (('{lean}', lib), ('{out}', elf), ('{jobs}', str(jobs))):
                l = l.replace(k, v)
            if pad_obj and l == r['textPad']['object']:
                l = pad_obj
            a.append(l)
        run(label, [tool('cxx')] + a, kit)
    log('== link (LTO)')
    link('link')

    log('== check for weak pthread functions')
    nm = run('nm', [tool('nm'), elf], kit, capture=True)
    rx = re.compile(r['postLinkCheck']['failIfLineMatches'])
    bad = [l for l in nm.splitlines() if rx.search(l)]
    if bad:
        sys.exit('error: %s\n%s' % (r['postLinkCheck']['message'], '\n'.join(bad)))

    log('== room after the code segment')
    def free_room():
        t = run('readelf', [tool('readelf'), '-lW', elf], kit, capture=True)
        loads = [l.split() for l in t.splitlines() if l.strip().startswith('LOAD')]
        return int(loads[1][2], 16) - (int(loads[0][2], 16) + int(loads[0][4], 16)) if len(loads) > 1 else 1 << 20
    need = r['textPad']['minFreeBytes']
    free = free_room()
    log('   free: %d bytes (need %d)' % (free, need))
    if free < need:
        pad = r['textPad']['hint'] + need + 4096 - free
        log('   padding .text by %d bytes and linking again' % pad)
        pobj = os.path.join(work, 'text_pad.o')
        fix = [fill(x).replace('{pad}', str(pad)).replace('{padObject}', pobj) for x in r['textPad']['fix']['args']]
        run('textpad', [tool('cxx')] + fix, kit)
        link('link2', pobj)

    # ---- vita-elf-create, vita-make-fself
    log('== vita-elf-create, vita-make-fself')
    velf, eboot = os.path.join(work, 'infected.velf'), os.path.join(out, 'eboot.bin')
    # vita-elf-create writes the module name (the ELF's file name) into the program by splitting the path at '/': on Windows a path
    # with backslashes would give "\infected.elf" instead of "infected.elf", so these three paths are given with forward slashes
    # (Windows accepts them). The builder must do the same.
    run('elf-create', [tool('elfCreate')] + [x.replace('{elf}', fw(elf)).replace('{velf}', fw(velf)) for x in r['elfCreate']['args']], kit)
    if r['elfCreate'].get('stripSections'):
        strip_velf(velf)
    run('fself', [tool('fself')] + [x.replace('{velf}', fw(velf)).replace('{eboot}', fw(eboot)) for x in r['fself']['args']], kit)

    # ---- the package
    log('== package')
    vpk = os.path.join(out, r['vpk']['zip'])
    if os.path.exists(vpk): os.remove(vpk)
    with zipfile.ZipFile(vpk, 'w', zipfile.ZIP_DEFLATED) as z:
        for name in r['vpk']['entries']:
            src = eboot if name == r['vpk']['eboot'] else os.path.join(kit, 'game', 'vpk', *name.split('/'))
            zi = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = 0o644 << 16
            with open(src, 'rb') as f:
                z.writestr(zi, f.read())
    log('   %s (%d bytes)' % (vpk, os.path.getsize(vpk)))
    shutil.copyfile(velf, os.path.join(out, 'infected.velf'))      # kept for comparing hosts; the installer never sees this file
    log('segments-sha256 %s' % segments_sha256(velf))
    log('eboot-sha256 %s' % sha256(eboot))
    log('vpk-sha256 %s' % sha256(vpk))
    if not keep:
        shutil.rmtree(work, ignore_errors=True)

if __name__ == '__main__':
    main()
