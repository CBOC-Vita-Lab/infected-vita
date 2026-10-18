"""Allegrex (MIPS32 + FPU + VFPU) decoder for the lean generator.

The classification mirrors PortableKit's src/decoder.cpp (decode_allegrex) and src/extra_instructions.cpp (classify)
one to one; `kind` is the OpcodeKind name there, `mn` the mnemonic.  The interpreter is the semantic reference.

Resources (registers) used by the data-flow analysis:
    0..31   GPR           32..63  FPR (f0..f31)      64 CC (FPU condition)      65 HI     66 LO
VFPU state (g_cpu.vfpu / g_cpu.vfpu_ctrl) is memory-resident and is not a data-flow resource.
"""
import collections

HI, LO, CC = 65, 66, 64
def F(n): return 32 + n
def s16(x): return x - 0x10000 if x & 0x8000 else x
def s32(x): return x - (1 << 32) if x & 0x80000000 else x

# register roles (o32-like PSP ABI as used by the Allegrex gcc)
RA, SP, GP, FP = 31, 29, 28, 30
ARG_INT = list(range(4, 12))                         # a0..a7  ($4..$11)
ARG_FP = [F(i) for i in range(12, 20)]               # f12..f19
CALLEE_SAVED = set(range(16, 24)) | {30} | {F(i) for i in range(20, 32)}
CALLER_SAVED_GPR = set(range(1, 16)) | {24, 25}
CALLER_SAVED = CALLER_SAVED_GPR | {F(i) for i in range(0, 20)} | {HI, LO, CC}
RET_REGS = (2, 3, F(0))

def rname(r):
    if r < 32: return '$%d' % r
    if r < 64: return '$f%d' % (r - 32)
    return {CC: 'cc', HI: 'hi', LO: 'lo'}[r]

class Insn:
    __slots__ = ('pc', 'w', 'op', 'rs', 'rt', 'rd', 'sa', 'imm', 'simm', 'tgt', 'kind', 'mn', 'uses', 'defs',
                 'delay', 'likely', 'ctl', 'mem', 'vfpu', 'fuse')
    def __repr__(self): return '<%08x %s %08x>' % (self.pc, self.mn, self.w)

BR_COND = {'beq', 'bne', 'blez', 'bgtz', 'bltz', 'bgez', 'bc1f', 'bc1t', 'bvf', 'bvt', 'bltzal', 'bgezal'}
BR_LIKELY = {'beql', 'bnel', 'blezl', 'bgtzl', 'bltzl', 'bgezl', 'bc1fl', 'bc1tl', 'bvfl', 'bvtl', 'bltzall', 'bgezall'}
BRANCHES = BR_COND | BR_LIKELY
CTRL = BRANCHES | {'j', 'jal', 'jr', 'jalr'}

# ---- VFPU / extra classification (ported from decoder.cpp / extra_instructions.cpp)
def extra_name(w):
    op = w >> 26
    if op == 0x30: return 'll'
    if op == 0x38: return 'sc'
    if op == 0x35: return 'lvl.q' if (w & 2) == 0 else 'lvr.q'
    if op == 0x3D: return 'svl.q' if (w & 2) == 0 else 'svr.q'
    if op == 0x18: return 'vsbn' if ((w >> 23) & 7) == 2 else None
    if op == 0x19: return 'vdet' if ((w >> 23) & 7) == 6 else None
    if op == 0x34:
        group = (w >> 21) & 31; operation = (w >> 16) & 31
        if group >= 24: return 'vwbn'
        if group == 1:
            return {0: 'vrnds', 22: 'vsbz', 23: 'vlgb', 29: 'vi2c', 30: 'vi2us'}.get(operation)
        if group == 2:
            return {0: 'vsrt1', 1: 'vsrt2', 3: 'vbfy2', 8: 'vsrt3', 9: 'vsrt4', 16: 'vmfvc', 17: 'vmtvc', 25: 'vt4444', 26: 'vt5551'}.get(operation)
    return None

def vfpu_length(w): return (((w >> 7) & 1) | (((w >> 15) & 1) << 1)) + 1

def classify(w):
    """-> (kind, mn) with kind == OpcodeKind name of PortableKit's decoder."""
    op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31; rd = (w >> 11) & 31; sa = (w >> 6) & 31
    if w == 0: return 'Nop', 'nop'
    ex = extra_name(w)
    if ex: return 'Extra', ex
    if op == 0x00:
        fn = w & 0x3F
        if fn == 0x02: return ('Rotr', 'rotr') if rs == 1 else ('Srl', 'srl')
        if fn == 0x06: return ('Rotrv', 'rotrv') if sa == 1 else ('Srlv', 'srlv')
        t = {0x00: ('Sll', 'sll'), 0x03: ('Sra', 'sra'), 0x04: ('Sllv', 'sllv'), 0x07: ('Srav', 'srav'), 0x08: ('Jr', 'jr'),
             0x09: ('Jalr', 'jalr'), 0x0A: ('Movz', 'movz'), 0x0B: ('Movn', 'movn'), 0x0C: ('Syscall', 'syscall'),
             0x0D: ('Break', 'break'), 0x0F: ('Sync', 'sync'), 0x10: ('Mfhi', 'mfhi'), 0x11: ('Mthi', 'mthi'),
             0x12: ('Mflo', 'mflo'), 0x13: ('Mtlo', 'mtlo'), 0x16: ('Clz', 'clz'), 0x17: ('Clo', 'clo'), 0x18: ('Mult', 'mult'),
             0x19: ('Multu', 'multu'), 0x1A: ('Div', 'div'), 0x1B: ('Divu', 'divu'), 0x1C: ('Madd', 'madd'), 0x1D: ('Maddu', 'maddu'),
             0x20: ('Add', 'add'), 0x21: ('Addu', 'addu'), 0x22: ('Sub', 'sub'), 0x23: ('Subu', 'subu'), 0x24: ('And', 'and'),
             0x25: ('Or', 'or'), 0x26: ('Xor', 'xor'), 0x27: ('Nor', 'nor'), 0x2A: ('Slt', 'slt'), 0x2B: ('Sltu', 'sltu'),
             0x2C: ('Max', 'max'), 0x2D: ('Min', 'min'), 0x2E: ('Msub', 'msub'), 0x2F: ('Msubu', 'msubu')}.get(fn)
        return t or ('Unsupported', 'special?')
    if op == 0x01:
        return {0: ('Bltz', 'bltz'), 1: ('Bgez', 'bgez'), 2: ('Bltzl', 'bltzl'), 3: ('Bgezl', 'bgezl'), 0x10: ('Bltzal', 'bltzal'),
                0x11: ('Bgezal', 'bgezal'), 0x12: ('Bltzall', 'bltzall'), 0x13: ('Bgezall', 'bgezall')}.get(rt, ('Unsupported', 'regimm?'))
    simple = {0x02: ('J', 'j'), 0x03: ('Jal', 'jal'), 0x04: ('Beq', 'beq'), 0x05: ('Bne', 'bne'), 0x06: ('Blez', 'blez'), 0x07: ('Bgtz', 'bgtz'),
              0x14: ('Beql', 'beql'), 0x15: ('Bnel', 'bnel'), 0x16: ('Blezl', 'blezl'), 0x17: ('Bgtzl', 'bgtzl'),
              0x08: ('Addiu', 'addi'), 0x09: ('Addiu', 'addiu'), 0x0A: ('Slti', 'slti'), 0x0B: ('Sltiu', 'sltiu'), 0x0C: ('Andi', 'andi'),
              0x0D: ('Ori', 'ori'), 0x0E: ('Xori', 'xori'), 0x0F: ('Lui', 'lui'),
              0x20: ('Lb', 'lb'), 0x21: ('Lh', 'lh'), 0x22: ('Lwl', 'lwl'), 0x23: ('Lw', 'lw'), 0x24: ('Lbu', 'lbu'), 0x25: ('Lhu', 'lhu'),
              0x26: ('Lwr', 'lwr'), 0x28: ('Sb', 'sb'), 0x29: ('Sh', 'sh'), 0x2A: ('Swl', 'swl'), 0x2B: ('Sw', 'sw'), 0x2E: ('Swr', 'swr'),
              0x31: ('Lwc1', 'lwc1'), 0x32: ('Lvs', 'lv.s'), 0x39: ('Swc1', 'swc1'), 0x3A: ('Svs', 'sv.s'), 0x2F: ('Cache', 'cache'),
              0x36: ('Lvq', 'lv.q'), 0x3E: ('Svq', 'sv.q'), 0x3F: ('Vflush', 'vflush')}
    if op in simple: return simple[op]
    if op == 0x11:
        if rs == 0: return 'Mfc1', 'mfc1'
        if rs == 4: return 'Mtc1', 'mtc1'
        if rs == 2: return 'Cfc1', 'cfc1'
        if rs == 6: return 'Ctc1', 'ctc1'
        if rs == 8: return {0: ('Bc1f', 'bc1f'), 1: ('Bc1t', 'bc1t'), 2: ('Bc1fl', 'bc1fl'), 3: ('Bc1tl', 'bc1tl')}[rt & 3]
        if rs == 0x10:
            fn = w & 0x3F
            t = {0: ('AddS', 'add.s'), 1: ('SubS', 'sub.s'), 2: ('MulS', 'mul.s'), 3: ('DivS', 'div.s'), 4: ('SqrtS', 'sqrt.s'),
                 5: ('AbsS', 'abs.s'), 6: ('MovS', 'mov.s'), 7: ('NegS', 'neg.s'), 0x0C: ('RoundWS', 'round.w.s'),
                 0x0D: ('TruncWS', 'trunc.w.s'), 0x0E: ('CeilWS', 'ceil.w.s'), 0x0F: ('FloorWS', 'floor.w.s'), 0x24: ('CvtWS', 'cvt.w.s')}.get(fn)
            if t: return t
            if (fn & 0x30) == 0x30: return 'FpuCompare', 'c.cond.s'
            return 'Unsupported', 'cop1.s?'
        if rs == 0x14 and (w & 0x3F) == 0x20: return 'CvtSW', 'cvt.s.w'
        return 'Unsupported', 'cop1?'
    if op == 0x1F:
        fn = w & 0x3F
        if fn == 0: return 'Ext', 'ext'
        if fn == 4: return 'Ins', 'ins'
        if fn in (0x18, 0x20):
            t = {0x02: ('Wsbh', 'wsbh'), 0x03: ('Wsbw', 'wsbw'), 0x10: ('Seb', 'seb'), 0x14: ('Bitrev', 'bitrev'), 0x18: ('Seh', 'seh')}.get(sa)
            return t or ('Unsupported', 'allegrex0?')
        return 'Unsupported', 'special3?'
    if op == 0x12:
        if rs == 3: return 'Mfv', 'mfv'
        if rs == 7: return 'Mtv', 'mtv'
        if rs == 8: return {0: ('Bvf', 'bvf'), 1: ('Bvt', 'bvt'), 2: ('Bvfl', 'bvfl'), 3: ('Bvtl', 'bvtl')}[rt & 3]
        return 'Vfpu', 'cop2/vfpu'
    if op == 0x18:
        o = (w >> 23) & 7
        if o in (0, 1, 7): return 'VfpuVec3', {0: 'vadd', 1: 'vsub', 7: 'vdiv'}[o]
        return 'Vfpu', 'vfpu0'
    if op == 0x19:
        o = (w >> 23) & 7
        if o == 0: return 'VfpuVec3', 'vmul'
        if o == 1: return 'Vdot', 'vdot'
        if o == 2: return 'Vscl', 'vscl'
        if o == 4: return 'Vhdp', 'vhdp'
        if o == 5 and vfpu_length(w) == 3: return 'Vcrs', 'vcrs.t'
        return 'Vfpu', 'vfpu1'
    if op == 0x1B:
        o = (w >> 23) & 7
        if o == 0: return 'Vcmp', 'vcmp'
        if o in (2, 3): return 'Vminmax', 'vmin' if o == 2 else 'vmax'
        if o >= 5: return 'VfpuCompare3', {5: 'vscmp', 6: 'vsge', 7: 'vslt'}[o]
        return 'Vfpu', 'vfpu3'
    if op in (0x35, 0x3D): return 'Vfpu', 'vfpu'
    if op == 0x37:
        t = (w >> 23) & 7
        if t <= 5: return 'Vpfx', 'vpfxs' if t <= 1 else ('vpfxt' if t <= 3 else 'vpfxd')
        if t == 6: return 'Viim', 'viim.s'
        return 'Vfim', 'vfim.s'
    if op == 0x34:
        group = (w >> 21) & 31; operation = (w >> 16) & 31
        if group == 3: return 'Vcst', 'vcst'
        if group == 1 and 1 <= operation <= 3: return 'Vrnd', ['vrndi', 'vrndf1', 'vrndf2'][operation - 1]
        if group == 1 and operation in (28, 31): return 'Vi2x', 'vi2uc' if operation == 28 else 'vi2s'
        if group == 2 and operation == 27: return 'Vt5650', 'vt5650'
        if group == 1 and operation == 18: return 'Vf2h', 'vf2h'
        if group == 1 and operation == 19: return 'Vh2f', 'vh2f'
        if group == 1 and 24 <= operation <= 27: return 'Vx2i', ['vuc2i', 'vc2i', 'vus2i', 'vs2i'][operation - 24]
        if 16 <= group <= 19: return 'Vf2i', ['vf2in', 'vf2iz', 'vf2iu', 'vf2id'][group - 16]
        if group == 20: return 'Vi2f', 'vi2f'
        if group == 21: return 'Vcmov', 'vcmovt' if ((w >> 19) & 1) == 0 else 'vcmovf'
        if group == 2 and operation == 4: return 'Vocp', 'vocp'
        if group == 2 and operation == 5: return 'Vsocp', 'vsocp'
        if group == 2 and operation == 2: return 'Vbfy1', 'vbfy1'
        if group == 2 and operation == 10: return 'Vsgn', 'vsgn'
        if group == 2 and operation in (6, 7): return 'VfpuHorizontal', 'vfad' if operation == 6 else 'vavg'
        if group == 0:
            t = {0: ('VfpuUnary', 'vmov'), 1: ('VfpuUnary', 'vabs'), 2: ('VfpuUnary', 'vneg'), 3: ('Vidt', 'vidt'), 4: ('VfpuUnary', 'vsat0'),
                 5: ('VfpuUnary', 'vsat1'), 6: ('VfpuVectorInit', 'vzero'), 7: ('VfpuVectorInit', 'vone'), 16: ('VfpuUnary', 'vrcp'),
                 17: ('VfpuUnary', 'vrsq'), 18: ('VfpuUnary', 'vsin'), 19: ('VfpuUnary', 'vcos'), 20: ('VfpuUnary', 'vexp2'),
                 21: ('VfpuUnary', 'vlog2'), 22: ('VfpuUnary', 'vsqrt'), 23: ('VfpuUnary', 'vasin'), 24: ('VfpuUnary', 'vnrcp'),
                 26: ('VfpuUnary', 'vnsin'), 28: ('VfpuUnary', 'vrexp2')}.get(operation)
            return t or ('Vfpu', 'vfpu4')
        return 'Vfpu', 'vfpu4'
    if op == 0x3C:
        group = (w >> 21) & 31
        if group <= 3: return 'Vmmul', 'vmmul'
        if 4 <= group <= 15: return 'Vtfm', 'vtfm'
        if 16 <= group <= 19: return 'Vmscl', 'vmscl'
        if 20 <= group <= 23:
            sc = ((w >> 7) & 1) | (((w >> 15) & 1) << 1)
            return 'VcrossQuat', 'vcrsp' if sc == 2 else ('vqmul' if sc == 3 else 'vcrsp/vqmul')
        if group == 28:
            m = (w >> 16) & 15
            if m == 0: return 'Vmmov', 'vmmov'
            if m == 3: return 'VmidT', 'vmidt'
            if m in (6, 7): return 'VfpuMatrixInit', 'vmzero' if m == 6 else 'vmone'
            return 'Vfpu', 'vfpu-matrix1'
        if group == 29: return 'Vrot', 'vrot'
        return 'Vfpu', 'vfpu6'
    return 'Unsupported', 'unknown'

VFPU_KINDS = {'Mfv', 'Mtv', 'Vpfx', 'Viim', 'Vfim', 'Vh2f', 'Vf2h', 'Vf2i', 'Vi2f', 'Vx2i', 'Vi2x', 'Vrnd', 'Vt5650', 'VmidT', 'Vmmov',
              'VfpuMatrixInit', 'Vidt', 'Vcst', 'Vocp', 'VfpuHorizontal', 'Vrot', 'Vtfm', 'VfpuVectorInit', 'VfpuVec3', 'Vscl', 'Vdot', 'Vhdp',
              'Vcmp', 'Vminmax', 'VfpuCompare3', 'Vcmov', 'VfpuUnary', 'Vmmul', 'Vmscl', 'VcrossQuat', 'Vcrs', 'Lvs', 'Svs', 'Lvq', 'Svq',
              'Vsgn', 'Vsocp', 'Vbfy1', 'Vflush', 'Bvf', 'Bvt', 'Bvfl', 'Bvtl', 'Vfpu'}
EXTRA_MEM = {'lvl.q', 'lvr.q', 'svl.q', 'svr.q', 'll', 'sc'}

def decode(pc, w):
    d = Insn()
    d.pc = pc; d.w = w
    d.op = w >> 26; d.rs = (w >> 21) & 31; d.rt = (w >> 16) & 31; d.rd = (w >> 11) & 31; d.sa = (w >> 6) & 31
    d.imm = w & 0xFFFF; d.simm = s16(d.imm); d.tgt = (w & 0x3FFFFFF) << 2
    d.kind, d.mn = classify(w)
    mn = d.mn
    d.likely = mn in BR_LIKELY
    d.delay = mn in CTRL
    d.ctl = mn in CTRL
    d.vfpu = d.kind in VFPU_KINDS or (d.kind == 'Extra' and mn not in ('ll', 'sc'))
    d.mem = None
    d.fuse = 0           # 1/2: first/second half of a fused lwl/lwr (swl/swr) pair, see fuse.py
    u, df = _usedef(d)
    d.uses = frozenset(x for x in u if x != 0)
    d.defs = frozenset(x for x in df if x != 0)
    return d

def _usedef(d):
    """(uses, defs) of the instruction proper (for control instructions: the operand reads at the branch point).
    Link-register writes of jal/jalr are NOT included (handled by the call model)."""
    m = d.mn; k = d.kind; rs, rt, rd, sa = d.rs, d.rt, d.rd, d.sa
    if k in ('Nop', 'Sync', 'Cache', 'Vflush', 'Vpfx', 'Break', 'Syscall'): return set(), set()
    if k in ('Sll', 'Srl', 'Sra', 'Rotr'): return {rt}, {rd}
    if k in ('Sllv', 'Srlv', 'Srav', 'Rotrv'): return {rt, rs}, {rd}
    if k in ('Movz', 'Movn'): return {rs, rt, rd}, {rd}
    if k in ('Add', 'Addu', 'Sub', 'Subu', 'And', 'Or', 'Xor', 'Nor', 'Slt', 'Sltu', 'Max', 'Min'): return {rs, rt}, {rd}
    if k in ('Addiu', 'Slti', 'Sltiu', 'Andi', 'Ori', 'Xori'): return {rs}, {rt}
    if k == 'Lui': return set(), {rt}
    if k in ('Clz', 'Clo'): return {rs}, {rd}
    if k == 'Ext': return {rs}, {rt}
    if k == 'Ins': return {rs, rt}, {rt}
    if k in ('Seb', 'Seh', 'Bitrev', 'Wsbh', 'Wsbw'): return {rt}, {rd}
    if k in ('Lb', 'Lh', 'Lw', 'Lbu', 'Lhu'): d.mem = 'ld'; return {rs}, {rt}
    if k in ('Lwl', 'Lwr'): d.mem = 'ld'; return {rs, rt}, {rt}
    if k in ('Sb', 'Sh', 'Sw', 'Swl', 'Swr'): d.mem = 'st'; return {rs, rt}, set()
    if k == 'Lwc1': d.mem = 'ld'; return {rs}, {F(rt)}
    if k == 'Swc1': d.mem = 'st'; return {rs, F(rt)}, set()
    if k in ('Mult', 'Multu', 'Div', 'Divu'): return {rs, rt}, {HI, LO}
    if k in ('Madd', 'Maddu', 'Msub', 'Msubu'): return {rs, rt, HI, LO}, {HI, LO}
    if k == 'Mfhi': return {HI}, {rd}
    if k == 'Mflo': return {LO}, {rd}
    if k == 'Mthi': return {rs}, {HI}
    if k == 'Mtlo': return {rs}, {LO}
    if k == 'Mfc1': return {F(rd)}, {rt}
    if k == 'Mtc1': return {rt}, {F(rd)}
    if k == 'Cfc1': return {CC}, {rt}
    if k == 'Ctc1': return {rt}, {CC}
    if k in ('AddS', 'SubS', 'MulS', 'DivS'): return {F(rd), F(rt)}, {F(sa)}
    if k in ('SqrtS', 'AbsS', 'MovS', 'NegS', 'RoundWS', 'TruncWS', 'CeilWS', 'FloorWS', 'CvtWS', 'CvtSW'): return {F(rd)}, {F(sa)}
    if k == 'FpuCompare': return {F(rd), F(rt)}, {CC}
    # VFPU memory / transfer
    if k in ('Lvs', 'Lvq'): d.mem = 'ld'; return {rs}, set()
    if k in ('Svs', 'Svq'): d.mem = 'st'; return {rs}, set()
    if k == 'Mtv': return {rt}, set()
    if k == 'Mfv': return set(), {rt}
    if k == 'Extra':
        if d.mn in ('lvl.q', 'lvr.q'): d.mem = 'ld'; return {rs}, set()
        if d.mn in ('svl.q', 'svr.q'): d.mem = 'st'; return {rs}, set()
        if d.mn == 'll': d.mem = 'ld'; return {rs}, {rt}
        if d.mn == 'sc': d.mem = 'st'; return {rs, rt}, {rt}
        return set(), set()
    if k in VFPU_KINDS and not k.startswith('Bv'): return set(), set()
    # control
    if m in ('beq', 'bne', 'beql', 'bnel'): return {rs, rt}, set()
    if m in ('blez', 'bgtz', 'bltz', 'bgez', 'blezl', 'bgtzl', 'bltzl', 'bgezl'): return {rs}, set()
    if m in ('bltzal', 'bgezal', 'bltzall', 'bgezall'): return {rs}, {RA}
    if m.startswith('bc1'): return {CC}, set()
    if m.startswith('bv'): return set(), set()
    if m == 'jr': return {rs}, set()
    if m == 'jalr': return {rs}, set()
    if m in ('j', 'jal'): return set(), set()
    return set(), set()

def branch_target(pc, d): return (pc + 4 + (d.simm << 2)) & 0xFFFFFFFF
def jump_target(pc, d): return ((pc + 4) & 0xF0000000) | d.tgt

def branch_const(d):
    """True/False when the outcome of a branch is statically known, else None."""
    m = d.mn[:-1] if d.likely else d.mn
    rs, rt = d.rs, d.rt
    if m == 'beq' and rs == rt: return True
    if m == 'bne' and rs == rt: return False
    if rs == 0 and m in ('blez', 'bgez'): return True
    if rs == 0 and m in ('bgtz', 'bltz'): return False
    if m in ('bltzal', 'bgezal') and rs == 0: return (m == 'bgezal')
    return None
