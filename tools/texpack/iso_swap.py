#!/usr/bin/env python3
"""Testing aid: makes one file of an ISO9660 image read as another one (patches the directory record in place; copy the ISO first).

    iso_swap.py IMAGE.iso --list [DIR]                      list a directory (default: the levels of Infected)
    iso_swap.py IMAGE.iso --redirect TARGET SOURCE          TARGET's record now points at SOURCE's data (extent and size)
    iso_swap.py IMAGE.iso --restore                         undo every redirect made (the table is kept in IMAGE.iso.swaps.json)

TARGET and SOURCE are paths inside the image, e.g. PSP_GAME/USRDIR/IMAGE/levels/Mission_1A.stream.zpm. Used to see another level of the game through
the mission-1 entry (to harvest its textures: coverage of the HD texture pack beyond the first mission). Never run it on the owner's only copy.
"""
import argparse
import json
import os
import struct
import sys

SECTOR = 2048
LEVELS = 'PSP_GAME/USRDIR/IMAGE/levels'


def u32both(b, off):
    return struct.unpack_from('<I', b, off)[0]


class Iso:
    def __init__(self, path, write=False):
        self.f = open(path, 'r+b' if write else 'rb')
        pvd = self.read(16 * SECTOR, SECTOR)
        assert pvd[0] == 1 and pvd[1:6] == b'CD001', 'no ISO9660 primary volume descriptor'
        self.root = self.record(pvd, 156, 16 * SECTOR + 156)

    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    @staticmethod
    def record(buf, at, abs_at):
        ln = buf[at]
        if ln == 0:
            return None
        name_len = buf[at + 32]
        raw = bytes(buf[at + 33:at + 33 + name_len])
        name = '.' if raw == b'\x00' else '..' if raw == b'\x01' else raw.decode('latin-1').split(';')[0]
        return dict(extent=u32both(buf, at + 2), size=u32both(buf, at + 10), flags=buf[at + 25], name=name, length=ln, abs=abs_at)

    def entries(self, directory):
        data = self.read(directory['extent'] * SECTOR, directory['size'])
        at = 0
        out = []
        while at < len(data):
            if data[at] == 0:
                at = (at // SECTOR + 1) * SECTOR
                continue
            rec = self.record(data, at, directory['extent'] * SECTOR + at)
            at += data[at]
            if rec and rec['name'] not in ('.', '..'):
                out.append(rec)
        return out

    def find(self, path):
        cur = self.root
        for part in [p for p in path.split('/') if p]:
            nxt = next((e for e in self.entries(cur) if e['name'].upper() == part.upper()), None)
            if nxt is None:
                raise SystemExit(f'not found: {path} (at {part})')
            cur = nxt
        return cur

    def patch(self, rec, extent, size):
        # both-endian fields: extent at +2 (LE) and +6 (BE), size at +10 (LE) and +14 (BE)
        self.f.seek(rec['abs'] + 2)
        self.f.write(struct.pack('<I', extent) + struct.pack('>I', extent) + struct.pack('<I', size) + struct.pack('>I', size))
        self.f.flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image')
    ap.add_argument('--list', nargs='?', const=LEVELS)
    ap.add_argument('--redirect', nargs=2, metavar=('TARGET', 'SOURCE'))
    ap.add_argument('--restore', action='store_true')
    args = ap.parse_args()
    table = args.image + '.swaps.json'
    iso = Iso(args.image, write=bool(args.redirect or args.restore))
    if args.list:
        for e in iso.entries(iso.find(args.list)):
            print(f'{e["name"]:40s} extent {e["extent"]:8d} size {e["size"]:10d}{"  <dir>" if e["flags"] & 2 else ""}')
    if args.redirect:
        target = iso.find(args.redirect[0])
        source = iso.find(args.redirect[1])
        swaps = json.load(open(table)) if os.path.exists(table) else {}
        swaps.setdefault(args.redirect[0], dict(extent=target['extent'], size=target['size']))
        json.dump(swaps, open(table, 'w'), indent=1)
        iso.patch(target, source['extent'], source['size'])
        print(f'{args.redirect[0]} now reads as {args.redirect[1]} (extent {source["extent"]}, {source["size"]} bytes)')
    if args.restore:
        swaps = json.load(open(table)) if os.path.exists(table) else {}
        for path, orig in swaps.items():
            iso.patch(iso.find(path), orig['extent'], orig['size'])
            print('restored', path)
        os.remove(table) if os.path.exists(table) else None
    return 0


if __name__ == '__main__':
    sys.exit(main())
