#!/usr/bin/env python3
"""rwsent.py: parse the 0x704 entity chunks of an Infected level stream (RenderWare Studio attribute serialisation).
entity chunk body: u32 0, then records [u32 size][u32 flags][payload], size includes the 8-byte header.
 flags 0x20000000: entity (behaviour-set) name; 0x40000000: GUID; 0x80000000: behaviour class name that opens an attribute block;
 flags 0 : attribute [u32 index][value bytes] belonging to the current block."""
import struct, sys

def load(path):
    return open(path, 'rb').read()

def cstr(b):
    i = b.find(b'\x00')
    return (b if i < 0 else b[:i]).decode('latin1')

def chunks(d):
    pos = 0
    while pos + 12 <= len(d):
        t, s, st = struct.unpack_from('<III', d, pos)
        yield pos, t, s
        pos += 12 + s

def parse_entity(d, pos, size):
    body = d[pos + 12: pos + 12 + size]
    p = 4
    ent = {'off': pos, 'name': None, 'guid': None, 'blocks': []}
    cur = None
    while p + 8 <= len(body):
        sz, fl = struct.unpack_from('<II', body, p)
        if sz < 8 or p + sz > len(body): break
        pay = body[p + 8: p + sz]
        if fl == 0x20000000: ent['name'] = cstr(pay)
        elif fl == 0x40000000: ent['guid'] = pay.hex()
        elif fl == 0x80000000:
            cur = {'cls': cstr(pay), 'attrs': []}; ent['blocks'].append(cur)
        elif fl < 0x1000 and cur is not None:
            cur['attrs'].append((fl, pay))
        else:
            cur = None
        p += sz
    return ent

def fmt(v):
    if len(v) == 4:
        i = struct.unpack('<i', v)[0]; f = struct.unpack('<f', v)[0]
        if v == b'\x00\xbf\xbf\xbf': return "''"
        if abs(f) > 1e-6 and abs(f) < 1e7 and not (-100000 < i < 100000 and i != 0 and abs(f) < 1e-30 or False):
            if 0 < abs(i) < 1000000: return 'i%d' % i
            return 'f%g' % f
        return 'i%d' % i
    s = v.rstrip(b'\x00\xbf')
    if s and all(32 <= c < 127 for c in s): return repr(s.decode())
    return v.hex()

def dump_entities(path):
    d = load(path)
    for pos, t, s in chunks(d):
        if t == 0x704:
            yield parse_entity(d, pos, s)

if __name__ == '__main__':
    only = sys.argv[2:] 
    for e in dump_entities(sys.argv[1]):
        if only and e['name'] not in only: continue
        print('ENT %s @%x guid=%s' % (e['name'], e['off'], e['guid']))
        for b in e['blocks']:
            print('   [%s] ' % b['cls'] + ' '.join('%d=%s' % (i, fmt(v)) for i, v in b['attrs']))
