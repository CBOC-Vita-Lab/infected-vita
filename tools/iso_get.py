#!/usr/bin/env python3
"""Copy one file out of an ISO9660 disc image (a UMD dump), using nothing but the standard library.

    iso_get.py <disc.iso> --list                       list every file with its size
    iso_get.py <disc.iso> <path-on-disc> <out-file>    e.g.  PSP_GAME/SYSDIR/BOOT.BIN  BOOT.BIN

The path is matched without regard to case and without the ISO's ";1" version suffix. This is how docs/BUILDING.md gets BOOT.BIN
out of the player's own disc image without needing 7-Zip or xorriso.
"""
import struct
import sys

SECTOR = 2048


def read_at(f, lba, length):
    f.seek(lba * SECTOR)
    return f.read(length)


def entries(f, lba, length):
    data = read_at(f, lba, length)
    i = 0
    while i < len(data):
        n = data[i]
        if n == 0:
            i = (i // SECTOR + 1) * SECTOR
            continue
        name_len = data[i + 32]
        raw = data[i + 33:i + 33 + name_len]
        name = raw.decode('latin1').split(';')[0]
        elba = struct.unpack('<I', data[i + 2:i + 6])[0]
        elen = struct.unpack('<I', data[i + 10:i + 14])[0]
        is_dir = bool(data[i + 25] & 2)
        if raw not in (b'\x00', b'\x01'):
            yield name, elba, elen, is_dir
        i += n


def walk(f, lba, length, prefix=''):
    for name, elba, elen, is_dir in entries(f, lba, length):
        path = prefix + name
        if is_dir:
            yield from walk(f, elba, elen, path + '/')
        else:
            yield path, elba, elen


def root(f):
    pvd = read_at(f, 16, SECTOR)
    if pvd[1:6] != b'CD001':
        raise SystemExit('not an ISO9660 image')
    rec = pvd[156:156 + 34]
    return struct.unpack('<I', rec[2:6])[0], struct.unpack('<I', rec[10:14])[0]


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    with open(sys.argv[1], 'rb') as f:
        rlba, rlen = root(f)
        if sys.argv[2] == '--list':
            for path, _lba, size in walk(f, rlba, rlen):
                print('%10d  %s' % (size, path))
            return
        if len(sys.argv) != 4:
            raise SystemExit(__doc__)
        want = sys.argv[2].strip('/').upper()
        for path, lba, size in walk(f, rlba, rlen):
            if path.upper() == want:
                with open(sys.argv[3], 'wb') as g:
                    left, pos = size, lba * SECTOR
                    f.seek(pos)
                    while left:
                        chunk = f.read(min(left, 1 << 20))
                        if not chunk:
                            raise SystemExit('short read')
                        g.write(chunk)
                        left -= len(chunk)
                print('%s: %d bytes' % (sys.argv[3], size))
                return
        raise SystemExit('not on the disc: %s' % sys.argv[2])


if __name__ == '__main__':
    main()
