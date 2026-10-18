#!/usr/bin/env python3
"""Extract every .PMF file from an ISO9660 disc image (the UMD dump), using nothing but the standard library.

    iso_extract.py <disc.iso> <outdir>          extract all PMFs, print "name size lba"
    iso_extract.py <disc.iso> --list            only list them

The output file names are the disc names (case kept), e.g. intro.pmf. ICON1.PMF (the XMB icon, not a game movie) is
extracted as ICON1.pmf like the rest; the caller decides what to do with it.
"""
import os
import struct
import sys

SECTOR = 2048


def read_sectors(f, lba, length):
    f.seek(lba * SECTOR)
    return f.read(length)


def list_dir(f, lba, length):
    data = read_sectors(f, lba, length)
    i = 0
    out = []
    while i < len(data):
        n = data[i]
        if n == 0:
            i = (i // SECTOR + 1) * SECTOR
            continue
        name_len = data[i + 32]
        name = data[i + 33:i + 33 + name_len].decode('latin1').split(';')[0]
        elba = struct.unpack('<I', data[i + 2:i + 6])[0]
        elen = struct.unpack('<I', data[i + 10:i + 14])[0]
        is_dir = bool(data[i + 25] & 2)
        if name not in ('\x00', '\x01'):
            out.append((name, elba, elen, is_dir))
        i += n
    return out


def walk(f, lba, length, path=''):
    for name, elba, elen, is_dir in list_dir(f, lba, length):
        full = path + '/' + name
        if is_dir:
            yield from walk(f, elba, elen, full)
        else:
            yield full, elba, elen


def find_pmfs(iso):
    with open(iso, 'rb') as f:
        pvd = read_sectors(f, 16, SECTOR)
        if pvd[1:6] != b'CD001':
            raise SystemExit('not an ISO9660 image: %s' % iso)
        root = pvd[156:156 + 34]
        rlba = struct.unpack('<I', root[2:6])[0]
        rlen = struct.unpack('<I', root[10:14])[0]
        return [(p, l, s) for p, l, s in walk(f, rlba, rlen) if p.lower().endswith('.pmf')]


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    iso, out = sys.argv[1], sys.argv[2]
    pmfs = find_pmfs(iso)
    with open(iso, 'rb') as f:
        for path, lba, size in sorted(pmfs, key=lambda e: os.path.basename(e[0]).lower()):
            name = os.path.basename(path)
            if out != '--list':
                os.makedirs(out, exist_ok=True)
                with open(os.path.join(out, name), 'wb') as g:
                    f.seek(lba * SECTOR)
                    left = size
                    while left:
                        chunk = f.read(min(left, 1 << 20))
                        if not chunk:
                            raise SystemExit('short read: %s' % path)
                        g.write(chunk)
                        left -= len(chunk)
            print('%s\t%d\t%d' % (name, size, lba))


if __name__ == '__main__':
    main()
