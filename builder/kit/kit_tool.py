#!/usr/bin/env python3
"""Helper of make-kit.sh (Python 3.8+, standard library only).  Run as: python3 -I kit_tool.py COMMAND ...

  game-link   turn a built CMake tree of vita/app into game/link (prebuilt objects, libraries, link.args)
  game-vpk    copy every file of the normal VPK except eboot.bin into game/vpk
  recipe      write kit.json
  host-copy   copy the host programs of a VitaSDK tree of one host OS into the kit's toolchain/
  trace       turn strace logs into lists/target-files.txt and lists/host-files.txt
  manifest    sha256 of every file of a folder (used by recipe)
"""
import argparse, glob, hashlib, json, os, re, shlex, shutil, stat, subprocess, sys

# --------------------------------------------------------------------------------------------------------- small helpers
def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()

def die(msg):
    sys.stderr.write('make-kit: ' + msg + '\n')
    sys.exit(1)

def cache_value(build, key):
    with open(os.path.join(build, 'CMakeCache.txt')) as f:
        for line in f:
            m = re.match(r'%s:[A-Z]+=(.*)$' % re.escape(key), line.rstrip('\n'))
            if m:
                return m.group(1)
    return None

def copy_file(src, dst, strip=None):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(src, dst)
    shutil.copymode(src, dst)
    if strip:
        subprocess.check_call([strip, '-D', '--strip-debug', dst])

def make_reproducible(path):
    os.utime(path, (0, 0))

# --------------------------------------------------------------------------------------------------------- game-link
def object_name(rel, pk, repo):
    """CMake object path (relative to the build tree) -> short stable name under game/link/obj."""
    rel = rel[:-4] if rel.endswith('.obj') else rel
    m = re.match(r'CMakeFiles/([^/]+)\.dir/(.*)$', rel) or re.match(r'[^/]+/CMakeFiles/([^/]+)\.dir/(.*)$', rel)
    tgt, rest = (m.group(1), m.group(2)) if m else ('x', rel)
    for prefix, tag in ((pk.lstrip('/'), 'pk'), (repo.lstrip('/'), 'proj')):
        if rest.startswith(prefix + '/'):
            return '%s/%s.o' % (tag, rest[len(prefix) + 1:])
    return '%s/%s.o' % (tgt, rest)

def cmd_game_link(a):
    build, kit = a.build, a.kit
    link = os.path.join(build, 'CMakeFiles', 'infected.dir', 'link.txt')
    toks = shlex.split(open(link).read().strip())
    lean_lib = cache_value(build, 'LEAN_LIB')
    pk = os.path.realpath(a.pk)
    repo = os.path.realpath(a.repo)
    out_obj = os.path.join(kit, 'game', 'link', 'obj')
    out_lib = os.path.join(kit, 'game', 'link', 'lib')
    shutil.rmtree(os.path.join(kit, 'game', 'link'), ignore_errors=True)
    os.makedirs(out_obj); os.makedirs(out_lib)
    ldirs = [t[2:] for t in toks if t.startswith('-L')]
    args, copied, libdir_emitted = [], {}, False
    i = 1                                   # toks[0] is the compiler
    while i < len(toks):
        t = toks[i]
        if t == '-o':
            args += ['-o', '{out}']; i += 2; continue
        if t.startswith('-flto='):
            args.append('-flto={jobs}'); i += 1; continue
        if t.startswith('-L'):
            if not libdir_emitted:
                args.append('-Lgame/link/lib'); libdir_emitted = True
            i += 1; continue
        if t.endswith('.obj') and not t.startswith('-'):
            name = object_name(t, pk, repo)
            copy_file(os.path.join(build, t), os.path.join(out_obj, name), a.strip)
            args.append('game/link/obj/' + name); i += 1; continue
        if t.endswith('.a') and not t.startswith('-'):
            src = t if os.path.isabs(t) else os.path.join(build, t)
            if lean_lib and os.path.realpath(src) == os.path.realpath(lean_lib):
                args.append('{lean}')
            else:
                base = os.path.basename(src)
                if base not in copied:
                    copy_file(src, os.path.join(out_lib, base), a.strip); copied[base] = True
                args.append('game/link/lib/' + base)
            i += 1; continue
        if t.startswith('-l') and len(t) > 2:
            for d in ldirs:                       # a library of the third-party sysroot goes into the kit; the SDK's own (stubs, -lm, -lpthread) stay in toolchain/
                p = os.path.join(d, 'lib%s.a' % t[2:])
                if os.path.exists(p):
                    base = os.path.basename(p)
                    if base not in copied:
                        copy_file(p, os.path.join(out_lib, base), a.strip); copied[base] = True
                    break
            args.append(t); i += 1; continue
        args.append(t); i += 1
    if '{lean}' not in args or '{out}' not in args:
        die('link.txt has no lean library or no -o: %s' % link)
    with open(os.path.join(kit, 'game', 'link', 'link.args'), 'w') as f:
        f.write('\n'.join(args) + '\n')
    print('game/link: %d link arguments, %d objects, %d libraries' % (len(args), sum(len(fs) for _, _, fs in os.walk(out_obj)), len(copied)))

# --------------------------------------------------------------------------------------------------------- game-vpk
def cmd_game_vpk(a):
    build, kit = a.build, a.kit
    mk = open(os.path.join(build, 'CMakeFiles', 'infected.vpk-vpk.dir', 'build.make')).read()
    pack = [l for l in mk.splitlines() if 'vita-pack-vpk' in l and ' -a ' in l]
    sfo = [l for l in mk.splitlines() if 'vita-mksfoex' in l]
    if len(pack) != 1 or len(sfo) != 1:
        die('cannot find vita-pack-vpk / vita-mksfoex in build.make')
    ptoks = shlex.split(pack[0].strip())
    stoks = shlex.split(sfo[0].strip())
    root = os.path.join(kit, 'game', 'vpk')
    shutil.rmtree(root, ignore_errors=True)
    entries = []
    j = 0
    while j < len(ptoks):
        if ptoks[j] == '-a':
            src, dest = ptoks[j + 1].rsplit('=', 1)
            copy_file(src, os.path.join(root, dest))
            entries.append(dest); j += 2
        else:
            j += 1
    # param.sfo: made by the CMake rule (vita-mksfoex with the flags below); the program is not needed at build time
    sfo_src = ptoks[ptoks.index('-s') + 1]
    copy_file(sfo_src, os.path.join(root, 'sce_sys', 'param.sfo'))
    # the order of the normal VPK: param.sfo, eboot.bin, then the -a files in CMake order
    order = ['sce_sys/param.sfo', 'eboot.bin'] + entries
    mkargs = stoks[1:-1]                    # drop the program and the output path; the last of these is the bubble name
    meta = {'entries': order, 'mksfoex': {'args': mkargs[:-1], 'name': mkargs[-1]}}
    with open(os.path.join(kit, 'game', 'vpk.json.tmp'), 'w') as f:
        json.dump(meta, f)
    print('game/vpk: %d files besides eboot.bin' % (len(order) - 1))

# --------------------------------------------------------------------------------------------------------- recipe
def parse_cmd(make_file, program):
    for l in open(make_file).read().splitlines():
        if program in l.split()[0:1] or (l.strip().startswith('/') and os.path.basename(shlex.split(l.strip())[0]) == program):
            return shlex.split(l.strip())
    die('no %s command in %s' % (program, make_file))

def pad_flags(build):
    """compiler flags of the infected target (flags.make) without the include paths; the compiler path"""
    vals = {}
    for l in open(os.path.join(build, 'CMakeFiles', 'infected.dir', 'flags.make')):
        m = re.match(r'(CXX_DEFINES|CXX_FLAGS) = (.*)$', l.rstrip('\n'))
        if m:
            vals[m.group(1)] = shlex.split(m.group(2))
    return vals.get('CXX_DEFINES', []) + vals.get('CXX_FLAGS', [])

def cmd_pad_object(a):
    cxx = os.path.join(a.sdk, 'bin', 'arm-vita-eabi-g++')
    src = os.path.join(a.repo, 'vita', 'app', 'src', 'text_pad.cpp')
    subprocess.check_call([cxx] + pad_flags(a.build) + ['-DINFECTED_TEXT_PAD=%d' % a.pad, '-c', src, '-o', a.out], cwd=a.build)
    if a.strip:
        subprocess.check_call([a.strip, '-D', '--strip-debug', a.out])

def text_pad_hint(build):
    v = cache_value(build, 'INFECTED_TEXT_PAD')
    return int(v) if v else 0

def cmd_recipe(a):
    kit = a.kit
    tool = lambda n: 'toolchain/bin/' + n
    # compile flags (recomp/build_lib.sh, LEAN_LTO=1)
    flags = shlex.split(a.lean_flags)
    cflags = []
    for f in flags:
        if f.startswith('-I'):
            continue
        cflags.append(f)
    compile_args = cflags + ['-frandom-seed={stem}', '-Iinc', '-Igen', '-c', 'gen/{stem}.cpp', '-o', 'obj/{stem}.o']
    # vita-elf-create / vita-make-fself exactly as CMake runs them
    velf = parse_cmd(os.path.join(a.build, 'CMakeFiles', 'infected-velf.dir', 'build.make'), 'vita-elf-create')
    fself = parse_cmd(os.path.join(a.build, 'CMakeFiles', 'infected.self-self.dir', 'build.make'), 'vita-make-fself')
    vpk = json.load(open(os.path.join(kit, 'game', 'vpk.json.tmp')))
    os.remove(os.path.join(kit, 'game', 'vpk.json.tmp'))
    # the text-pad object: how it is compiled (flags.make), so the program can redo it when the pad is not enough
    padflags = pad_flags(a.build)
    recipe = {
        'format': 1,
        'name': 'Infected-Vita-builder',
        'os': None,
        'arch': None,
        'game': {'titleId': cache_value(a.build, 'VITA_TITLEID'), 'name': cache_value(a.build, 'VITA_APP_NAME'),
                 'version': cache_value(a.build, 'VITA_VERSION'), 'bootSha256': a.boot_sha256},
        'sdk': {'gcc': '15.2.0', 'target': 'arm-vita-eabi', 'source': a.sdk_note},
        'notes': 'All commands run with the kit folder as the working directory. Placeholders in braces are replaced by the program; '
                 'a tool name is a path relative to the kit. PATH is not needed and not used: gcc finds its parts relative to its own location.',
        'tools': {'cxx': tool('arm-vita-eabi-g++'), 'ar': tool('arm-vita-eabi-gcc-ar'), 'nm': tool('arm-vita-eabi-nm'),
                  'readelf': tool('arm-vita-eabi-readelf'), 'elfCreate': tool('vita-elf-create'), 'fself': tool('vita-make-fself')},
        'profile': 'game/profile.json',
        'placeholders': {
                         '{stem}': 'a generated file name without .cpp (also the fixed -frandom-seed: LTO objects are only reproducible with one)', '{objs}': 'all objects (work/obj), in the order of "archive.order"',
                         '{lib}': 'the game archive made by "archive"', '{lean}': 'the same archive in link.args',
                         '{out}': 'the output ELF', '{jobs}': 'LTO partitions compiled in parallel (-flto=N)',
                         '{elf}': 'the linked ELF', '{velf}': 'the Sony ELF', '{eboot}': 'eboot.bin'},
        'compile': {'tool': 'cxx', 'args': compile_args, 'cwd': 'work',
                    'stage': {'inc': 'copy of the kit folder game/include', 'gen': 'the generated C++ (lean_NNN.cpp, lean_table.cpp, lean_decls.h)',
                              'obj': 'empty folder for the objects'},
                    'why': 'cwd is a work folder holding inc/, gen/ and obj/, with relative names, because GCC streams the names of the headers, the '
                           'absolute working directory and the absolute location of the toolchain into the LTO objects, and the final link orders a few '
                           'branch stubs by them. The eboot.bin is the same for the same toolchain folder and work folder; for other folders it differs '
                           'in some hundred bytes of .text (branch stub order), nothing else. -frandom-seed is needed for the same reason (the clock).',
                    'sources': ['lean_[0-9]*.cpp', 'lean_table.cpp'], 'parallel': True},
        'archive': {'tool': 'ar', 'args': ['rcs', '{lib}', '{objs}'], 'order': ['lean_[0-9]*.o', 'lean_table.o'],
                    'deleteFirst': True},
        'link': {'tool': 'cxx', 'argsFile': 'game/link/link.args', 'lean': '{lean}', 'out': '{out}', 'jobs': 2,
                 'tempDirEnv': 'TMPDIR', 'tempDirNote': 'the LTO link writes about 50 MB of temporary files; point TMPDIR (TEMP and TMP on Windows) at the work folder'},
        'postLinkCheck': {'tool': 'nm', 'args': ['{out}'], 'failIfLineMatches': '^ +w (pthread_|sched_)',
                          'message': 'pthread functions are weak and undefined: add them to the -u list (vita/app/CMakeLists.txt)'},
        'textPad': {'object': 'game/link/obj/infected/src/text_pad.cpp.o', 'hint': text_pad_hint(a.build), 'minFreeBytes': 8192,
                    'check': {'tool': 'readelf', 'args': ['-lW', '{elf}'],
                              'rule': 'LOAD[1].VirtAddr - (LOAD[0].VirtAddr + LOAD[0].FileSiz) must be >= minFreeBytes'},
                    'fix': {'newPad': 'oldPad + minFreeBytes + 4096 - free', 'source': 'game/link/text_pad.cpp',
                            'tool': 'cxx', 'args': padflags + ['-DINFECTED_TEXT_PAD={pad}', '-c', 'game/link/text_pad.cpp', '-o', '{padObject}'],
                            'then': 'link again with {padObject} in place of textPad.object'},
                    'note': 'hint = the pad the reference build ended with; link.args already uses an object made with it, so normally there is one link.'},
        'elfCreate': {'tool': 'elfCreate', 'args': velf[1:-2] + ['{elf}', '{velf}']},
        'fself': {'tool': 'fself', 'args': fself[1:-2] + ['{velf}', '{eboot}']},
        'vpk': {'zip': 'infected.vpk', 'method': 'deflate', 'entries': vpk['entries'], 'fromKit': 'game/vpk/', 'eboot': 'eboot.bin',
                'paramSfoMadeWith': {'program': 'vita-mksfoex', 'args': vpk['mksfoex']['args'] + [vpk['mksfoex']['name'], 'param.sfo']}},
        'minDiskBytes': 700000000, 'minDiskNote': 'measured peak of a build: about 220 MB in the work folder (generated C++ 29, objects 35, archive 35, ELF 29, velf 31, LTO temporaries 40, eboot 6, vpk 6); the unpacked kit is about 150 MB',
    }
    recipe.update(json.loads(a.extra))
    with open(os.path.join(kit, 'target.json'), 'w') as f:
        json.dump(recipe, f, indent=1)
    print('target.json written')

def cmd_finalize(a):
    """host stage: the recipe of the target stage + this host OS (tool names, hashes of every file) -> kit.json"""
    kit = a.kit
    recipe = json.load(open(os.path.join(kit, 'target.json')))
    os.remove(os.path.join(kit, 'target.json'))
    recipe['os'], recipe['arch'] = a.os, a.arch
    if a.os == 'windows':
        recipe['tools'] = {k: v + '.exe' for k, v in recipe['tools'].items()}
    files = {}
    for sub in ('game', 'toolchain', 'licenses'):
        base = os.path.join(kit, sub)
        for dp, dn, fn in os.walk(base):
            for n in sorted(fn):
                p = os.path.join(dp, n)
                files[os.path.relpath(p, kit).replace(os.sep, '/')] = {'sha256': sha256(p), 'size': os.path.getsize(p)}
    recipe['files'] = dict(sorted(files.items()))
    with open(os.path.join(kit, 'kit.json'), 'w') as f:
        json.dump(recipe, f, indent=1)
        f.write('\n')
    print('kit.json: %d files' % len(files))

# --------------------------------------------------------------------------------------------------------- host-copy
def read_list(path):
    out = []
    for l in open(path):
        l = l.split('#', 1)[0].strip()
        if l:
            out.append(l.split(None, 1))
    return out

def candidates(kind, path, os_name):
    """names a host file can have on a host OS: kind prog (executable), lib (shared library / plugin), data (same everywhere)."""
    base, ext = os.path.splitext(path) if kind == 'lib' else (path, '')
    if kind == 'prog':
        return [path + '.exe'] if os_name == 'windows' else [path]
    if kind == 'lib':
        return {'linux': [base + '.so'], 'windows': [base + '.dll', base + '.so'], 'mac': [base + '.dylib', base + '.so']}[os_name]
    return [path]

def cmd_host_copy(a):
    missing = []
    n = 0
    for kind, path in read_list(a.list):
        got = None
        for c in candidates(kind, path, a.os):
            p = os.path.join(a.host_sdk, c)
            if os.path.isfile(p):
                got = c; break
        if got is None:
            missing.append(path); continue
        dst = os.path.join(a.kit, 'toolchain', got)
        copy_file(os.path.join(a.host_sdk, got), dst)
        if kind != 'data' and a.os != 'windows' and a.strip and os.path.getsize(dst) > 0:
            subprocess.call([a.strip, '-x' if a.os == 'mac' else '--strip-unneeded', dst], stderr=subprocess.DEVNULL)
        os.chmod(dst, os.stat(dst).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH if kind != 'data' else os.stat(dst).st_mode)
        n += 1
    if missing:
        die('the host SDK %s lacks: %s' % (a.host_sdk, ', '.join(missing)))
    print('toolchain host part: %d files for %s' % (n, a.os))

# --------------------------------------------------------------------------------------------------------- trace
SYSCALL_OK = re.compile(r'^(?:\[pid\s+\d+\]\s+|\d+\s+)?(openat|open|execve)\((.*)\)\s+=\s+(-?\d+)')

def trace_paths(logs, sdk):
    sdk = os.path.realpath(sdk)
    found = set()
    for log in logs:
        for line in open(log, errors='replace'):
            m = SYSCALL_OK.match(line)
            if not m or int(m.group(3)) < 0:
                continue
            q = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(2))
            if not q:
                continue
            p = q[0]
            if not p.startswith('/'):
                continue
            p = os.path.normpath(p)
            real = os.path.realpath(p) if os.path.lexists(p) else p
            for cand in (p, real):
                if cand.startswith(sdk + '/'):
                    found.add(os.path.relpath(cand, sdk))
    return sorted(found)

def is_elf(path):
    with open(path, 'rb') as f:
        return f.read(4) == b'\x7fELF'

PURPOSE = {
    'bin/arm-vita-eabi-g++': 'the compiler driver: compiles, and links (runs collect2, lto-wrapper, ld)',
    'lib/gcc/arm-vita-eabi/15.2.0/cc1plus': 'the C++ compiler proper (compiling the 74 generated units)',
    'lib/gcc/arm-vita-eabi/15.2.0/lto1': 'the link-time optimiser (whole-program analysis and the partitions)',
    'lib/gcc/arm-vita-eabi/15.2.0/collect2': 'the link driver gcc calls instead of ld',
    'lib/gcc/arm-vita-eabi/15.2.0/lto-wrapper': 'runs lto1 for the link (and `make`, when there is one, to run partitions in parallel)',
    'lib/gcc/arm-vita-eabi/15.2.0/liblto_plugin': 'the linker plugin that hands the LTO objects to lto-wrapper',
    'arm-vita-eabi/bin/as': 'the assembler gcc runs on the code lto1 writes',
    'arm-vita-eabi/bin/ld': 'the linker (collect2 runs this one, not bin/arm-vita-eabi-ld)',
    'arm-vita-eabi/bin/ar': 'archive tool run by gcc-ar',
    'bin/arm-vita-eabi-gcc-ar': 'archive step: ar with the LTO plugin',
    'lib/bfd-plugins/liblto_plugin': 'the plugin ar loads to index LTO objects',
    'lib/bfd-plugins/libdep': 'loaded by ar together with the plugin above',
    'bin/arm-vita-eabi-nm': 'post-link check for weak pthread functions',
    'bin/arm-vita-eabi-readelf': 'measures the room after the code segment (text pad check)',
    'bin/vita-elf-create': 'ELF -> Sony ELF (imports/exports from the stub libraries)',
    'bin/vita-make-fself': 'Sony ELF -> eboot.bin',
}

def cmd_trace(a):
    rels = trace_paths(a.logs, a.sdk)
    prog, lib, data, tgt = [], [], [], []
    for r in rels:
        p = os.path.join(a.sdk, r)
        if not os.path.isfile(p):
            continue
        if is_elf(p):
            with open(p, 'rb') as f:
                f.seek(16); et = int.from_bytes(f.read(2), 'little')    # ET_REL=1 object, ET_EXEC=2, ET_DYN=3
            if et == 1:
                tgt.append(r)                                           # crt0.o, stubs members etc.: target
            elif et == 3 and r.endswith('.so') :
                lib.append(r)
            else:
                prog.append(r)
        elif r.startswith(('share/', 'etc/')):
            data.append(r)
        else:
            tgt.append(r)
    if a.exclude:
        ex = [l.strip() for l in open(a.exclude) if l.strip() and not l.startswith('#')]
        for lst in (prog, lib, data, tgt):
            lst[:] = [r for r in lst if not any(r.startswith(e) for e in ex)]
    with open(os.path.join(a.out, 'host-files.txt'), 'w') as f:
        f.write('# generated by make-kit.sh trace from strace logs of the real build steps; kind paths are relative to the VitaSDK root.\n')
        f.write('# prog = executable (".exe" is added on Windows), lib = shared library or plugin (.so / .dll / .dylib), data = read by a host program.\n')
        f.write('# Not needed (never opened): cc1, bin/arm-vita-eabi-ld, vita-mksfoex, vita-pack-vpk, psp2rela, gdb, lto-dump, objdump, strip, ranlib.\n')
        for k, lst in (('prog', prog), ('lib', [os.path.splitext(x)[0] for x in lib]), ('data', data)):
            for r in lst:
                f.write('%s %s%s\n' % (k, r, ('   # ' + PURPOSE[r]) if r in PURPOSE else ''))
    with open(os.path.join(a.out, 'target-files.txt'), 'w') as f:
        f.write('# generated by make-kit.sh trace: target files (headers, crt, libraries, linker scripts) the real build steps opened; relative to the VitaSDK root.\n')
        for r in tgt:
            f.write(r + '\n')
    print('trace: %d host programs, %d host libs, %d host data files, %d target files' % (len(prog), len(lib), len(data), len(tgt)))

def cmd_zip(a):
    import zipfile
    root = os.path.abspath(a.kit)
    top = os.path.basename(a.top) if a.top else ''
    names = []
    for dp, dn, fn in os.walk(root):
        dn.sort()
        for n in sorted(fn):
            names.append(os.path.join(dp, n))
    names.sort(key=lambda p: (os.path.relpath(p, root) != 'kit.json', os.path.relpath(p, root)))
    if os.path.exists(a.out): os.remove(a.out)
    with zipfile.ZipFile(a.out, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in names:
            rel = os.path.relpath(p, root).replace(os.sep, '/')
            st = os.stat(p)
            zi = zipfile.ZipInfo(rel, (2026, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = ((stat.S_IFREG | (st.st_mode & 0o777)) << 16)
            with open(p, 'rb') as f:
                z.writestr(zi, f.read(), zipfile.ZIP_DEFLATED, 9)
    print('%s: %d files, %d bytes' % (a.out, len(names), os.path.getsize(a.out)))

def cmd_manifest(a):
    for dp, dn, fn in os.walk(a.dir):
        for n in sorted(fn):
            p = os.path.join(dp, n)
            print(sha256(p), os.path.relpath(p, a.dir))

def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('game-link'); p.add_argument('--build', required=True); p.add_argument('--repo', required=True)
    p.add_argument('--pk', required=True); p.add_argument('--kit', required=True); p.add_argument('--strip', default=None)
    p = sub.add_parser('pad-object'); p.add_argument('--build', required=True); p.add_argument('--sdk', required=True); p.add_argument('--repo', required=True)
    p.add_argument('--pad', type=int, required=True); p.add_argument('--out', required=True); p.add_argument('--strip', default=None)
    p = sub.add_parser('game-vpk'); p.add_argument('--build', required=True); p.add_argument('--kit', required=True)
    p = sub.add_parser('recipe'); p.add_argument('--build', required=True); p.add_argument('--kit', required=True)
    p.add_argument('--lean-flags', required=True); p.add_argument('--extra', default='{}')
    p.add_argument('--boot-sha256', required=True); p.add_argument('--sdk-note', default='')
    p = sub.add_parser('finalize'); p.add_argument('--kit', required=True); p.add_argument('--os', required=True); p.add_argument('--arch', required=True)
    p = sub.add_parser('host-copy'); p.add_argument('--list', required=True); p.add_argument('--host-sdk', required=True)
    p.add_argument('--kit', required=True); p.add_argument('--os', required=True); p.add_argument('--strip', default=None)
    p = sub.add_parser('trace'); p.add_argument('--sdk', required=True); p.add_argument('--out', required=True)
    p.add_argument('--exclude', default=None); p.add_argument('logs', nargs='+')
    p = sub.add_parser('manifest'); p.add_argument('dir')
    p = sub.add_parser('zip'); p.add_argument('--kit', required=True); p.add_argument('--out', required=True); p.add_argument('--top', default='')
    a = ap.parse_args()
    {'game-link': cmd_game_link, 'game-vpk': cmd_game_vpk, 'pad-object': cmd_pad_object, 'recipe': cmd_recipe, 'finalize': cmd_finalize, 'host-copy': cmd_host_copy,
     'trace': cmd_trace, 'zip': cmd_zip, 'manifest': cmd_manifest}[a.cmd](a)

if __name__ == '__main__':
    main()
