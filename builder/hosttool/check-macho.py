#!/usr/bin/env python3
"""Checks that the Mach-O programs and libraries under a folder run on the macOS the kit promises.

    python3 -I check-macho.py DIR --max-minos 10.13 [--arch x86_64|arm64] [--imports FILE] [--summary FILE]

Needs otool, lipo and nm (Xcode command line tools). For every Mach-O file it reads, from the load commands (otool -l),
  * the lowest macOS it runs on: "minos" of LC_BUILD_VERSION, or "version" of LC_VERSION_MIN_MACOSX (the linker writes that one when
    the target is older than 10.14, and then no LC_BUILD_VERSION);
  * whether it uses chained fixups (LC_DYLD_CHAINED_FIXUPS, which dyld has only from macOS 12), when the minimum is older than 12;
and from otool -L the libraries it loads, which must all be system libraries (/usr/lib or /System/Library: nothing from Homebrew or
from the build machine), and from nm -u the symbols it imports from libc++, which must not include any that macOS 10.13 does not
have (aligned new/delete, std::filesystem, libc++'s verbose abort and atomic wait, ...).
On Apple silicon it also runs `codesign -v` (macOS refuses an unsigned arm64 program).
Exit status 1 if anything is wrong. The table goes to stdout (and to --summary, as Markdown, for the job summary).
"""
import argparse, os, re, subprocess, sys

def version_tuple(v):
    parts = [int(x) for x in v.split('.')]
    return tuple(parts + [0] * (3 - len(parts)))

def run(*cmd):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)

def is_macho(path):
    try:
        with open(path, 'rb') as f:
            magic = f.read(4)
    except OSError:
        return False
    return magic in (b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca')

ALLOWED_PREFIX = ('/usr/lib/', '/System/Library/')
# libc++ symbols (mangled with a leading underscore, as nm prints them on macOS) that the libc++ of macOS 10.13 does not export, or that
# changed. Matching any of them in a program's imports means it will not start on the oldest macOS.
BAD_IMPORTS = [
    (r'align_val_t', 'aligned operator new/delete (macOS 10.14)'),
    (r'__fs\b|10filesystem|4__fs', 'std::filesystem (macOS 10.15)'),
    (r'verbose_abort', 'libc++ verbose abort (macOS 13.3)'),
    (r'__libcpp_atomic_(wait|monitor|notify)|__libcpp_(semaphore|latch|barrier)', 'C++20 atomic wait (macOS 11.3)'),
    (r'to_chars|from_chars', 'std::to_chars/from_chars (macOS 13.3)'),
    (r'__hash_memory', 'libc++ hash function (macOS 15)'),
    (r'pmr', 'std::pmr (macOS 14)'),
]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir'); ap.add_argument('--max-minos', required=True)
    ap.add_argument('--arch', default=None); ap.add_argument('--imports', default=None); ap.add_argument('--summary', default=None)
    ap.add_argument('--skip', action='append', default=[], help='file name (not path) to leave out')
    a = ap.parse_args()
    limit = version_tuple(a.max_minos)
    rows, problems, imports = [], [], {}
    for dp, dn, fn in os.walk(a.dir):
        dn.sort()
        for n in sorted(fn):
            p = os.path.join(dp, n)
            if os.path.islink(p) or not is_macho(p) or n in a.skip:
                continue
            rel = os.path.relpath(p, a.dir)
            archs = run('lipo', '-archs', p).stdout.split()
            if a.arch and archs != [a.arch]:
                problems.append('%s: architectures %s, expected only %s' % (rel, archs, a.arch))
            ol = run('otool', '-l', p).stdout
            minos = None
            m = re.search(r'cmd LC_BUILD_VERSION.*?\n\s+platform (\w+)\n\s+minos ([\d.]+)', ol, re.S)
            kind = ''
            if m:
                minos, kind = m.group(2), 'BUILD_VERSION'
            else:
                m = re.search(r'cmd LC_VERSION_MIN_MACOSX\n\s+cmdsize \d+\n\s+version ([\d.]+)', ol)
                if m:
                    minos, kind = m.group(1), 'VERSION_MIN'
            chained = 'LC_DYLD_CHAINED_FIXUPS' in ol
            libs = [l.split(' (')[0].strip() for l in run('otool', '-L', p).stdout.splitlines()[1:] if l.strip()]
            if minos is None:
                problems.append('%s: no minimum macOS version in the load commands' % rel)
            elif version_tuple(minos) > limit:
                problems.append('%s: needs macOS %s, limit %s' % (rel, minos, a.max_minos))
            if chained and limit < (12, 0, 0):
                problems.append('%s: chained fixups (macOS 12 and newer only)' % rel)
            for l in libs:
                if not l.startswith(ALLOWED_PREFIX):
                    problems.append('%s: loads %s, which is not a system library' % (rel, l))
            if 'arm64' in archs:
                cs = run('codesign', '-v', p)
                if cs.returncode != 0:
                    problems.append('%s: code signature invalid: %s' % (rel, cs.stderr.strip()))
            for line in run('nm', '-u', '-j', p).stdout.splitlines():
                s = line.strip()
                if s.startswith('__Z') and ('St3__1' in s or 'NSt' in s or s.startswith('__ZSt') or 'align_val_t' in s):
                    imports.setdefault(s, set()).add(rel)
                    for rx, why in BAD_IMPORTS:
                        if re.search(rx, s):
                            problems.append('%s: imports %s: %s' % (rel, s, why))
            rows.append((rel, ','.join(archs), minos or '?', kind, 'chained' if chained else 'classic', ', '.join(sorted(set(os.path.basename(l) for l in libs)))))
    lines = ['| file | arch | minos | load command | fixups | loads |', '|---|---|---|---|---|---|']
    lines += ['| %s |' % ' | '.join(r) for r in rows]
    table = '\n'.join(lines)
    print(table)
    if rows:
        worst = max((version_tuple(r[2]), r[2]) for r in rows if r[2] != '?')
        print('\n%d Mach-O files, highest minos %s (limit %s)' % (len(rows), worst[1], a.max_minos))
    else:
        problems.append('no Mach-O file under %s' % a.dir)
    if a.imports:
        with open(a.imports, 'w') as f:
            for s in sorted(imports):
                f.write('%s\t%s\n' % (s, ' '.join(sorted(imports[s]))))
        print('%d different libc++ symbols imported (list: %s)' % (len(imports), a.imports))
    if a.summary:
        with open(a.summary, 'a') as f:
            f.write(table + '\n\nhighest minos: %s; problems: %d\n' % (worst[1] if rows else '-', len(problems)))
    if problems:
        print('\nPROBLEMS:')
        for pr in problems:
            print('  ' + pr)
        sys.exit(1)
    print('OK')

main()
