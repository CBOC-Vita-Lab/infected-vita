"""Calling-convention helpers shared by the emitter, the thunk generator and the manifest.

Typed form   RET f_ADDR(u32 sp, u32 a0..a7, float f12..f19)     (only the registers in A | Aw, in this order)
Regfile form void t_ADDR()                                        (g_cpu is the argument / result area)
Thunk        void t_ADDR()   for typed functions: loads A|Aw from g_cpu, calls f_ADDR, stores the returned registers

Return packing (R' = returned subset of v0, v1, f0):
    {}            void
    {v0} {v1}     uint32_t
    {f0}          float
    two of three  uint64_t   (lo = first, hi = second; f0 travels as its bit pattern)
    all three     lean_r3    { uint64_t a = v0 | v1 << 32; uint32_t b = f0 bits }
"""
from .mips import F, SP
from .analysis import OUT_ENC, V0, V1, F0, regs_in

def params(s):
    """ordered parameter registers of a typed function"""
    regs = regs_in(s.A | s.Aw)
    out = [r for r in regs if r == SP]
    out += [r for r in regs if 4 <= r <= 11]
    out += [r for r in regs if F(12) <= r <= F(19)]
    return out

def rset(L, s):
    """returned registers (typed form)"""
    m = L.rp(s) & OUT_ENC
    return [r for r in (V0, V1, F0) if m >> r & 1]

def reg_name(r):
    if r == 0: return '0u'
    if r == SP: return 'sp'
    if r < 32: return 'r%d' % r
    if r < 64: return 'f%d' % (r - 32)
    return {64: 'cc', 65: 'hi', 66: 'lo'}[r]

def is_fp(r): return 32 <= r < 64

def ctype(r): return 'float' if is_fp(r) else 'uint32_t'

def ret_type(regs):
    n = len(regs)
    if n == 0: return 'void'
    if n == 1: return 'float' if regs[0] == F0 else 'uint32_t'
    if n == 2: return 'uint64_t'
    return 'lean_r3'

def as_bits(r):
    return 'lean_f2u(%s)' % reg_name(r) if is_fp(r) else reg_name(r)

def from_bits(r, expr):
    return 'lean_u2f(%s)' % expr if is_fp(r) else expr

def pack_return(regs):
    """C expression for the return value"""
    n = len(regs)
    if n == 0: return ''
    if n == 1: return reg_name(regs[0])
    if n == 2: return 'lean_pk2(%s, %s)' % (as_bits(regs[0]), as_bits(regs[1]))
    return 'lean_r3{lean_pk2(%s, %s), %s}' % (as_bits(regs[0]), as_bits(regs[1]), as_bits(regs[2]))

def unpack_return(regs, expr, tmp='rv'):
    """list of C statements assigning the returned registers from the call expression"""
    n = len(regs)
    if n == 0: return ['%s;' % expr]
    if n == 1: return ['%s = %s;' % (reg_name(regs[0]), expr)]
    if n == 2:
        return ['{ const uint64_t %s = %s;' % (tmp, expr),
                '  %s = %s;' % (reg_name(regs[0]), from_bits(regs[0], 'lean_lo(%s)' % tmp)),
                '  %s = %s; }' % (reg_name(regs[1]), from_bits(regs[1], 'lean_hi(%s)' % tmp))]
    return ['{ const lean_r3 %s = %s;' % (tmp, expr),
            '  %s = %s;' % (reg_name(regs[0]), from_bits(regs[0], 'lean_lo(%s.a)' % tmp)),
            '  %s = %s;' % (reg_name(regs[1]), from_bits(regs[1], 'lean_hi(%s.a)' % tmp)),
            '  %s = %s; }' % (reg_name(regs[2]), from_bits(regs[2], '%s.b' % tmp))]

def cpu_ref(r):
    """g_cpu storage of a register"""
    if r < 32: return 'g_cpu.gpr[%d]' % r
    if r < 64: return 'g_cpu.fpr[%d]' % (r - 32)
    return {65: 'g_cpu.hi', 66: 'g_cpu.lo'}[r]

def load_from_cpu(r):
    return '%s = %s;' % (reg_name(r), cpu_ref(r))

def store_to_cpu(r):
    return '%s = %s;' % (cpu_ref(r), reg_name(r))

def signature(s, L, name=None):
    ps = params(s)
    args = ', '.join('%s %s' % (ctype(r), reg_name(r)) for r in ps) or 'void'
    return '%s %s(%s)' % (ret_type(rset(L, s)), name or 'f_%08X' % s.entry, args)
