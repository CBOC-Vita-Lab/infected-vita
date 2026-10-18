"""Loader for the relocated Infected image + PRX metadata (relocation sites, module info, import stubs)."""
import struct, collections, os

BASE = 0x08804000
# Both come from the player's own disc: BOOT.BIN is PSP_GAME/SYSDIR/BOOT.BIN of the ISO, image.bin is what tools/dump_image.cpp
# writes from it (docs/BUILDING.md). Defaults: the current directory.
IMG_PATH = os.environ.get('LEAN_IMAGE', 'image.bin')
BOOT_PATH = os.environ.get('LEAN_BOOT', 'BOOT.BIN')
R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16 = 2, 4, 5, 6

class Image:
    def __init__(self, img_path=IMG_PATH, boot_path=BOOT_PATH):
        self.img = open(img_path, 'rb').read()
        boot = open(boot_path, 'rb').read()
        self.base = BASE
        # --- ELF sections / segments of the PRX
        e_phoff, = struct.unpack_from('<I', boot, 28)
        e_shoff, = struct.unpack_from('<I', boot, 32)
        e_phnum, = struct.unpack_from('<H', boot, 44)
        e_shnum, e_shstrndx = struct.unpack_from('<HH', boot, 48)
        self.entry_off, = struct.unpack_from('<I', boot, 24)
        self.segments = []
        for i in range(e_phnum):
            t, off, vaddr, paddr, fsz, msz, fl, al = struct.unpack_from('<8I', boot, e_phoff + 32 * i)
            self.segments.append(dict(type=t, off=off, vaddr=vaddr, paddr=paddr, filesz=fsz, memsz=msz))
        secs = []
        for i in range(e_shnum):
            nm, ty, fl, ad, off, sz, lk, inf, al, es = struct.unpack_from('<10I', boot, e_shoff + 40 * i)
            secs.append(dict(name_off=nm, type=ty, addr=ad, off=off, size=sz, link=lk, info=inf))
        stroff = secs[e_shstrndx]['off']
        for s in secs:
            e = boot.index(b'\0', stroff + s['name_off'])
            s['name'] = boot[stroff + s['name_off']:e].decode()
        self.sections = secs
        text = [s for s in secs if s['name'] == '.text'][0]
        self.text_start = BASE + text['addr']
        self.text_end = BASE + text['addr'] + text['size']        # end of .text proper
        stub_secs = [s for s in secs if s['name'].startswith('.sceStub.text')]
        self.stub_start = BASE + min(s['addr'] for s in stub_secs)
        self.stub_end = BASE + max(s['addr'] + s['size'] for s in stub_secs)
        self.code_end = self.stub_end                              # words below this are decoded as code candidates
        self.nwords = (self.stub_end - BASE) // 4
        self.words = struct.unpack('<%dI' % self.nwords, self.img[:self.nwords * 4])
        # --- segments' runtime addresses
        self.seg_addr = [BASE + s['vaddr'] for s in self.segments]
        # --- relocation sites
        self.relocs = []          # (addr, type, patch_seg, target_seg)
        for s in secs:
            if s['type'] != 0x700000A0: continue
            for j in range(s['size'] // 8):
                off, info = struct.unpack_from('<II', boot, s['off'] + 8 * j)
                typ = info & 0xF; ps = (info >> 8) & 0xFF; ts = (info >> 16) & 0xFF
                if ps >= len(self.seg_addr) or ts >= len(self.seg_addr): continue
                self.relocs.append((self.seg_addr[ps] + off, typ, ps, ts))
        # --- module info
        mi = [s for s in secs if s['name'] == '.rodata.sceModuleInfo'][0]
        o = mi['addr']
        self.module_name = self.img[o + 4:o + 32].split(b'\0')[0].decode()
        self.gp, self.ent_top, self.ent_end, self.stub_top, self.stub_end_tbl = struct.unpack_from('<5I', self.img, o + 32)
        # --- imports (stub table walk, as Elf32Image::scan_imports)
        self.imports = {}          # stub addr -> (library, nid)
        cur = self.stub_top
        while cur < self.stub_end_tbl:
            libname, = struct.unpack_from('<I', self.img, cur - BASE)
            nwords = self.img[cur - BASE + 8]
            count, = struct.unpack_from('<H', self.img, cur - BASE + 10)
            nidt, stubt = struct.unpack_from('<II', self.img, cur - BASE + 12)
            lib = self.cstr(libname)
            for i in range(count):
                nid, = struct.unpack_from('<I', self.img, nidt - BASE + 4 * i)
                self.imports[stubt + 8 * i] = (lib, nid)
            cur += max(20, nwords * 4)
        self.import_names = {}
        tsv = os.path.join(os.path.dirname(__file__), '..', '..', 'tools', 'imports.tsv')
        if os.path.exists(tsv):
            for line in open(tsv):
                p = line.rstrip('\n').split('\t')
                if len(p) >= 4: self.import_names[int(p[0], 16)] = p[3]
        self.entry = BASE + self.entry_off

    def cstr(self, a):
        o = a - BASE
        return self.img[o:self.img.index(b'\0', o)].decode(errors='replace')

    def word(self, a): return struct.unpack_from('<I', self.img, a - BASE)[0]
    def in_code(self, a): return BASE <= a < self.stub_end and (a & 3) == 0
    def in_text(self, a): return self.text_start <= a < self.text_end and (a & 3) == 0
    def is_stub(self, a): return a in self.imports

    def code_pointers(self):
        """Address-taken code pointers from the relocation table.
        -> dict addr -> set(source kinds)  ('data32' R_MIPS_32 words in data/rodata, 'hilo' lui/addiu pairs, 'jal' R_MIPS_26)"""
        out = collections.defaultdict(set)
        self.ptr_src = collections.defaultdict(list)       # code pointer value -> addresses of the R_MIPS_32 words holding it
        by_id = collections.defaultdict(list)
        sites = [r for r in self.relocs if r[1] in (R_MIPS_32, R_MIPS_HI16, R_MIPS_LO16)]
        # pair HI16 with the next LO16 of the same (patch,target) segment pair, like PortableKit's loader
        n = len(self.relocs)
        rel = self.relocs
        for i, (a, t, ps, ts) in enumerate(rel):
            if t == R_MIPS_32:
                v = self.word(a) if (a - BASE) + 4 <= len(self.img) else 0
                if self.in_code(v):
                    out[v].add('data32' if a >= self.stub_end else 'text32')
                    self.ptr_src[v].append(a)
            elif t == R_MIPS_HI16:
                for j in range(i + 1, len(rel)):
                    b, t2, ps2, ts2 = rel[j]
                    if t2 == R_MIPS_HI16: continue
                    if t2 != R_MIPS_LO16 or (ps2, ts2) != (ps, ts): continue
                    hi = self.word(a) & 0xFFFF; lo = self.word(b) & 0xFFFF
                    if lo & 0x8000: lo -= 0x10000
                    v = ((hi << 16) + lo) & 0xFFFFFFFF
                    if self.in_code(v): out[v].add('hilo')
                    break
        return out
