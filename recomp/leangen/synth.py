"""A small MIPS (Allegrex) assembler and a synthetic image, for the focused tests of the recompiler.

The synthetic image looks to the generator like the real one: code at BASE, an import stub area at the end of the code range,
then data (jump tables with R_MIPS_32 relocations).  Every label declared with `.func` is a function start.  Nothing here knows
anything about a game.

    a = Asm()
    a.asm('''
        .func f_add
            addu    v0, a0, a1
            jr      ra
            nop
    ''')
    im = a.image()
"""
import re, struct
from . import mips
from .image import Image

BASE = 0x08804000

REGS = {n: i for i, n in enumerate(['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
                                    's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra'])}
REGS['s8'] = 30

def reg(x):
    x = x.strip().lstrip('$')
    if x in REGS: return REGS[x]
    if x.isdigit(): return int(x)
    raise ValueError('register %r' % x)

def freg(x):
    x = x.strip().lstrip('$')
    if x[0] == 'f' and x[1:].isdigit(): return int(x[1:])
    raise ValueError('fpu register %r' % x)

def num(x):
    return int(x.strip(), 0)

R3 = {'addu': 0x21, 'add': 0x20, 'subu': 0x23, 'sub': 0x22, 'and': 0x24, 'or': 0x25, 'xor': 0x26, 'nor': 0x27, 'slt': 0x2A, 'sltu': 0x2B,
      'max': 0x2C, 'min': 0x2D, 'movz': 0x0A, 'movn': 0x0B}
SHIFT = {'sll': 0x00, 'srl': 0x02, 'sra': 0x03}
SHIFTV = {'sllv': 0x04, 'srlv': 0x06, 'srav': 0x07}
MULDIV = {'mult': 0x18, 'multu': 0x19, 'div': 0x1A, 'divu': 0x1B, 'madd': 0x1C, 'maddu': 0x1D, 'msub': 0x2E, 'msubu': 0x2F}
IMM = {'addiu': 9, 'addi': 8, 'slti': 10, 'sltiu': 11, 'andi': 12, 'ori': 13, 'xori': 14}
MEM = {'lb': 0x20, 'lh': 0x21, 'lwl': 0x22, 'lw': 0x23, 'lbu': 0x24, 'lhu': 0x25, 'lwr': 0x26, 'sb': 0x28, 'sh': 0x29, 'swl': 0x2A,
       'sw': 0x2B, 'swr': 0x2E}
BR2 = {'beq': 4, 'bne': 5, 'beql': 0x14, 'bnel': 0x15}
BR1 = {'blez': 6, 'bgtz': 7, 'blezl': 0x16, 'bgtzl': 0x17}
REGIMM = {'bltz': 0, 'bgez': 1, 'bltzl': 2, 'bgezl': 3}
FPU3 = {'add.s': 0, 'sub.s': 1, 'mul.s': 2, 'div.s': 3}
FPU2 = {'sqrt.s': 4, 'abs.s': 5, 'mov.s': 6, 'neg.s': 7, 'round.w.s': 0x0C, 'trunc.w.s': 0x0D, 'ceil.w.s': 0x0E, 'floor.w.s': 0x0F,
        'cvt.w.s': 0x24}
FCMP = ['f', 'un', 'eq', 'ueq', 'olt', 'ult', 'ole', 'ule', 'sf', 'ngle', 'seq', 'ngl', 'lt', 'nge', 'le', 'ngt']

class Asm:
    def __init__(self, base=BASE):
        self.base = base
        self.words = []              # code words
        self.labels = {}             # name -> address (code) or data offset marker
        self.funcs = []              # (name, address)
        self.fix = []                # (index, kind, label, pc)
        self.data = []               # (name, [labels])
        self.imports = []            # (name, nid)
        self.pending_funcs = []

    @property
    def pc(self): return self.base + 4 * len(self.words)

    def emit(self, w): self.words.append(w & 0xFFFFFFFF)

    def label(self, name):
        if name in self.labels: raise ValueError('duplicate label ' + name)
        self.labels[name] = self.pc

    def func(self, name):
        self.label(name)
        self.funcs.append((name, self.pc))

    def table(self, name, targets):
        self.data.append((name, list(targets)))

    def import_stub(self, name, nid):
        self.imports.append((name, nid))

    # ----------------------------------------------------------------------- text front end
    def asm(self, text):
        for raw in text.split('\n'):
            line = raw.split('#')[0].strip()
            if not line: continue
            while True:
                m = re.match(r'^([A-Za-z_.$][\w.$]*):\s*(.*)$', line)
                if not m: break
                self.label(m.group(1)); line = m.group(2)
            if not line: continue
            self.line(line)

    def line(self, line):
        parts = line.split(None, 1)
        mn = parts[0]
        ops = [o.strip() for o in parts[1].split(',')] if len(parts) > 1 else []
        pc = self.pc
        e = self.emit
        if mn == '.func': self.func(ops[0] if ops else parts[1].strip()); return
        if mn == '.word':
            for o in ops: e(num(o))
            return
        if mn == '.table':
            self.table(ops[0], ops[1:]); return
        if mn == '.import':
            self.import_stub(ops[0], num(ops[1])); return
        if mn == 'nop': e(0); return
        if mn == 'move': e((reg(ops[1]) << 21) | (reg(ops[0]) << 11) | 0x21); return
        if mn == 'li':
            v = num(ops[1]) & 0xFFFFFFFF
            rd = reg(ops[0])
            if v < 0x10000: e((13 << 26) | (rd << 16) | v)
            elif v >= 0xFFFF8000: e((9 << 26) | (rd << 16) | (v & 0xFFFF))
            else:
                e((15 << 26) | (rd << 16) | (v >> 16))
                if v & 0xFFFF: e((13 << 26) | (rd << 21) | (rd << 16) | (v & 0xFFFF))
            return
        if mn == 'la':
            rd = reg(ops[0])
            self.fix.append((len(self.words), 'hi', ops[1], pc)); e((15 << 26) | (rd << 16))
            self.fix.append((len(self.words), 'lo', ops[1], pc + 4)); e((9 << 26) | (rd << 21) | (rd << 16))
            return
        if mn in R3:
            e((reg(ops[1]) << 21) | (reg(ops[2]) << 16) | (reg(ops[0]) << 11) | R3[mn]); return
        if mn in SHIFT:
            e((reg(ops[1]) << 16) | (reg(ops[0]) << 11) | ((num(ops[2]) & 31) << 6) | SHIFT[mn]); return
        if mn == 'rotr':
            e((1 << 21) | (reg(ops[1]) << 16) | (reg(ops[0]) << 11) | ((num(ops[2]) & 31) << 6) | 0x02); return
        if mn in SHIFTV:
            e((reg(ops[2]) << 21) | (reg(ops[1]) << 16) | (reg(ops[0]) << 11) | SHIFTV[mn]); return
        if mn in MULDIV:
            e((reg(ops[0]) << 21) | (reg(ops[1]) << 16) | MULDIV[mn]); return
        if mn in ('mfhi', 'mflo'): e((reg(ops[0]) << 11) | (0x10 if mn == 'mfhi' else 0x12)); return
        if mn in ('mthi', 'mtlo'): e((reg(ops[0]) << 21) | (0x11 if mn == 'mthi' else 0x13)); return
        if mn in ('clz', 'clo'): e((reg(ops[1]) << 21) | (reg(ops[0]) << 11) | (0x16 if mn == 'clz' else 0x17)); return
        if mn in ('seb', 'seh', 'wsbh', 'bitrev', 'wsbw'):
            sa = {'wsbh': 2, 'wsbw': 3, 'seb': 0x10, 'bitrev': 0x14, 'seh': 0x18}[mn]
            fn = 0x20 if mn in ('seb', 'seh', 'wsbh', 'bitrev', 'wsbw') else 0x18
            e((0x1F << 26) | (reg(ops[1]) << 16) | (reg(ops[0]) << 11) | (sa << 6) | fn); return
        if mn == 'ext':
            pos, size = num(ops[2]), num(ops[3])
            e((0x1F << 26) | (reg(ops[1]) << 21) | (reg(ops[0]) << 16) | ((size - 1) << 11) | (pos << 6) | 0); return
        if mn == 'ins':
            pos, size = num(ops[2]), num(ops[3])
            e((0x1F << 26) | (reg(ops[1]) << 21) | (reg(ops[0]) << 16) | ((pos + size - 1) << 11) | (pos << 6) | 4); return
        if mn in IMM:
            e((IMM[mn] << 26) | (reg(ops[1]) << 21) | (reg(ops[0]) << 16) | (num(ops[2]) & 0xFFFF)); return
        if mn == 'lui': e((15 << 26) | (reg(ops[0]) << 16) | (num(ops[1]) & 0xFFFF)); return
        if mn in MEM or mn in ('lwc1', 'swc1', 'lv.s', 'sv.s', 'lv.q', 'sv.q', 'lvl.q', 'lvr.q', 'svl.q', 'svr.q', 'll', 'sc'):
            m = re.match(r'^(-?\w*)\((\$?\w+)\)$', ops[1])
            if not m: raise ValueError('memory operand %r' % ops[1])
            off = num(m.group(1)) if m.group(1) not in ('', '-') else 0
            base = reg(m.group(2))
            if mn in MEM:
                e((MEM[mn] << 26) | (base << 21) | (reg(ops[0]) << 16) | (off & 0xFFFF)); return
            if mn in ('lwc1', 'swc1'):
                e(((0x31 if mn == 'lwc1' else 0x39) << 26) | (base << 21) | (freg(ops[0]) << 16) | (off & 0xFFFF)); return
            if mn in ('ll', 'sc'):
                e(((0x30 if mn == 'll' else 0x38) << 26) | (base << 21) | (reg(ops[0]) << 16) | (off & 0xFFFF)); return
            v = num(ops[0])                         # raw VFPU register number: lv.s 0..127, lv.q 0..63
            if mn in ('lv.s', 'sv.s'):
                e(((0x32 if mn == 'lv.s' else 0x3A) << 26) | (base << 21) | ((v & 0x1F) << 16) | (off & 0xFFFC) | ((v >> 5) & 3)); return
            op = {'lv.q': 0x36, 'sv.q': 0x3E, 'lvl.q': 0x35, 'lvr.q': 0x35, 'svl.q': 0x3D, 'svr.q': 0x3D}[mn]
            lo = {'lvl.q': 0, 'lvr.q': 2, 'svl.q': 0, 'svr.q': 2}.get(mn, 0)
            e((op << 26) | (base << 21) | ((v & 0x1F) << 16) | (off & 0xFFFC) | ((v >> 5) & 1) | lo); return
        if mn in ('bvf', 'bvt', 'bvfl', 'bvtl'):
            ty = {'bvf': 0, 'bvt': 1, 'bvfl': 2, 'bvtl': 3}[mn]
            self.fix.append((len(self.words), 'br', ops[1], pc)); e((0x12 << 26) | (8 << 21) | ((num(ops[0]) & 7) << 18) | (ty << 16)); return
        if mn == 'b':
            self.fix.append((len(self.words), 'br', ops[0], pc)); e(4 << 26); return
        if mn in ('beqz', 'bnez'):
            self.fix.append((len(self.words), 'br', ops[1], pc)); e(((4 if mn == 'beqz' else 5) << 26) | (reg(ops[0]) << 21)); return
        if mn in BR2:
            self.fix.append((len(self.words), 'br', ops[2], pc)); e((BR2[mn] << 26) | (reg(ops[0]) << 21) | (reg(ops[1]) << 16)); return
        if mn in BR1:
            self.fix.append((len(self.words), 'br', ops[1], pc)); e((BR1[mn] << 26) | (reg(ops[0]) << 21)); return
        if mn in REGIMM:
            self.fix.append((len(self.words), 'br', ops[1], pc)); e((1 << 26) | (reg(ops[0]) << 21) | (REGIMM[mn] << 16)); return
        if mn in ('bc1f', 'bc1t', 'bc1fl', 'bc1tl'):
            rt = {'bc1f': 0, 'bc1t': 1, 'bc1fl': 2, 'bc1tl': 3}[mn]
            self.fix.append((len(self.words), 'br', ops[0], pc)); e((0x11 << 26) | (8 << 21) | (rt << 16)); return
        if mn in ('j', 'jal'):
            self.fix.append((len(self.words), 'j', ops[0], pc)); e((2 if mn == 'j' else 3) << 26); return
        if mn == 'jr': e((reg(ops[0]) << 21) | 0x08); return
        if mn == 'jalr':
            rd, rs = (31, reg(ops[0])) if len(ops) == 1 else (reg(ops[0]), reg(ops[1]))
            e((rs << 21) | (rd << 11) | 0x09); return
        if mn == 'break': e(0x0D); return
        if mn == 'sync': e(0x0F); return
        # FPU
        if mn == 'mfc1': e((0x11 << 26) | (0 << 21) | (reg(ops[0]) << 16) | (freg(ops[1]) << 11)); return
        if mn == 'mtc1': e((0x11 << 26) | (4 << 21) | (reg(ops[0]) << 16) | (freg(ops[1]) << 11)); return
        if mn == 'cfc1': e((0x11 << 26) | (2 << 21) | (reg(ops[0]) << 16) | (num(ops[1]) << 11)); return
        if mn == 'ctc1': e((0x11 << 26) | (6 << 21) | (reg(ops[0]) << 16) | (num(ops[1]) << 11)); return
        if mn in FPU3:
            e((0x11 << 26) | (0x10 << 21) | (freg(ops[2]) << 16) | (freg(ops[1]) << 11) | (freg(ops[0]) << 6) | FPU3[mn]); return
        if mn in FPU2:
            e((0x11 << 26) | (0x10 << 21) | (freg(ops[1]) << 11) | (freg(ops[0]) << 6) | FPU2[mn]); return
        if mn == 'cvt.s.w': e((0x11 << 26) | (0x14 << 21) | (freg(ops[1]) << 11) | (freg(ops[0]) << 6) | 0x20); return
        m = re.match(r'^c\.(\w+)\.s$', mn)
        if m:
            e((0x11 << 26) | (0x10 << 21) | (freg(ops[1]) << 16) | (freg(ops[0]) << 11) | 0x30 | FCMP.index(m.group(1))); return
        raise ValueError('unknown mnemonic %r in %r' % (mn, line))

    # ----------------------------------------------------------------------- image
    def image(self, gp=0x08A00000):
        # layout: code | stubs | data
        code = list(self.words)
        stub_addr = {}
        # stubs are placed after the code, 8 bytes each (jr ra; nop), as the real stub area is
        stub_start = self.base + 4 * len(code)
        for i, (name, nid) in enumerate(self.imports):
            stub_addr[name] = stub_start + 8 * i
            code += [0x03E00008, 0]
        stub_end = self.base + 4 * len(code)
        data_addr = {}
        data = []
        for name, targets in self.data:
            data_addr[name] = stub_end + 4 * len(data)
            data += [0] * len(targets)
        labels = dict(self.labels); labels.update(stub_addr); labels.update(data_addr)
        for idx, kind, name, pc in self.fix:
            if name not in labels: raise ValueError('undefined label ' + name)
            t = labels[name]
            w = code[idx]
            if kind == 'br':
                off = (t - (pc + 4)) >> 2
                if not -32768 <= off < 32768: raise ValueError('branch out of range')
                code[idx] = w | (off & 0xFFFF)
            elif kind == 'j': code[idx] = w | ((t >> 2) & 0x3FFFFFF)
            elif kind == 'hi': code[idx] = w | (((t + 0x8000) >> 16) & 0xFFFF)
            elif kind == 'lo': code[idx] = w | (t & 0xFFFF)
        relocs = []
        o = 0
        for name, targets in self.data:
            for j, tg in enumerate(targets):
                data[o + j] = labels[tg]
                relocs.append((data_addr[name] + 4 * j, 2, 0, 0))
            o += len(targets)
        return SynthImage(self.base, code, data, stub_end, stub_start, gp, relocs, {stub_addr[n]: ('synth', nid) for n, nid in self.imports},
                          [a for _, a in self.funcs], dict(labels))

class SynthImage(Image):
    def __init__(self, base, code, data, stub_end, stub_start, gp, relocs, imports, funcs, labels):
        self.base = base
        self.words = tuple(code)
        self.img = struct.pack('<%dI' % (len(code) + len(data)), *(list(code) + list(data)))
        self.stub_end = stub_end
        self.stub_start = stub_start
        self.code_end = stub_end
        self.nwords = len(code)
        self.text_start = base
        self.text_end = stub_start
        self.gp = gp
        self.relocs = relocs
        self.imports = imports
        self.import_names = {}
        self.func_starts = funcs
        self.labels = labels
        self.entry = funcs[0] if funcs else base
        self.segments = []
        self.sections = []
        self.module_name = 'synth'

    def code_pointers(self):
        out = super().code_pointers()
        for a in self.func_starts: out[a].add('hilo')       # every declared function is a start
        return out
