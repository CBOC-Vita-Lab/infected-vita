"""C++ emission of one guest function (typed or regfile form)."""
import collections
from . import mips, analysis, abi, vfpu
from .mips import F, HI, LO, CC, RA, SP
from .analysis import OUT_ENC, V0, V1, F0, ARGS_NOSP, regs_in, RES_MASK
from .abi import reg_name, params, rset, ctype, ret_type, pack_return, unpack_return, cpu_ref

M32 = 0xFFFFFFFF
STRONG_BITS = (1 << 512) - 1

def u(x):
    x &= M32
    return '%du' % x if x < 10 else '0x%Xu' % x

def hexw(w): return '0x%08Xu' % w

class Unsupported(Exception):
    pass

def build_pc_index(f):
    idx = {}
    for n in f.nodes:
        if isinstance(n.key, int): idx[n.key] = n.id
    return idx

class FuncGen:
    def __init__(self, E, e):
        self.E = E; self.e = e
        self.P = E.P; self.L = E.L; self.im = E.P.im
        self.s = self.P.S[e]; self.f = self.s.f
        self.typed = self.s.typed
        self.nodes = self.f.nodes
        self.fa = analysis.analyze_function_abs(self.f, self.P, self.P.mw_ind, keep=True)
        sfa = self.s.fa
        if self.fa.promoted != sfa.promoted or set(self.fa.mem) != set(sfa.mem):
            raise AssertionError('analysis of %08x is not reproducible' % e)
        # a regfile function must hand back exactly the registers the caller expects: sp, ra and every callee-saved
        # register are live at its return (their restores must not be eliminated)
        extra = 0 if self.typed else (analysis.CALLEE_SAVED_MASK | (1 << 29) | (1 << 31))
        self.A, self.Aw, self.live_in, self.md = self.L.liveness(e, ret_extra=extra)
        self.fl = self.L.fl[e]
        self.ps = params(self.s) if self.typed else []
        self.rs_ret = rset(self.L, self.s) if self.typed else []
        self.used = set()           # registers referenced
        self.written = set()        # registers assigned
        self.lines = []
        self.targets = set()        # node ids that are goto targets
        self.uses_hsp = False
        # lowest entry-relative frame offset any unpromoted sp-relative access touches (<= 0): the frame base hb = g_ram + ((entry sp + frame_lo) & MASK)
        # makes every frame access a small positive displacement (Thumb-2 str/ldr/vldr cannot encode a large negative one)
        self.frame_lo = -((-min([0] + [m[1] - 3 for m in self.fa.mem.values() if m[2] is None]) + 3) // 4 * 4)
        self.al = {}                # register -> name of the pointer local g_ram + (reg & MASK) that is valid right now
        self.alias_out = {}         # node id -> alias state after the node
        self.alias_regs = set()
        self.slot_types = {}
        self.needs_jt = False
        self.cond_vars = set()
        self.notes = []
        self.nlines_stmt = 0
        self.fatal_tail = False
        self.vst, self.vissues = vfpu.analyze(self.f)      # prefix state before every node (None: no VFPU code)
        self.has_vfpu = self.vst is not None
        self.vf_fast = 0; self.vf_gen = 0

    # ----------------------------------------------------------------------- register access
    def R(self, r):
        if r == 0: return '0u'
        if r == 28: return u(self.im.gp)
        self.used.add(r)
        return reg_name(r)

    def Wr(self, r):
        self.used.add(r); self.written.add(r)
        self.al.pop(r, None)
        return reg_name(r)

    def addr(self, base, imm):
        imm &= M32
        if base == 0: return u(imm)
        b = self.R(base)
        if imm == 0: return b
        if imm >= 0x80000000: return '(%s - %s)' % (b, u((-imm) & M32))
        return '(%s + %s)' % (b, u(imm))

    def frame_ref(self, off):
        """C pointer expression of the stack byte at entry-sp-relative offset off (hb = g_ram + ((entry sp + frame_lo) & MASK), off - frame_lo >= 0)"""
        self.uses_hsp = True
        k = off - self.frame_lo
        return 'hb + %d' % k if k else 'hb'

    def alias_ptr(self, out, base, imm):
        """pointer expression for the guest address base + imm: the per-register pointer local b<base> = g_ram + (base & MASK) is
        shared by every access in the straight-line region where base is unchanged (the displacement is added to the masked address,
        which differs from masking the sum only for addresses outside the 32 MB RAM block, where the interpreter faults anyway)"""
        nm = self.al.get(base)
        if nm is None:
            nm = 'b%d' % base
            self.alias_regs.add(base)
            out.append('%s = g_ram + (%s & LEAN_MASK);' % (nm, self.R(base)))
            self.al[base] = nm
        v = mips.s16(imm & 0xFFFF)
        return nm if v == 0 else ('%s + %d' % (nm, v) if v > 0 else '%s - %d' % (nm, -v))

    # ----------------------------------------------------------------------- dead code
    def is_dead(self, nid):
        n = self.nodes[nid]
        if n.kind not in ('n', 'slot'): return False
        if self.fl.side[nid]: return False
        out = 0
        for sx in n.succ: out |= self.live_in[sx]
        return not (self.fl.dk[nid] & out)

    # ----------------------------------------------------------------------- slots
    def collect_slots(self):
        types = collections.defaultdict(set)
        for nid, m in self.fa.mem.items():
            if m[2] is None: continue
            d = self.nodes[nid].insn
            types[m[2]].add('f' if d.kind in ('Lwc1', 'Swc1') else 'i')
        for i, t in types.items():
            self.slot_types[i] = 'float' if t == {'f'} else 'uint32_t'

    def slot_ref(self, i): return 'sl%d' % i

    # ----------------------------------------------------------------------- instruction lowering
    def lower(self, nid, n):
        d = n.insn
        k = d.kind
        pc = d.pc
        rs, rt, rd, sa = d.rs, d.rt, d.rd, d.sa
        out = []
        A = out.append
        if d.fuse == 1:
            if d.kind in ('Lwl', 'Lwr', 'Swl', 'Swr'): return out
        m = self.fa.mem.get(nid)
        simm = d.simm

        def setr(r, expr):
            if r == 0: return
            A('%s = %s;' % (self.Wr(r), expr))

        # ---- memory
        if k in ('Lw', 'Lh', 'Lhu', 'Lb', 'Lbu', 'Lwc1', 'Sw', 'Sh', 'Sb', 'Swc1'):
            is_load = k[0] == 'L'
            fp = k in ('Lwc1', 'Swc1')
            if m is not None and m[2] is not None:
                ref = self.slot_ref(m[2]); ty = self.slot_types[m[2]]
                if is_load:
                    if fp: setr(F(rt), ref if ty == 'float' else 'lean_u2f(%s)' % ref)
                    else: setr(rt, ref)
                else:
                    if fp: A('%s = %s;' % (ref, self.R(F(rt)) if ty == 'float' else 'lean_f2u(%s)' % self.R(F(rt))))
                    else: A('%s = %s;' % (ref, self.R(rt)))
                return out
            suffix = ''
            if m is not None:
                suffix = '_p'
                a = self.frame_ref(m[1])
            elif rs not in (0, 28) and not self.E.no_alias:
                suffix = '_p'
                a = self.alias_ptr(out, rs, simm)
            else:
                a = self.addr(rs, simm)
            nm = {'Lw': 'lw', 'Lh': 'lh', 'Lhu': 'lhu', 'Lb': 'lb', 'Lbu': 'lbu', 'Lwc1': 'lwc1',
                  'Sw': 'sw', 'Sh': 'sh', 'Sb': 'sb', 'Swc1': 'swc1'}[k]
            if is_load:
                setr(F(rt) if fp else rt, 'lean_%s%s(%s)' % (nm, suffix, a))
            else:
                A('lean_%s%s(%s, %s);' % (nm, suffix, a, self.R(F(rt)) if fp else self.R(rt)))
            return out
        if k in ('Lwl', 'Lwr', 'Swl', 'Swr'):
            if d.fuse == 2:
                lr_off = simm if k in ('Lwr', 'Swr') else simm - 3
                if m is not None:
                    key = m[1] if k in ('Lwr', 'Swr') else m[1] - 3
                    loc = self.frame_ref(key)
                    if k in ('Lwl', 'Lwr'): setr(rt, 'lean_lw_p(%s)' % loc)
                    else: A('lean_sw_p(%s, %s);' % (loc, self.R(rt)))
                else:
                    a = self.addr(rs, lr_off)
                    if k in ('Lwl', 'Lwr'): setr(rt, 'lean_ulw(%s)' % a)
                    else: A('lean_usw(%s, %s);' % (a, self.R(rt)))
                return out
            a = self.addr(rs, simm)
            if k == 'Lwl': setr(rt, 'lean_lwl(%s, %s)' % (a, self.R(rt)))
            elif k == 'Lwr': setr(rt, 'lean_lwr(%s, %s)' % (a, self.R(rt)))
            elif k == 'Swl': A('lean_swl(%s, %s);' % (a, self.R(rt)))
            else: A('lean_swr(%s, %s);' % (a, self.R(rt)))
            return out

        # ---- integer
        if k == 'Nop' or k in ('Cache', 'Sync'): return out
        if k == 'Addiu':
            if rs == 0: setr(rt, u(simm))
            else: setr(rt, self.addr(rs, simm) if simm else self.R(rs))
            return out
        if k == 'Slti': setr(rt, '((int32_t)%s < %d)' % (self.R(rs), simm)); return out
        if k == 'Sltiu': setr(rt, '(%s < %s)' % (self.R(rs), u(simm))); return out
        if k == 'Andi': setr(rt, '(%s & %s)' % (self.R(rs), u(d.imm))); return out
        if k == 'Ori': setr(rt, '(%s | %s)' % (self.R(rs), u(d.imm)) if rs else u(d.imm)); return out
        if k == 'Xori': setr(rt, '(%s ^ %s)' % (self.R(rs), u(d.imm))); return out
        if k == 'Lui': setr(rt, u(d.imm << 16)); return out
        if k == 'Add': setr(rd, 'lean_add_trap(%s, %s, %s)' % (self.R(rs), self.R(rt), hexw(pc))); return out
        if k == 'Sub': setr(rd, 'lean_sub_trap(%s, %s, %s)' % (self.R(rs), self.R(rt), hexw(pc))); return out
        if k == 'Addu':
            if rs == 0: setr(rd, self.R(rt))
            elif rt == 0: setr(rd, self.R(rs))
            else: setr(rd, '(%s + %s)' % (self.R(rs), self.R(rt)))
            return out
        if k == 'Subu':
            setr(rd, '(%s - %s)' % (self.R(rs), self.R(rt))) if rt else setr(rd, self.R(rs)); return out
        if k == 'And': setr(rd, '(%s & %s)' % (self.R(rs), self.R(rt))); return out
        if k == 'Or':
            if rs == 0: setr(rd, self.R(rt))
            elif rt == 0: setr(rd, self.R(rs))
            else: setr(rd, '(%s | %s)' % (self.R(rs), self.R(rt)))
            return out
        if k == 'Xor': setr(rd, '(%s ^ %s)' % (self.R(rs), self.R(rt))); return out
        if k == 'Nor': setr(rd, '~(%s | %s)' % (self.R(rs), self.R(rt))); return out
        if k == 'Slt': setr(rd, 'lean_slt(%s, %s)' % (self.R(rs), self.R(rt))); return out
        if k == 'Sltu': setr(rd, '(%s < %s)' % (self.R(rs), self.R(rt))); return out
        if k == 'Max': setr(rd, 'lean_max(%s, %s)' % (self.R(rs), self.R(rt))); return out
        if k == 'Min': setr(rd, 'lean_min(%s, %s)' % (self.R(rs), self.R(rt))); return out
        if k == 'Movz':
            if rd: A('if (%s == 0u) %s = %s;' % (self.R(rt), self.Wr(rd), self.R(rs)))
            return out
        if k == 'Movn':
            if rd: A('if (%s != 0u) %s = %s;' % (self.R(rt), self.Wr(rd), self.R(rs)))
            return out
        if k == 'Sll': setr(rd, '(%s << %d)' % (self.R(rt), sa)); return out
        if k == 'Srl': setr(rd, '(%s >> %d)' % (self.R(rt), sa)); return out
        if k == 'Sra': setr(rd, 'lean_sra(%s, %d)' % (self.R(rt), sa)); return out
        if k == 'Rotr': setr(rd, 'lean_rotr(%s, %d)' % (self.R(rt), sa)); return out
        if k == 'Sllv': setr(rd, '(%s << (%s & 31u))' % (self.R(rt), self.R(rs))); return out
        if k == 'Srlv': setr(rd, '(%s >> (%s & 31u))' % (self.R(rt), self.R(rs))); return out
        if k == 'Srav': setr(rd, 'lean_sra(%s, %s)' % (self.R(rt), self.R(rs))); return out
        if k == 'Rotrv': setr(rd, 'lean_rotr(%s, %s)' % (self.R(rt), self.R(rs))); return out
        if k == 'Clz': setr(rd, 'lean_clz(%s)' % self.R(rs)); return out
        if k == 'Clo': setr(rd, 'lean_clo(%s)' % self.R(rs)); return out
        if k == 'Ext': setr(rt, 'lean_ext(%s, %d, %d)' % (self.R(rs), sa, rd + 1)); return out
        if k == 'Ins': setr(rt, 'lean_ins(%s, %s, %d, %d)' % (self.R(rt), self.R(rs), sa, rd)); return out
        if k == 'Seb': setr(rd, 'lean_seb(%s)' % self.R(rt)); return out
        if k == 'Seh': setr(rd, 'lean_seh(%s)' % self.R(rt)); return out
        if k == 'Bitrev': setr(rd, 'lean_bitrev(%s)' % self.R(rt)); return out
        if k == 'Wsbh': setr(rd, 'lean_wsbh(%s)' % self.R(rt)); return out
        if k == 'Wsbw': setr(rd, 'lean_wsbw(%s)' % self.R(rt)); return out
        if k in ('Mult', 'Multu', 'Div', 'Divu', 'Madd', 'Maddu', 'Msub', 'Msubu'):
            fn = {'Mult': 'mult', 'Multu': 'multu', 'Div': 'div', 'Divu': 'divu', 'Madd': 'madd', 'Maddu': 'maddu',
                  'Msub': 'msub', 'Msubu': 'msubu'}[k]
            self.used.add(HI); self.used.add(LO); self.written.add(HI); self.written.add(LO)
            A('lean_%s(%s, %s, hi, lo);' % (fn, self.R(rs), self.R(rt)))
            return out
        if k == 'Mfhi': setr(rd, self.R(HI)); return out
        if k == 'Mflo': setr(rd, self.R(LO)); return out
        if k == 'Mthi': A('%s = %s;' % (self.Wr(HI), self.R(rs))); return out
        if k == 'Mtlo': A('%s = %s;' % (self.Wr(LO), self.R(rs))); return out

        # ---- FPU
        if k == 'Mfc1': setr(rt, 'lean_f2u(%s)' % self.R(F(rd))); return out
        if k == 'Mtc1': A('%s = lean_u2f(%s);' % (self.Wr(F(rd)), self.R(rt))); return out
        if k == 'Cfc1':
            if rd != 31: A('pk_fatal("unsupported CFC1 control register", %s);' % hexw(pc)); return out
            setr(rt, 'lean_cfc1(%s)' % self.R(CC)); return out
        if k == 'Ctc1':
            if rd != 31: A('pk_fatal("unsupported CTC1 control register", %s);' % hexw(pc)); return out
            A('%s = lean_ctc1(%s);' % (self.Wr(CC), self.R(rt))); return out
        fd, fs, ft = F(sa), F(rd), F(rt)
        if k == 'AddS': A('%s = %s + %s;' % (self.Wr(fd), self.R(fs), self.R(ft))); return out
        if k == 'SubS': A('%s = %s - %s;' % (self.Wr(fd), self.R(fs), self.R(ft))); return out
        if k == 'MulS': A('%s = lean_mul_s(%s, %s);' % (self.Wr(fd), self.R(fs), self.R(ft))); return out
        if k == 'DivS': A('%s = %s / %s;' % (self.Wr(fd), self.R(fs), self.R(ft))); return out
        if k == 'SqrtS': A('%s = lean_sqrt_s(%s);' % (self.Wr(fd), self.R(fs))); return out
        if k == 'AbsS': A('%s = lean_abs_s(%s);' % (self.Wr(fd), self.R(fs))); return out
        if k == 'MovS': A('%s = %s;' % (self.Wr(fd), self.R(fs))); return out
        if k == 'NegS': A('%s = lean_neg_s(%s);' % (self.Wr(fd), self.R(fs))); return out
        if k in ('RoundWS', 'TruncWS', 'CeilWS', 'FloorWS', 'CvtWS'):
            fn = {'RoundWS': 'round_w', 'TruncWS': 'trunc_w', 'CeilWS': 'ceil_w', 'FloorWS': 'floor_w', 'CvtWS': 'cvt_w'}[k]
            A('%s = lean_u2f(lean_%s(%s));' % (self.Wr(fd), fn, self.R(fs))); return out
        if k == 'CvtSW': A('%s = lean_cvt_s_w(lean_f2u(%s));' % (self.Wr(fd), self.R(fs))); return out
        if k == 'FpuCompare':
            A('%s = lean_fcmp<%du>(%s, %s);' % (self.Wr(CC), d.w & 15, self.R(fs), self.R(ft))); return out

        # ---- VFPU and extra
        return self.lower_vfpu(d, out, nid)

    def lower_vfpu(self, d, out, nid=None):
        A = out.append
        if self.vst is not None and nid is not None and not self.E.no_vfpu_fast:
            fl = vfpu.fast_lines(self, d, self.vst.get(nid), nid)
            if fl is not None:
                self.E.stats['vfpu_fast'] += 1; self.vf_fast += 1
                self.E.vfpu_forms[vfpu.fast_name(d) or d.kind] += 1
                out += fl
                return out
        if d.vfpu: self.vf_gen += 1; self.E.stats['vfpu_generic'] += 1; self.E.vfpu_generic[d.kind if d.kind != 'Extra' else d.mn] += 1
        k = d.kind; w = hexw(d.w); pc = d.pc; rt = d.rt
        vf = 'lean_vf::'
        simple = {'Vflush': 'vflush', 'Vpfx': 'vpfx', 'Viim': 'viim', 'Vfim': 'vfim', 'Vf2h': 'vf2h', 'Vh2f': 'vh2f',
                  'Vi2x': 'vi2x', 'Vt5650': 'vt5650', 'Vrnd': 'vrnd', 'Vx2i': 'vx2i', 'Vf2i': 'vf2i', 'Vi2f': 'vi2f',
                  'VmidT': 'vmidt', 'VfpuMatrixInit': 'vmatrix_init', 'Vmmov': 'vmmov', 'Vmmul': 'vmmul', 'Vmscl': 'vmscl',
                  'Vtfm': 'vtfm', 'Vidt': 'vidt', 'VfpuVectorInit': 'vvector_init', 'Vcst': 'vcst', 'Vocp': 'vocp',
                  'VfpuHorizontal': 'vhorizontal', 'Vrot': 'vrot', 'VfpuVec3': 'vvec3', 'Vscl': 'vscl', 'Vdot': 'vdot',
                  'Vhdp': 'vhdp', 'Vcrs': 'vcrs', 'VcrossQuat': 'vcross_quat', 'Vminmax': 'vminmax',
                  'VfpuCompare3': 'vcompare3', 'Vcmp': 'vcmp', 'Vcmov': 'vcmov', 'VfpuUnary': 'vunary', 'Vsgn': 'vsgn'}
        if k in simple:
            A('%s%s(%s);' % (vf, simple[k], w)); return out
        if k == 'Vbfy1': A('%svbfy1(%s, %s);' % (vf, w, hexw(pc))); return out
        if k == 'Vsocp': A('%svsocp(%s, %s);' % (vf, w, hexw(pc))); return out
        if k == 'Mtv': A('%smtv(%s, %s);' % (vf, w, self.R(rt))); return out
        if k == 'Mfv':
            if rt: A('%s = %smfv(%s);' % (self.Wr(rt), vf, w))
            return out
        if k in ('Lvs', 'Svs', 'Lvq', 'Svq'):
            off = mips.s16(d.w & 0xFFFC)
            a = self.addr(d.rs, off)
            A('%s%s(%s, %s);' % (vf, {'Lvs': 'lvs', 'Svs': 'svs', 'Lvq': 'lvq', 'Svq': 'svq'}[k], w, a)); return out
        if k == 'Extra':
            mn = d.mn
            if mn in ('lvl.q', 'lvr.q', 'svl.q', 'svr.q'):
                off = mips.s16(d.w & 0xFFFC)
                a = self.addr(d.rs, off)
                A('%squad_unaligned(%s, %s, %d);' % (vf, w, a, ['lvl.q', 'lvr.q', 'svl.q', 'svr.q'].index(mn))); return out
            if mn == 'll':
                if d.rt: A('%s = lean_lw(%s);' % (self.Wr(d.rt), self.addr(d.rs, d.simm)))
                return out
            if mn == 'sc':
                A('lean_sw(%s, %s);' % (self.addr(d.rs, d.simm), self.R(d.rt)))
                if d.rt: A('%s = 1u;' % self.Wr(d.rt))
                return out
            fn = {'vsbn': 'vsbn', 'vdet': 'vdet', 'vwbn': 'vwbn', 'vrnds': 'vrnds', 'vsbz': 'vsbz', 'vlgb': 'vlgb',
                  'vmfvc': 'vmfvc', 'vmtvc': 'vmtvc'}.get(mn)
            if fn: A('%s%s(%s);' % (vf, fn, w)); return out
            if mn == 'vi2c': A('%svi2c_vi2us(%s, true);' % (vf, w)); return out
            if mn == 'vi2us': A('%svi2c_vi2us(%s, false);' % (vf, w)); return out
            if mn in ('vsrt1', 'vsrt2', 'vsrt3', 'vsrt4'): A('%svsort(%s, %d);' % (vf, w, int(mn[-1]) - 1)); return out
            if mn == 'vbfy2': A('%svsort(%s, 4);' % (vf, w)); return out
            if mn == 'vt4444': A('%svt4444_5551(%s, true);' % (vf, w)); return out
            if mn == 'vt5551': A('%svt4444_5551(%s, false);' % (vf, w)); return out
        A('pk_fatal("unsupported instruction %s", %s);' % (d.mn, hexw(pc)))
        return out

    # ----------------------------------------------------------------------- branch conditions
    def cond_expr(self, d):
        m = d.mn[:-1] if d.likely else d.mn
        rs, rt = d.rs, d.rt
        if m == 'beq': return '(%s == %s)' % (self.R(rs), self.R(rt))
        if m == 'bne': return '(%s != %s)' % (self.R(rs), self.R(rt))
        if m == 'blez': return '((int32_t)%s <= 0)' % self.R(rs)
        if m == 'bgtz': return '((int32_t)%s > 0)' % self.R(rs)
        if m == 'bltz': return '((int32_t)%s < 0)' % self.R(rs)
        if m == 'bgez': return '((int32_t)%s >= 0)' % self.R(rs)
        if m == 'bc1f': return '(%s == 0u)' % self.R(CC)
        if m == 'bc1t': return '(%s != 0u)' % self.R(CC)
        if m == 'bvf': return '(lean_vf::cc_bit(%s) == 0u)' % hexw(d.w)
        if m == 'bvt': return '(lean_vf::cc_bit(%s) != 0u)' % hexw(d.w)
        raise Unsupported('branch %s' % d.mn)

    # ----------------------------------------------------------------------- calls
    def marshal_to_cpu(self, regs):
        out = []
        for r in regs:
            out.append('%s = %s;' % (cpu_ref(r), self.R(r)))
        return out

    def call_lines(self, nd, tail):
        """statements for a call-completion ('cont') or tail node; returns lines; the caller adds the return for tails"""
        lines = []
        d = nd.insn
        t = nd.tgt
        if nd.kind == 'cont' and d.mn == 'jalr':
            ms = (self.md[nd.id] or 0) | self.s.A | self.s.Aw
            ms &= ARGS_NOSP
            # v0 / v1 / f0 that are live after the call and defined here are marshalled too: an indirect callee (its thunk
            # stores only the registers it returns) that leaves one untouched hands the caller's value straight back
            after = 0
            for sx in nd.succ: after |= self.live_in[sx]
            ms |= (self.md[nd.id] or 0) & after & OUT_ENC
            regs = regs_in(ms)
            lines += self.marshal_to_cpu(regs)
            lines.append('g_cpu.gpr[29] = %s;' % self.R(SP))
            lines.append('pk_guest_call(jt);')
            self.needs_jt = True
            for r in (V0, V1, F0):
                lines.append('%s = %s;' % (self.Wr(r), cpu_ref(r)))
            return lines
        if t in self.P.imports:
            regs = regs_in(self.L.ar.get(t, 0))
            lines += self.marshal_to_cpu(regs)
            lines.append('g_cpu.gpr[29] = %s;' % self.R(SP))
            lines.append('pk_hle_call(%s);' % hexw(t))
            for r in (V0, V1, F0):
                lines.append('%s = %s;' % (self.Wr(r), cpu_ref(r)))
            self.E.stats['import_calls'] += 1
            return lines
        ts = self.P.S.get(t)
        if ts is None:
            lines.append('g_cpu.gpr[29] = %s;' % self.R(SP))
            lines += self.marshal_to_cpu([r for r in regs_in(ARGS_NOSP)])
            lines.append('pk_guest_call(%s);' % hexw(t))
            for r in (V0, V1, F0):
                lines.append('%s = %s;' % (self.Wr(r), cpu_ref(r)))
            self.E.fallbacks.append((self.e, t, 'direct call target is not a known function'))
            return lines
        if ts.typed:
            args = ', '.join(self.R(r) for r in params(ts))
            callee = 'f_%08X(%s)' % (t, args)
            rr = rset(self.L, ts)
            for r in rr: self.Wr(r)
            lines += unpack_return(rr, callee)
        else:
            regs = params(analysis_params_regfile(ts))
            if SP not in regs: regs = [SP] + regs
            # a regfile callee hands back the register file in place: an output it leaves untouched on some path (a callee-saved
            # register an inner ABI violator writes, v0 it never defines) keeps whatever the caller stored, so every register the
            # callee returns that the caller still reads (or, at a tail call, passes on) is stored before the call as well
            after = 0
            if tail: after = -1
            else:
                for sx in nd.succ: after |= self.live_in[sx]
            extra = [r for r in regs_in(self.L.rp(ts) & after) if r not in regs and r not in (0, 28, CC, SP, RA) and r <= 66]
            regs = regs + extra
            lines += self.marshal_to_cpu(regs)
            lines.append('t_%08X();' % t)
            for r in regs_in(self.L.rp(ts)):
                if r in (SP, RA) or r > 66: continue
                lines.append('%s = %s;' % (self.Wr(r), cpu_ref(r)))
        return lines

    def return_lines(self, nd):
        if self.typed:
            regs = self.rs_ret
            if not regs: return ['return;']
            for r in regs: self.R(r)
            return ['return %s;' % pack_return(regs)]
        return ['goto L_ret;']

    # ----------------------------------------------------------------------- switch
    def switch_lines(self, nd, label_of):
        data = nd.data
        ents = data['entries']
        idx = data.get('idx_reg')
        ok = data.get('mode') == 'idx'
        shifted = ok and data.get('shifted')
        lines = []
        groups = collections.OrderedDict()
        if ok:
            for i, e in enumerate(ents): groups.setdefault(e, []).append(i)
            lines.append('switch (%s) {' % (self.R(idx) + (' >> 2' if shifted else '')))
            for e, idxs in groups.items():
                tgt = self.node_for_pc(e)
                self.targets.add(tgt)
                lines.append('  ' + ' '.join('case %du:' % i for i in idxs) + ' goto L%d;' % tgt)
            lines.append('  default: pk_fatal("jump table index out of range", %s);' % hexw(nd.pc))
            lines.append('}')
        else:
            jr = nd.insn
            uniq = []
            for e in ents:
                if e not in uniq: uniq.append(e)
            lines.append('switch (%s) {' % self.R(jr.rs))
            for e in uniq:
                tgt = self.node_for_pc(e); self.targets.add(tgt)
                lines.append('  case %s: goto L%d;' % (hexw(e), tgt))
            lines.append('  default: pk_fatal("indirect jump to unknown target", %s);' % hexw(nd.pc))
            lines.append('}')
            self.E.stats['value_switch'] += 1
        return lines

    def node_for_pc(self, pc):
        n = self.f.by_key.get(pc)
        if n is None: n = self.f.by_key.get(('T', pc))
        if n is None: raise AssertionError('no node for %08x in %08x' % (pc, self.e))
        return n.id

    # ----------------------------------------------------------------------- layout
    def preferred_next(self, n):
        k = n.kind
        sc = n.succ
        if not sc: return None
        if k == 'slot' and len(sc) == 2: return sc[1]
        if k == 'br' and len(sc) == 2: return sc[1]
        return sc[0]

    def order_nodes(self):
        nodes = self.nodes
        start = self.f.by_key[self.f.entry].id
        seen = set(); order = []
        pending = [start]
        while pending:
            cur = pending.pop()
            while cur is not None and cur not in seen:
                seen.add(cur); order.append(cur)
                n = nodes[cur]
                nxt = self.preferred_next(n)
                for sx in reversed(n.succ):
                    if sx != nxt and sx not in seen: pending.append(sx)
                cur = nxt if (nxt is not None and nxt not in seen) else None
        return order

    def back_edge_targets(self):
        """loop headers: targets of DFS back edges"""
        nodes = self.nodes
        start = self.f.by_key[self.f.entry].id
        color = {}
        heads = set()
        stack = [(start, 0)]
        color[start] = 1
        while stack:
            i, j = stack[-1]
            su = nodes[i].succ
            if j < len(su):
                stack[-1] = (i, j + 1)
                s2 = su[j]
                c = color.get(s2, 0)
                if c == 0:
                    color[s2] = 1; stack.append((s2, 0))
                elif c == 1:
                    heads.add(s2)
            else:
                color[i] = 2; stack.pop()
        return heads

    # ----------------------------------------------------------------------- the function
    def gen(self):
        nodes = self.nodes
        self.collect_slots()
        order = self.order_nodes()
        pos = {nid: i for i, nid in enumerate(order)}
        yield_fn = self.E.yield_set and self.e in self.E.yield_set
        heads = self.back_edge_targets() if yield_fn else set()
        body = []                 # (node id or None, [lines])
        P = self.P

        def goto(src_idx, tgt):
            """control transfer to node tgt from the node emitted at position src_idx"""
            nxt = order[src_idx + 1] if src_idx + 1 < len(order) else None
            if tgt == nxt: return []
            self.targets.add(tgt)
            return ['goto L%d;' % tgt]

        for i, nid in enumerate(order):
            n = nodes[nid]
            k = n.kind
            L_ = []
            # alias state: inherited from the single predecessor when it was emitted earlier, empty at joins / loop headers
            self.al = dict(self.alias_out[n.pred[0]]) if len(n.pred) == 1 and n.pred[0] in self.alias_out else {}
            if k in ('n', 'slot'):
                if not self.is_dead(nid):
                    L_ += self.lower(nid, n)
                if k == 'slot':
                    pr = nodes[n.pred[0]] if n.pred else None
                    if pr is not None and pr.kind == 'br' and not pr.insn.likely and len(n.succ) == 2:
                        cv = 'c_%x' % pr.pc
                        self.cond_vars.add(cv)
                        t_, f_ = n.succ
                        self.targets.add(t_)
                        L_.append('if (%s) goto L%d;' % (cv, t_))
                        L_ += goto(i, f_)
                        body.append((nid, L_)); self.alias_out[nid] = dict(self.al); continue
                if len(n.succ) == 1: L_ += goto(i, n.succ[0])
                elif len(n.succ) == 0: L_.append('pk_fatal("fell off the function", %s);' % hexw(n.pc))
                else: raise AssertionError('slot/n with %d succ at %08x' % (len(n.succ), n.pc))
            elif k == 'br':
                d = n.insn
                if d.mn in ('bltzal', 'bgezal', 'bltzall', 'bgezall'):
                    raise Unsupported('branch and link at %08x' % n.pc)
                const = n.data.get('const') if isinstance(n.data, dict) else None
                if d.likely:
                    if len(n.succ) == 2:
                        sl, fall = n.succ
                        self.targets.add(sl)
                        L_.append('if (%s) goto L%d;' % (self.cond_expr(d), sl))
                        L_ += goto(i, fall)
                    else:
                        L_ += goto(i, n.succ[0])
                else:
                    if const is None:
                        cv = 'c_%x' % n.pc
                        self.cond_vars.add(cv)
                        L_.append('%s = %s;' % (cv, self.cond_expr(d)))
                    L_ += goto(i, n.succ[0])
            elif k in ('jmp', 'jr'):
                L_ += goto(i, n.succ[0])
            elif k == 'call':
                d = n.insn
                if d.mn == 'jalr':
                    L_.append('jt = %s;' % self.R(d.rs)); self.needs_jt = True
                    out_live = 0
                    for sx in n.succ: out_live |= self.live_in[sx]
                    if d.rd and (out_live >> d.rd & 1):
                        L_.append('%s = %s;' % (self.Wr(d.rd), u(d.pc + 8)))
                else:
                    out_live = 0
                    for sx in n.succ: out_live |= self.live_in[sx]
                    if out_live >> RA & 1:
                        L_.append('%s = %s;' % (self.Wr(RA), u(d.pc + 8)))
                L_ += goto(i, n.succ[0])
            elif k == 'cont':
                if n.data == 'noreturn':
                    L_ += self.call_lines(n, False)
                    L_.append('pk_fatal("noreturn function returned", %s);' % hexw(n.pc))
                else:
                    L_ += self.call_lines(n, False)
                    if self.has_vfpu: L_.append('LEAN_VFPU_ENTRY_CHECK(%s);' % hexw(n.pc))
                    if len(n.succ) == 1: L_ += goto(i, n.succ[0])
                    else: L_.append('pk_fatal("call continuation missing", %s);' % hexw(n.pc))
            elif k == 'tail':
                L_ += self.call_lines(n, True)
                L_ += self.return_lines(n)
            elif k == 'ret':
                if not self.typed:
                    L_.append('if (%s != ra0) pk_fatal("return through a foreign return address", %s);' % (self.R(RA), hexw(n.pc)))
                    L_.append('if (%s != sp0) pk_fatal("stack pointer not balanced at return", %s);' % (self.R(SP), hexw(n.pc)))
                L_ += self.return_lines(n)
            elif k == 'switch':
                L_ += self.switch_lines(n, None)
            elif k == 'ijmp':
                L_.append('pk_fatal("unresolved indirect jump", %s);' % hexw(n.pc))
            elif k == 'fatal':
                d = n.insn
                what = n.data if isinstance(n.data, str) else 'fatal'
                L_.append('pk_fatal("%s", %s);' % (what, hexw(n.pc)))
            else:
                raise AssertionError(k)
            body.append((nid, L_))
            self.alias_out[nid] = dict(self.al)
        self.body = body
        self.heads = heads
        return self.assemble()

    # ----------------------------------------------------------------------- assembly
    def assemble(self):
        e = self.e
        s = self.s
        out = []
        regfile = not self.typed
        if self.typed:
            out.append('%s {' % abi.signature(s, self.L))
        else:
            out.append('void t_%08X() {' % e)
        pre = []
        if regfile:
            # load every register read before written (full live-in), plus sp / ra for the exit checks
            li = self.live_in[self.f.by_key[self.f.entry].id] & STRONG_BITS & RES_MASK
            load = set(regs_in(li)) | {SP, RA}
            load -= {0, 28, CC}
            for r in sorted(load):
                self.used.add(r)
            self.load_regs = sorted(load)
        if self.uses_hsp: self.used.add(SP)
        if regfile: self.used |= {SP, RA}
        # declarations
        decl_i = []; decl_f = []
        pset = set(self.ps)
        for r in sorted(self.used):
            if r in pset or r == 0: continue
            if r == CC or r in (HI, LO) or r < 32: decl_i.append(reg_name(r))
            else: decl_f.append(reg_name(r))
        if decl_i: pre.append('uint32_t %s;' % ', '.join('%s = 0' % x for x in decl_i))
        if decl_f: pre.append('float %s;' % ', '.join('%s = 0.0f' % x for x in decl_f))
        for i, ty in sorted(self.slot_types.items()):
            pre.append('%s %s = %s;' % (ty, self.slot_ref(i), '0.0f' if ty == 'float' else '0'))
        if self.cond_vars: pre.append('uint32_t %s;' % ', '.join('%s = 0' % c for c in sorted(self.cond_vars)))
        if self.needs_jt: pre.append('uint32_t jt = 0;')
        if regfile:
            for r in self.load_regs: pre.append('%s = %s;' % (reg_name(r), cpu_ref(r)))
            pre.append('const uint32_t sp0 = sp, ra0 = r31;')
        if self.alias_regs:
            pre.append('uint8_t ' + ', '.join('*b%d = nullptr' % r for r in sorted(self.alias_regs)) + ';')
        if self.uses_hsp:
            # the frame base is derived from the entry sp itself (not from a second pointer) so the compiler cannot fold the
            # displacements back into negative offsets from the entry sp
            pre.append('uint8_t* const hb = g_ram + ((sp - %du) & LEAN_MASK);' % (-self.frame_lo) if self.frame_lo else 'uint8_t* const hb = g_ram + (sp & LEAN_MASK);')
        out += ['  ' + x for x in pre if x]
        have = pset if self.typed else set(self.load_regs)
        tmask = 0; targs = []
        for i, r in enumerate((4, 5, 6, 7)):
            if r in have: tmask |= 1 << i; targs.append(reg_name(r))
            else: targs.append('0u')
        out.append('  LEAN_TRC(0x%08XU, %d, %s);' % (e, tmask, ', '.join(targs)))
        hook = self.E.hook_prologue(self)
        if hook: out += ['  ' + x for x in hook]
        if self.has_vfpu: out.append('  LEAN_VFPU_ENTRY_CHECK(0x%08XU);' % e)
        for nid, lines in self.body:
            if nid in self.targets:
                out.append('L%d:;' % nid)
                if nid in self.heads: out.append('  PK_YIELD();')
            for ln in lines:
                if ln: out.append('  ' + ln)
        if regfile:
            out.append('L_ret:;')
            wr = sorted(r for r in self.written if r not in (0, 28, CC, SP, RA))
            for r in wr:
                out.append('  %s = %s;' % (cpu_ref(r), reg_name(r)))
            self.store_regs = wr
        out.append('}')
        # the register declarations were computed before the body of regfile loads; recompute 'used' for sp in hsp
        return out

def analysis_params_regfile(ts):
    """regfile callee: the registers a caller must store before the call (A | Aw and sp)"""
    class _T: pass
    t = _T(); t.A = ts.A | ts.Aw; t.Aw = 0
    return t
