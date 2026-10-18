#!/usr/bin/env python3
"""Summarise a PS Vita .psp2dmp crash dump: threads, registers, and the stack
words that point into the app's code, resolved with addr2line.
usage: parse_core.py CORE.psp2dmp APP.elf [--stack N]"""
import gzip, struct, subprocess, sys
from elftools.elf.elffile import ELFFile

def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def cstr(b, o):
    e = b.index(b'\0', o); return b[o:e].decode('latin1')
STOP = {0: 'none', 0x30002: 'undefined instruction', 0x30003: 'prefetch abort',
        0x30004: 'data abort', 0x60080: 'division by zero'}

def main():
    core, elfp = sys.argv[1], sys.argv[2]
    nstack = int(sys.argv[sys.argv.index('--stack') + 1]) if '--stack' in sys.argv else 48
    raw = open(core, 'rb').read()
    if raw[:2] == b'\x1f\x8b': raw = gzip.decompress(raw)
    import io
    ef = ELFFile(io.BytesIO(raw))
    notes, segs = {}, []
    for s in ef.iter_segments():
        if s.header.p_type == 'PT_NOTE':
            for n in s.iter_notes():
                d = n['n_desc']
                if isinstance(d, str): d = d.encode('latin1')
                notes[n['n_name']] = d
        elif s.header.p_type == 'PT_LOAD':
            segs.append((s.header.p_vaddr, s.data()))
    def rd(a, n):
        for v, d in segs:
            if v <= a < v + len(d): return d[a - v:a - v + n]
        return None
    mods = []
    d = notes['MODULE_INFO']; num = u32(d, 4); off = 8
    for _ in range(num):
        name = cstr(d, off + 0x24); ns = u32(d, off + 0x4C); off += 0x50
        sg = [(u32(d, off + 0x14 * i + 8), u32(d, off + 0x14 * i + 12)) for i in range(ns)]
        off += ns * 0x14 + 0x10
        mods.append((name, sg))
    a2l = lambda offs: subprocess.run(['arm-vita-eabi-addr2line', '-e', elfp, '-f', '-C', '-i', '-p'] +
                                      [hex(o) for o in offs], capture_output=True, text=True).stdout.strip().split('\n')
    ef2 = ELFFile(open(elfp, 'rb'))
    base = min(s.header.p_vaddr for s in ef2.iter_segments() if s.header.p_type == 'PT_LOAD')
    def loc(a):
        for name, sg in mods:
            for i, (st, sz) in enumerate(sg):
                if st <= a < st + sz: return name, i + 1, a - st
        return None
    def desc(a):
        l = loc(a)
        if not l: return '0x%08x' % a
        s = '0x%08x %s@%d+0x%x' % (a, l[0], l[1], l[2])
        if l[0].endswith('.elf') or l[0] in ('infected', 'eboot.bin') or mods.index((l[0], [m for m in mods if m[0] == l[0]][0][1])) == 0:
            if l[1] == 1: s += '  => ' + ' | '.join(a2l([base + (l[2] & ~1)]))
        return s
    thr = {}
    d = notes['THREAD_INFO']; num = u32(d, 4); off = 8
    for _ in range(num):
        sz = u32(d, off); t = d[off:off + sz]
        thr[u32(t, 4)] = dict(name=cstr(t, 8), stop=u32(t, 0x74), status=u16(t, 0x30), pc=u32(t, 0x9C)); off += sz
    d = notes['THREAD_REG_INFO']; num = u32(d, 4); off = 8
    for _ in range(num):
        sz = u32(d, off); t = d[off:off + sz]
        if u32(t, 4) in thr: thr[u32(t, 4)]['gpr'] = [u32(t, 8 + 4 * i) for i in range(16)]
        off += sz
    print('modules:', ', '.join('%s[%s]' % (n, ' '.join('%x+%x' % s for s in sg)) for n, sg in mods[:3]), '...')
    for tid, t in sorted(thr.items(), key=lambda kv: -kv[1]['stop']):
        print('\n== %s (0x%x) stop=0x%x (%s) status=%d' % (t['name'], tid, t['stop'], STOP.get(t['stop'], '?'), t['status']))
        g = t.get('gpr')
        if not g: continue
        print('   PC', desc(g[15])); print('   LR', desc(g[14]))
        if t['stop'] or '--all' in sys.argv:
            print('   ' + ' '.join('r%d=%08x' % (i, g[i]) for i in range(13)), 'sp=%08x' % g[13])
            sp = g[13]; shown = 0
            for i in range(0, 4096, 4):
                w = rd(sp + i, 4)
                if w is None: break
                v = u32(w, 0); l = loc(v)
                if l and l[1] == 1:
                    print('   sp+%03x %s' % (i, desc(v))); shown += 1
                    if shown >= nstack: break
main()
