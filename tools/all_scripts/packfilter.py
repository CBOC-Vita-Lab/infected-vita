#!/usr/bin/env python3 -I
"""packfilter.py IN OUT KEEP : copy an IFTP v1 pack keeping the records for which KEEP(fmt, mips, w, h, flags) is true.
KEEP is a Python expression over fmt (0 RGBA8, 1 DXT1, 2 DXT5), mips, w, h, flags, e.g. "fmt==1"."""
import struct, sys
src, dst, expr = sys.argv[1:4]
keep = eval("lambda fmt,mips,w,h,flags,size: " + expr)
f = open(src, 'rb')
magic, ver, count, scale = struct.unpack('<4sIII', f.read(16))
assert magic == b'IFTP' and ver == 1
recs = []
for i in range(count):
    key, off, size, w, h, fmt, mips, flags = struct.unpack('<QQIHHBBH', f.read(28))
    if keep(fmt, mips, w, h, flags, size):
        recs.append([key, off, size, w, h, fmt, mips, flags])
base = 16 + 28 * len(recs)
out = open(dst, 'wb')
out.write(struct.pack('<4sIII', b'IFTP', 1, len(recs), scale))
pos = base
blobs = []
for r in recs:
    f.seek(r[1]); blobs.append(f.read(r[2]))
    out.write(struct.pack('<QQIHHBBH', r[0], pos, r[2], r[3], r[4], r[5], r[6], r[7]))
    pos += r[2]
for b in blobs: out.write(b)
out.close()
print(dst, len(recs), 'records', pos, 'bytes')
