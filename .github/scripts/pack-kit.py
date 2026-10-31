#!/usr/bin/env python3
"""Packs an unpacked builder kit into a reproducible .tar.gz (the format the installer's release-format.md asks for).

    python3 -I pack-kit.py KIT_DIR OUT.tar.gz

* The files sit at the top of the archive (no wrapping folder): kit.json first, then everything sorted.
* Modes: 0755 for programs, 0644 for the rest. On macOS and Linux a program is a file with an execute bit; on Windows (which has no
  such bits) it is a *.exe or *.dll.
* No links (the installer only accepts links that stay inside the kit; the kit has none), no owners, one fixed time stamp, gzip
  header without name or time: the same kit gives the same archive.
"""
import gzip, hashlib, os, stat, sys, tarfile

def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    kit, out = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    if not os.path.isfile(os.path.join(kit, 'kit.json')):
        sys.exit('no kit.json in %s' % kit)
    windows = os.name == 'nt'
    stamp = int(os.environ.get('SOURCE_DATE_EPOCH', '1767225600'))      # 2026-01-01
    names = []
    for dp, dn, fn in os.walk(kit):
        dn.sort()
        for d in dn:
            if os.path.islink(os.path.join(dp, d)):
                sys.exit('link in the kit: %s' % os.path.join(dp, d))
        for n in fn:
            p = os.path.join(dp, n)
            if os.path.islink(p):
                sys.exit('link in the kit: %s' % p)
            names.append(os.path.relpath(p, kit).replace(os.sep, '/'))
    names.sort(key=lambda r: (r != 'kit.json', r))
    total = 0
    with open(out, 'wb') as raw:
        with gzip.GzipFile(filename='', mode='wb', fileobj=raw, compresslevel=9, mtime=0) as gz:
            with tarfile.open(fileobj=gz, mode='w', format=tarfile.GNU_FORMAT) as tar:
                for rel in names:
                    p = os.path.join(kit, *rel.split('/'))
                    st = os.stat(p)
                    if windows:
                        exe = rel.endswith('.exe') or rel.endswith('.dll')
                    else:
                        exe = bool(st.st_mode & stat.S_IXUSR)
                    ti = tarfile.TarInfo(rel)
                    ti.size, ti.mtime, ti.mode, ti.type = st.st_size, stamp, 0o755 if exe else 0o644, tarfile.REGTYPE
                    ti.uid = ti.gid = 0; ti.uname = ti.gname = ''
                    with open(p, 'rb') as f:
                        tar.addfile(ti, f)
                    total += st.st_size
    h = hashlib.sha256(open(out, 'rb').read()).hexdigest()
    print('%s: %d files, %.1f MB unpacked, %.1f MB packed, sha256 %s' % (out, len(names), total / 1e6, os.path.getsize(out) / 1e6, h))

if __name__ == '__main__':
    main()
