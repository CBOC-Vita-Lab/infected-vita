// emit.go mirrors recomp/leangen/emit.py: C++ emission of one guest function (typed or regfile form).
package leangen

import (
	"fmt"
	"sort"
	"strings"
)

func u(x uint32) string {
	if x < 10 {
		return fmt.Sprintf("%du", x)
	}
	return fmt.Sprintf("0x%Xu", x)
}

func hexw(w uint32) string { return fmt.Sprintf("0x%08Xu", w) }

// unsupportedError is Python's emit.Unsupported.
type unsupportedError struct{ msg string }

func (e *unsupportedError) Error() string { return e.msg }

// FuncGen generates the C++ of one function.
type FuncGen struct {
	E         *Emitter
	e         uint32
	P         *Program2
	L         *Solver
	im        *Image
	s         *Summary
	f         *Func
	typed     bool
	nodes     []*Node
	fa        *FA
	A, Aw     Mask
	liveIn    []Bits
	md        []Mask
	mdOK      bool
	fl        *FL
	ps        []int
	rsRet     []int
	used      [NRES]bool
	written   [NRES]bool
	targets   map[int]struct{}
	usesHsp   bool
	frameLo   int
	al        uint32 // registers whose pointer local b<reg> is valid right now
	aliasOut  []uint32
	aliasSet  []bool
	aliasReg  uint32 // registers that got a pointer local
	slotTypes map[int]string
	needsJt   bool
	condVars  map[string]struct{}
	vst       map[int]vstate
	vissues   []vIssue
	hasVfpu   bool
	vfFast    int
	vfGen     int
	body      []bodyEnt
	heads     map[int]struct{}
	loadRegs  []int
	storeRegs []int
	keyIndex  map[Key]*Node
}

type bodyEnt struct {
	nid   int
	lines []string
}

// NewFuncGen ports FuncGen.__init__.
func NewFuncGen(E *Emitter, e uint32) *FuncGen {
	g := &FuncGen{E: E, e: e, P: E.P, L: E.L, im: E.P.Im}
	g.s = g.P.S[e]
	g.f = g.s.F
	g.typed = g.s.Typed
	g.nodes = g.f.Nodes
	g.fa = g.s.FA // the analysis is reproducible (the Python emitter asserts it); the one from phase 1 is the final one
	// a regfile function must hand back exactly the registers the caller expects: sp, ra and every callee-saved register are live
	// at its return (their restores must not be eliminated)
	var extra Mask
	if !g.typed {
		extra = calleeSaved.Or(bitMask(29)).Or(bitMask(31))
	}
	g.A, g.Aw, g.liveIn, g.md, g.mdOK = g.L.Liveness(e, extra)
	g.fl = g.L.fl[e]
	if g.typed {
		g.ps = params(g.s)
		g.rsRet = g.L.rset(g.s)
	}
	g.targets = map[int]struct{}{}
	g.condVars = map[string]struct{}{}
	g.slotTypes = map[int]string{}
	// lowest entry-relative frame offset any unpromoted sp-relative access touches (<= 0): the frame base hb = g_ram + ((entry sp +
	// frame_lo) & MASK) makes every frame access a small positive displacement (Thumb-2 str/ldr/vldr cannot encode a large negative
	// one)
	x := 0
	for _, m := range g.fa.Mem {
		if m.Has && m.Slot < 0 && int(m.Key)-3 < x {
			x = int(m.Key) - 3
		}
	}
	g.frameLo = -((-x + 3) / 4 * 4)
	g.aliasOut = make([]uint32, len(g.nodes))
	g.aliasSet = make([]bool, len(g.nodes))
	g.vst, g.vissues = vfpuAnalyze(g.f)
	g.hasVfpu = g.vst != nil
	return g
}

// ----------------------------------------------------------------------- register access

// R ports R(): the C name of register r for a read.
func (g *FuncGen) R(r int) string {
	if r == 0 {
		return "0u"
	}
	if r == 28 {
		return u(g.im.Gp)
	}
	g.used[r] = true
	return regName(r)
}

// Wr ports Wr(): the C name of register r for a write.
func (g *FuncGen) Wr(r int) string {
	g.used[r] = true
	g.written[r] = true
	if r < 32 {
		g.al &^= 1 << uint(r)
	}
	return regName(r)
}

func (g *FuncGen) addr(base int, imm uint32) string {
	if base == 0 {
		return u(imm)
	}
	b := g.R(base)
	if imm == 0 {
		return b
	}
	if imm >= 0x80000000 {
		return fmt.Sprintf("(%s - %s)", b, u(-imm))
	}
	return fmt.Sprintf("(%s + %s)", b, u(imm))
}

// frameRef ports frame_ref(): C pointer expression of the stack byte at entry-sp-relative offset off.
func (g *FuncGen) frameRef(off int) string {
	g.usesHsp = true
	k := off - g.frameLo
	if k != 0 {
		return fmt.Sprintf("hb + %d", k)
	}
	return "hb"
}

// aliasPtr ports alias_ptr(): pointer expression for the guest address base + imm: the per-register pointer local
// b<base> = g_ram + (base & MASK) is shared by every access in the straight-line region where base is unchanged (the
// displacement is added to the masked address, which differs from masking the sum only for addresses outside the 32 MB RAM block,
// where the interpreter faults anyway)
func (g *FuncGen) aliasPtr(out *[]string, base int, imm uint32) string {
	nm := fmt.Sprintf("b%d", base)
	if g.al>>uint(base)&1 == 0 {
		g.aliasReg |= 1 << uint(base)
		*out = append(*out, fmt.Sprintf("%s = g_ram + (%s & LEAN_MASK);", nm, g.R(base)))
		g.al |= 1 << uint(base)
	}
	v := s16(imm & 0xFFFF)
	switch {
	case v == 0:
		return nm
	case v > 0:
		return fmt.Sprintf("%s + %d", nm, v)
	}
	return fmt.Sprintf("%s - %d", nm, -v)
}

// ----------------------------------------------------------------------- dead code

func (g *FuncGen) isDead(nid int) bool {
	n := g.nodes[nid]
	if n.Kind != NN && n.Kind != NSlot {
		return false
	}
	if g.fl.side[nid] {
		return false
	}
	var out Bits
	for _, sx := range n.Succ {
		out = out.or(g.liveIn[sx])
	}
	return !g.fl.defs[nid].both().intersects(out)
}

// ----------------------------------------------------------------------- slots

func (g *FuncGen) collectSlots() {
	types := map[int]uint8{} // 1: float, 2: int
	for nid, m := range g.fa.Mem {
		if !m.Has || m.Slot < 0 {
			continue
		}
		d := g.nodes[nid].Insn
		if d.Kind == KLwc1 || d.Kind == KSwc1 {
			types[int(m.Slot)] |= 1
		} else {
			types[int(m.Slot)] |= 2
		}
	}
	for i, t := range types {
		if t == 1 {
			g.slotTypes[i] = "float"
		} else {
			g.slotTypes[i] = "uint32_t"
		}
	}
}

func slotRef(i int) string { return fmt.Sprintf("sl%d", i) }

// ----------------------------------------------------------------------- instruction lowering

var memNames = map[Kind]string{KLw: "lw", KLh: "lh", KLhu: "lhu", KLb: "lb", KLbu: "lbu", KLwc1: "lwc1",
	KSw: "sw", KSh: "sh", KSb: "sb", KSwc1: "swc1"}

var mulNames = map[Kind]string{KMult: "mult", KMultu: "multu", KDiv: "div", KDivu: "divu", KMadd: "madd", KMaddu: "maddu",
	KMsub: "msub", KMsubu: "msubu"}

var cvtNames = map[Kind]string{KRoundWS: "round_w", KTruncWS: "trunc_w", KCeilWS: "ceil_w", KFloorWS: "floor_w", KCvtWS: "cvt_w"}

func (g *FuncGen) lower(nid int, n *Node) []string {
	d := n.Insn
	k := d.Kind
	pc := d.PC
	rs, rt, rd, sa := int(d.Rs), int(d.Rt), int(d.Rd), int(d.Sa)
	var out []string
	A := func(s string) { out = append(out, s) }
	if d.Fuse == 1 {
		if k == KLwl || k == KLwr || k == KSwl || k == KSwr {
			return out
		}
	}
	m := g.fa.Mem[nid]
	simm := d.Simm
	setr := func(r int, expr string) {
		if r == 0 {
			return
		}
		A(fmt.Sprintf("%s = %s;", g.Wr(r), expr))
	}

	// ---- memory
	switch k {
	case KLw, KLh, KLhu, KLb, KLbu, KLwc1, KSw, KSh, KSb, KSwc1:
		isLoad := k == KLw || k == KLh || k == KLhu || k == KLb || k == KLbu || k == KLwc1
		fp := k == KLwc1 || k == KSwc1
		if m.Has && m.Slot >= 0 {
			ref := slotRef(int(m.Slot))
			ty := g.slotTypes[int(m.Slot)]
			if isLoad {
				if fp {
					if ty == "float" {
						setr(F(rt), ref)
					} else {
						setr(F(rt), fmt.Sprintf("lean_u2f(%s)", ref))
					}
				} else {
					setr(rt, ref)
				}
			} else {
				if fp {
					if ty == "float" {
						A(fmt.Sprintf("%s = %s;", ref, g.R(F(rt))))
					} else {
						A(fmt.Sprintf("%s = lean_f2u(%s);", ref, g.R(F(rt))))
					}
				} else {
					A(fmt.Sprintf("%s = %s;", ref, g.R(rt)))
				}
			}
			return out
		}
		suffix := ""
		var a string
		if m.Has {
			suffix = "_p"
			a = g.frameRef(int(m.Key))
		} else if rs != 0 && rs != 28 && !g.E.NoAlias {
			suffix = "_p"
			a = g.aliasPtr(&out, rs, uint32(simm))
		} else {
			a = g.addr(rs, uint32(simm))
		}
		nm := memNames[k]
		if isLoad {
			r := rt
			if fp {
				r = F(rt)
			}
			setr(r, fmt.Sprintf("lean_%s%s(%s)", nm, suffix, a))
		} else {
			r := rt
			if fp {
				r = F(rt)
			}
			A(fmt.Sprintf("lean_%s%s(%s, %s);", nm, suffix, a, g.R(r)))
		}
		return out
	case KLwl, KLwr, KSwl, KSwr:
		if d.Fuse == 2 {
			lrOff := simm - 3
			if k == KLwr || k == KSwr {
				lrOff = simm
			}
			if m.Has {
				key := int(m.Key) - 3
				if k == KLwr || k == KSwr {
					key = int(m.Key)
				}
				loc := g.frameRef(key)
				if k == KLwl || k == KLwr {
					setr(rt, fmt.Sprintf("lean_lw_p(%s)", loc))
				} else {
					A(fmt.Sprintf("lean_sw_p(%s, %s);", loc, g.R(rt)))
				}
			} else {
				a := g.addr(rs, uint32(lrOff))
				if k == KLwl || k == KLwr {
					setr(rt, fmt.Sprintf("lean_ulw(%s)", a))
				} else {
					A(fmt.Sprintf("lean_usw(%s, %s);", a, g.R(rt)))
				}
			}
			return out
		}
		a := g.addr(rs, uint32(simm))
		switch k {
		case KLwl:
			setr(rt, fmt.Sprintf("lean_lwl(%s, %s)", a, g.R(rt)))
		case KLwr:
			setr(rt, fmt.Sprintf("lean_lwr(%s, %s)", a, g.R(rt)))
		case KSwl:
			A(fmt.Sprintf("lean_swl(%s, %s);", a, g.R(rt)))
		default:
			A(fmt.Sprintf("lean_swr(%s, %s);", a, g.R(rt)))
		}
		return out
	}

	// ---- integer
	switch k {
	case KNop, KCache, KSync:
		return out
	case KAddiu:
		if rs == 0 {
			setr(rt, u(uint32(simm)))
		} else if simm != 0 {
			setr(rt, g.addr(rs, uint32(simm)))
		} else {
			setr(rt, g.R(rs))
		}
		return out
	case KSlti:
		setr(rt, fmt.Sprintf("((int32_t)%s < %d)", g.R(rs), simm))
		return out
	case KSltiu:
		setr(rt, fmt.Sprintf("(%s < %s)", g.R(rs), u(uint32(simm))))
		return out
	case KAndi:
		setr(rt, fmt.Sprintf("(%s & %s)", g.R(rs), u(d.Imm)))
		return out
	case KOri:
		if rs != 0 {
			setr(rt, fmt.Sprintf("(%s | %s)", g.R(rs), u(d.Imm)))
		} else {
			setr(rt, u(d.Imm))
		}
		return out
	case KXori:
		setr(rt, fmt.Sprintf("(%s ^ %s)", g.R(rs), u(d.Imm)))
		return out
	case KLui:
		setr(rt, u(d.Imm<<16))
		return out
	case KAdd:
		setr(rd, fmt.Sprintf("lean_add_trap(%s, %s, %s)", g.R(rs), g.R(rt), hexw(pc)))
		return out
	case KSub:
		setr(rd, fmt.Sprintf("lean_sub_trap(%s, %s, %s)", g.R(rs), g.R(rt), hexw(pc)))
		return out
	case KAddu:
		if rs == 0 {
			setr(rd, g.R(rt))
		} else if rt == 0 {
			setr(rd, g.R(rs))
		} else {
			setr(rd, fmt.Sprintf("(%s + %s)", g.R(rs), g.R(rt)))
		}
		return out
	case KSubu:
		if rt != 0 {
			setr(rd, fmt.Sprintf("(%s - %s)", g.R(rs), g.R(rt)))
		} else {
			setr(rd, g.R(rs))
		}
		return out
	case KAnd:
		setr(rd, fmt.Sprintf("(%s & %s)", g.R(rs), g.R(rt)))
		return out
	case KOr:
		if rs == 0 {
			setr(rd, g.R(rt))
		} else if rt == 0 {
			setr(rd, g.R(rs))
		} else {
			setr(rd, fmt.Sprintf("(%s | %s)", g.R(rs), g.R(rt)))
		}
		return out
	case KXor:
		setr(rd, fmt.Sprintf("(%s ^ %s)", g.R(rs), g.R(rt)))
		return out
	case KNor:
		setr(rd, fmt.Sprintf("~(%s | %s)", g.R(rs), g.R(rt)))
		return out
	case KSlt:
		setr(rd, fmt.Sprintf("lean_slt(%s, %s)", g.R(rs), g.R(rt)))
		return out
	case KSltu:
		setr(rd, fmt.Sprintf("(%s < %s)", g.R(rs), g.R(rt)))
		return out
	case KMax:
		setr(rd, fmt.Sprintf("lean_max(%s, %s)", g.R(rs), g.R(rt)))
		return out
	case KMin:
		setr(rd, fmt.Sprintf("lean_min(%s, %s)", g.R(rs), g.R(rt)))
		return out
	case KMovz:
		if rd != 0 {
			A(fmt.Sprintf("if (%s == 0u) %s = %s;", g.R(rt), g.Wr(rd), g.R(rs)))
		}
		return out
	case KMovn:
		if rd != 0 {
			A(fmt.Sprintf("if (%s != 0u) %s = %s;", g.R(rt), g.Wr(rd), g.R(rs)))
		}
		return out
	case KSll:
		setr(rd, fmt.Sprintf("(%s << %d)", g.R(rt), sa))
		return out
	case KSrl:
		setr(rd, fmt.Sprintf("(%s >> %d)", g.R(rt), sa))
		return out
	case KSra:
		setr(rd, fmt.Sprintf("lean_sra(%s, %d)", g.R(rt), sa))
		return out
	case KRotr:
		setr(rd, fmt.Sprintf("lean_rotr(%s, %d)", g.R(rt), sa))
		return out
	case KSllv:
		setr(rd, fmt.Sprintf("(%s << (%s & 31u))", g.R(rt), g.R(rs)))
		return out
	case KSrlv:
		setr(rd, fmt.Sprintf("(%s >> (%s & 31u))", g.R(rt), g.R(rs)))
		return out
	case KSrav:
		setr(rd, fmt.Sprintf("lean_sra(%s, %s)", g.R(rt), g.R(rs)))
		return out
	case KRotrv:
		setr(rd, fmt.Sprintf("lean_rotr(%s, %s)", g.R(rt), g.R(rs)))
		return out
	case KClz:
		setr(rd, fmt.Sprintf("lean_clz(%s)", g.R(rs)))
		return out
	case KClo:
		setr(rd, fmt.Sprintf("lean_clo(%s)", g.R(rs)))
		return out
	case KExt:
		setr(rt, fmt.Sprintf("lean_ext(%s, %d, %d)", g.R(rs), sa, rd+1))
		return out
	case KIns:
		setr(rt, fmt.Sprintf("lean_ins(%s, %s, %d, %d)", g.R(rt), g.R(rs), sa, rd))
		return out
	case KSeb:
		setr(rd, fmt.Sprintf("lean_seb(%s)", g.R(rt)))
		return out
	case KSeh:
		setr(rd, fmt.Sprintf("lean_seh(%s)", g.R(rt)))
		return out
	case KBitrev:
		setr(rd, fmt.Sprintf("lean_bitrev(%s)", g.R(rt)))
		return out
	case KWsbh:
		setr(rd, fmt.Sprintf("lean_wsbh(%s)", g.R(rt)))
		return out
	case KWsbw:
		setr(rd, fmt.Sprintf("lean_wsbw(%s)", g.R(rt)))
		return out
	case KMult, KMultu, KDiv, KDivu, KMadd, KMaddu, KMsub, KMsubu:
		g.used[HI], g.used[LO], g.written[HI], g.written[LO] = true, true, true, true
		A(fmt.Sprintf("lean_%s(%s, %s, hi, lo);", mulNames[k], g.R(rs), g.R(rt)))
		return out
	case KMfhi:
		setr(rd, g.R(HI))
		return out
	case KMflo:
		setr(rd, g.R(LO))
		return out
	case KMthi:
		A(fmt.Sprintf("%s = %s;", g.Wr(HI), g.R(rs)))
		return out
	case KMtlo:
		A(fmt.Sprintf("%s = %s;", g.Wr(LO), g.R(rs)))
		return out

	// ---- FPU
	case KMfc1:
		setr(rt, fmt.Sprintf("lean_f2u(%s)", g.R(F(rd))))
		return out
	case KMtc1:
		A(fmt.Sprintf("%s = lean_u2f(%s);", g.Wr(F(rd)), g.R(rt)))
		return out
	case KCfc1:
		if rd != 31 {
			A(fmt.Sprintf("pk_fatal(\"unsupported CFC1 control register\", %s);", hexw(pc)))
			return out
		}
		setr(rt, fmt.Sprintf("lean_cfc1(%s)", g.R(CC)))
		return out
	case KCtc1:
		if rd != 31 {
			A(fmt.Sprintf("pk_fatal(\"unsupported CTC1 control register\", %s);", hexw(pc)))
			return out
		}
		A(fmt.Sprintf("%s = lean_ctc1(%s);", g.Wr(CC), g.R(rt)))
		return out
	}
	fd, fs, ft := F(sa), F(rd), F(rt)
	switch k {
	case KAddS:
		A(fmt.Sprintf("%s = %s + %s;", g.Wr(fd), g.R(fs), g.R(ft)))
		return out
	case KSubS:
		A(fmt.Sprintf("%s = %s - %s;", g.Wr(fd), g.R(fs), g.R(ft)))
		return out
	case KMulS:
		A(fmt.Sprintf("%s = lean_mul_s(%s, %s);", g.Wr(fd), g.R(fs), g.R(ft)))
		return out
	case KDivS:
		A(fmt.Sprintf("%s = %s / %s;", g.Wr(fd), g.R(fs), g.R(ft)))
		return out
	case KSqrtS:
		A(fmt.Sprintf("%s = lean_sqrt_s(%s);", g.Wr(fd), g.R(fs)))
		return out
	case KAbsS:
		A(fmt.Sprintf("%s = lean_abs_s(%s);", g.Wr(fd), g.R(fs)))
		return out
	case KMovS:
		A(fmt.Sprintf("%s = %s;", g.Wr(fd), g.R(fs)))
		return out
	case KNegS:
		A(fmt.Sprintf("%s = lean_neg_s(%s);", g.Wr(fd), g.R(fs)))
		return out
	case KRoundWS, KTruncWS, KCeilWS, KFloorWS, KCvtWS:
		A(fmt.Sprintf("%s = lean_u2f(lean_%s(%s));", g.Wr(fd), cvtNames[k], g.R(fs)))
		return out
	case KCvtSW:
		A(fmt.Sprintf("%s = lean_cvt_s_w(lean_f2u(%s));", g.Wr(fd), g.R(fs)))
		return out
	case KFpuCompare:
		A(fmt.Sprintf("%s = lean_fcmp<%du>(%s, %s);", g.Wr(CC), d.W&15, g.R(fs), g.R(ft)))
		return out
	}

	// ---- VFPU and extra
	return g.lowerVfpu(d, out, nid)
}

var vfSimple = map[Kind]string{KVflush: "vflush", KVpfx: "vpfx", KViim: "viim", KVfim: "vfim", KVf2h: "vf2h", KVh2f: "vh2f",
	KVi2x: "vi2x", KVt5650: "vt5650", KVrnd: "vrnd", KVx2i: "vx2i", KVf2i: "vf2i", KVi2f: "vi2f",
	KVmidT: "vmidt", KVfpuMatrixInit: "vmatrix_init", KVmmov: "vmmov", KVmmul: "vmmul", KVmscl: "vmscl",
	KVtfm: "vtfm", KVidt: "vidt", KVfpuVectorInit: "vvector_init", KVcst: "vcst", KVocp: "vocp",
	KVfpuHorizontal: "vhorizontal", KVrot: "vrot", KVfpuVec3: "vvec3", KVscl: "vscl", KVdot: "vdot",
	KVhdp: "vhdp", KVcrs: "vcrs", KVcrossQuat: "vcross_quat", KVminmax: "vminmax",
	KVfpuCompare3: "vcompare3", KVcmp: "vcmp", KVcmov: "vcmov", KVfpuUnary: "vunary", KVsgn: "vsgn"}

func (g *FuncGen) lowerVfpu(d *Insn, out []string, nid int) []string {
	A := func(s string) { out = append(out, s) }
	if g.vst != nil && !g.E.NoVfpuFast {
		st, hasSt := g.vst[nid]
		if fl, ok := g.fastLines(d, st, hasSt); ok {
			g.E.stats.add("vfpu_fast")
			g.vfFast++
			name := fastName(d)
			if name == "" {
				name = d.Kind.String()
			}
			g.E.vfpuForms.add(name)
			out = append(out, fl...)
			return out
		}
	}
	if d.Vfpu {
		g.vfGen++
		g.E.stats.add("vfpu_generic")
		name := d.Kind.String()
		if d.Kind == KExtra {
			name = d.Mn
		}
		g.E.vfpuGeneric.add(name)
	}
	k := d.Kind
	w := hexw(d.W)
	pc := d.PC
	rt := int(d.Rt)
	const vf = "lean_vf::"
	if nm, ok := vfSimple[k]; ok {
		A(fmt.Sprintf("%s%s(%s);", vf, nm, w))
		return out
	}
	switch k {
	case KVbfy1:
		A(fmt.Sprintf("%svbfy1(%s, %s);", vf, w, hexw(pc)))
		return out
	case KVsocp:
		A(fmt.Sprintf("%svsocp(%s, %s);", vf, w, hexw(pc)))
		return out
	case KMtv:
		A(fmt.Sprintf("%smtv(%s, %s);", vf, w, g.R(rt)))
		return out
	case KMfv:
		if rt != 0 {
			A(fmt.Sprintf("%s = %smfv(%s);", g.Wr(rt), vf, w))
		}
		return out
	case KLvs, KSvs, KLvq, KSvq:
		off := s16(d.W & 0xFFFC)
		a := g.addr(int(d.Rs), uint32(off))
		nm := map[Kind]string{KLvs: "lvs", KSvs: "svs", KLvq: "lvq", KSvq: "svq"}[k]
		A(fmt.Sprintf("%s%s(%s, %s);", vf, nm, w, a))
		return out
	case KExtra:
		mn := d.Mn
		switch mn {
		case "lvl.q", "lvr.q", "svl.q", "svr.q":
			off := s16(d.W & 0xFFFC)
			a := g.addr(int(d.Rs), uint32(off))
			idx := map[string]int{"lvl.q": 0, "lvr.q": 1, "svl.q": 2, "svr.q": 3}[mn]
			A(fmt.Sprintf("%squad_unaligned(%s, %s, %d);", vf, w, a, idx))
			return out
		case "ll":
			if d.Rt != 0 {
				A(fmt.Sprintf("%s = lean_lw(%s);", g.Wr(int(d.Rt)), g.addr(int(d.Rs), uint32(d.Simm))))
			}
			return out
		case "sc":
			A(fmt.Sprintf("lean_sw(%s, %s);", g.addr(int(d.Rs), uint32(d.Simm)), g.R(int(d.Rt))))
			if d.Rt != 0 {
				A(fmt.Sprintf("%s = 1u;", g.Wr(int(d.Rt))))
			}
			return out
		}
		fn := map[string]string{"vsbn": "vsbn", "vdet": "vdet", "vwbn": "vwbn", "vrnds": "vrnds", "vsbz": "vsbz", "vlgb": "vlgb",
			"vmfvc": "vmfvc", "vmtvc": "vmtvc"}[mn]
		if fn != "" {
			A(fmt.Sprintf("%s%s(%s);", vf, fn, w))
			return out
		}
		switch mn {
		case "vi2c":
			A(fmt.Sprintf("%svi2c_vi2us(%s, true);", vf, w))
			return out
		case "vi2us":
			A(fmt.Sprintf("%svi2c_vi2us(%s, false);", vf, w))
			return out
		case "vsrt1", "vsrt2", "vsrt3", "vsrt4":
			A(fmt.Sprintf("%svsort(%s, %d);", vf, w, int(mn[len(mn)-1]-'0')-1))
			return out
		case "vbfy2":
			A(fmt.Sprintf("%svsort(%s, 4);", vf, w))
			return out
		case "vt4444":
			A(fmt.Sprintf("%svt4444_5551(%s, true);", vf, w))
			return out
		case "vt5551":
			A(fmt.Sprintf("%svt4444_5551(%s, false);", vf, w))
			return out
		}
	}
	A(fmt.Sprintf("pk_fatal(\"unsupported instruction %s\", %s);", d.Mn, hexw(pc)))
	return out
}

// ----------------------------------------------------------------------- branch conditions

func (g *FuncGen) condExpr(d *Insn) (string, error) {
	m := d.Mn
	if d.Likely {
		m = m[:len(m)-1]
	}
	rs, rt := int(d.Rs), int(d.Rt)
	switch m {
	case "beq":
		return fmt.Sprintf("(%s == %s)", g.R(rs), g.R(rt)), nil
	case "bne":
		return fmt.Sprintf("(%s != %s)", g.R(rs), g.R(rt)), nil
	case "blez":
		return fmt.Sprintf("((int32_t)%s <= 0)", g.R(rs)), nil
	case "bgtz":
		return fmt.Sprintf("((int32_t)%s > 0)", g.R(rs)), nil
	case "bltz":
		return fmt.Sprintf("((int32_t)%s < 0)", g.R(rs)), nil
	case "bgez":
		return fmt.Sprintf("((int32_t)%s >= 0)", g.R(rs)), nil
	case "bc1f":
		return fmt.Sprintf("(%s == 0u)", g.R(CC)), nil
	case "bc1t":
		return fmt.Sprintf("(%s != 0u)", g.R(CC)), nil
	case "bvf":
		return fmt.Sprintf("(lean_vf::cc_bit(%s) == 0u)", hexw(d.W)), nil
	case "bvt":
		return fmt.Sprintf("(lean_vf::cc_bit(%s) != 0u)", hexw(d.W)), nil
	}
	return "", &unsupportedError{"branch " + d.Mn}
}

// ----------------------------------------------------------------------- calls

func (g *FuncGen) marshalToCPU(regs []int) []string {
	out := make([]string, 0, len(regs))
	for _, r := range regs {
		out = append(out, fmt.Sprintf("%s = %s;", cpuRef(r), g.R(r)))
	}
	return out
}

func inList(r int, l []int) bool {
	for _, x := range l {
		if x == r {
			return true
		}
	}
	return false
}

// callLines ports call_lines(): statements for a call-completion ('cont') or tail node.
func (g *FuncGen) callLines(nd *Node, tail bool) []string {
	var lines []string
	d := nd.Insn
	if nd.Kind == NCont && d.Mn == "jalr" {
		var mdv Mask
		if g.mdOK {
			mdv = g.md[nd.ID]
		}
		ms := mdv.Or(g.s.A).Or(g.s.Aw).And(argsNoSP)
		// v0 / v1 / f0 that are live after the call and defined here are marshalled too: an indirect callee (its thunk stores only
		// the registers it returns) that leaves one untouched hands the caller's value straight back
		var after Bits
		for _, sx := range nd.Succ {
			after = after.or(g.liveIn[sx])
		}
		ms = ms.Or(mdv.And(after.reg()).And(outEnc))
		lines = append(lines, g.marshalToCPU(ms.Regs())...)
		lines = append(lines, fmt.Sprintf("g_cpu.gpr[29] = %s;", g.R(SP)))
		lines = append(lines, "pk_guest_call(jt);")
		g.needsJt = true
		for _, r := range []int{V0, V1, F0} {
			lines = append(lines, fmt.Sprintf("%s = %s;", g.Wr(r), cpuRef(r)))
		}
		return lines
	}
	t := uint32(nd.Tgt)
	if _, imp := g.P.Imports[t]; imp {
		regs := g.L.ar[t].Regs()
		lines = append(lines, g.marshalToCPU(regs)...)
		lines = append(lines, fmt.Sprintf("g_cpu.gpr[29] = %s;", g.R(SP)))
		lines = append(lines, fmt.Sprintf("pk_hle_call(%s);", hexw(t)))
		for _, r := range []int{V0, V1, F0} {
			lines = append(lines, fmt.Sprintf("%s = %s;", g.Wr(r), cpuRef(r)))
		}
		g.E.stats.add("import_calls")
		return lines
	}
	ts := g.P.S[t]
	if ts == nil {
		lines = append(lines, fmt.Sprintf("g_cpu.gpr[29] = %s;", g.R(SP)))
		lines = append(lines, g.marshalToCPU(argsNoSP.Regs())...)
		lines = append(lines, fmt.Sprintf("pk_guest_call(%s);", hexw(t)))
		for _, r := range []int{V0, V1, F0} {
			lines = append(lines, fmt.Sprintf("%s = %s;", g.Wr(r), cpuRef(r)))
		}
		g.E.Fallbacks = append(g.E.Fallbacks, Fallback{g.e, t, "direct call target is not a known function"})
		return lines
	}
	if ts.Typed {
		ps := params(ts)
		as := make([]string, len(ps))
		for i, r := range ps {
			as[i] = g.R(r)
		}
		callee := fmt.Sprintf("f_%08X(%s)", t, strings.Join(as, ", "))
		rr := g.L.rset(ts)
		for _, r := range rr {
			g.Wr(r)
		}
		lines = append(lines, unpackReturn(rr, callee)...)
		return lines
	}
	// regfile callee: the registers a caller must store before the call (A | Aw and sp)
	regs := paramsOf(ts.A.Or(ts.Aw))
	if !inList(SP, regs) {
		regs = append([]int{SP}, regs...)
	}
	// a regfile callee hands back the register file in place: an output it leaves untouched on some path (a callee-saved register
	// an inner ABI violator writes, v0 it never defines) keeps whatever the caller stored, so every register the callee returns
	// that the caller still reads (or, at a tail call, passes on) is stored before the call as well
	rpts := g.L.Rp(ts)
	var afterReg Mask
	if tail {
		afterReg = rpts
	} else {
		var after Bits
		for _, sx := range nd.Succ {
			after = after.or(g.liveIn[sx])
		}
		afterReg = after.reg()
	}
	for _, r := range rpts.And(afterReg).Regs() {
		if !inList(r, regs) && r != 0 && r != 28 && r != CC && r != SP && r != RA && r <= 66 {
			regs = append(regs, r)
		}
	}
	lines = append(lines, g.marshalToCPU(regs)...)
	lines = append(lines, fmt.Sprintf("t_%08X();", t))
	for _, r := range rpts.Regs() {
		if r == SP || r == RA || r > 66 {
			continue
		}
		lines = append(lines, fmt.Sprintf("%s = %s;", g.Wr(r), cpuRef(r)))
	}
	return lines
}

func (g *FuncGen) returnLines() []string {
	if g.typed {
		regs := g.rsRet
		if len(regs) == 0 {
			return []string{"return;"}
		}
		for _, r := range regs {
			g.R(r)
		}
		return []string{fmt.Sprintf("return %s;", packReturn(regs))}
	}
	return []string{"goto L_ret;"}
}

// ----------------------------------------------------------------------- switch

func (g *FuncGen) nodeForPC(pc uint32) (int, error) {
	if g.keyIndex == nil {
		g.keyIndex = make(map[Key]*Node, len(g.nodes))
		for _, n := range g.nodes {
			g.keyIndex[n.Key] = n
		}
	}
	n := g.keyIndex[Key{0, pc}]
	if n == nil {
		n = g.keyIndex[Key{'T', pc}]
	}
	if n == nil {
		return 0, fmt.Errorf("no node for %08x in %08x", pc, g.e)
	}
	return n.ID, nil
}

func (g *FuncGen) switchLines(nd *Node) ([]string, error) {
	data := nd.Table()
	ents := data.Entries
	idx := data.IdxReg
	ok := data.Idx
	shifted := ok && data.Shifted
	var lines []string
	if ok {
		type grp struct {
			e    uint32
			idxs []int
		}
		var groups []*grp
		byE := map[uint32]*grp{}
		for i, e := range ents {
			gp := byE[e]
			if gp == nil {
				gp = &grp{e: e}
				byE[e] = gp
				groups = append(groups, gp)
			}
			gp.idxs = append(gp.idxs, i)
		}
		sw := g.R(idx)
		if shifted {
			sw += " >> 2"
		}
		lines = append(lines, fmt.Sprintf("switch (%s) {", sw))
		for _, gp := range groups {
			tgt, err := g.nodeForPC(gp.e)
			if err != nil {
				return nil, err
			}
			g.targets[tgt] = struct{}{}
			cs := make([]string, len(gp.idxs))
			for i, x := range gp.idxs {
				cs[i] = fmt.Sprintf("case %du:", x)
			}
			lines = append(lines, fmt.Sprintf("  %s goto L%d;", strings.Join(cs, " "), tgt))
		}
		lines = append(lines, fmt.Sprintf("  default: pk_fatal(\"jump table index out of range\", %s);", hexw(nd.PC)))
		lines = append(lines, "}")
	} else {
		jr := nd.Insn
		seen := map[uint32]struct{}{}
		var uniq []uint32
		for _, e := range ents {
			if _, ok := seen[e]; !ok {
				seen[e] = struct{}{}
				uniq = append(uniq, e)
			}
		}
		lines = append(lines, fmt.Sprintf("switch (%s) {", g.R(int(jr.Rs))))
		for _, e := range uniq {
			tgt, err := g.nodeForPC(e)
			if err != nil {
				return nil, err
			}
			g.targets[tgt] = struct{}{}
			lines = append(lines, fmt.Sprintf("  case %s: goto L%d;", hexw(e), tgt))
		}
		lines = append(lines, fmt.Sprintf("  default: pk_fatal(\"indirect jump to unknown target\", %s);", hexw(nd.PC)))
		lines = append(lines, "}")
		g.E.stats.add("value_switch")
	}
	return lines, nil
}

// ----------------------------------------------------------------------- layout

func (g *FuncGen) preferredNext(n *Node) int {
	sc := n.Succ
	if len(sc) == 0 {
		return -1
	}
	if n.Kind == NSlot && len(sc) == 2 {
		return sc[1]
	}
	if n.Kind == NBr && len(sc) == 2 {
		return sc[1]
	}
	return sc[0]
}

func (g *FuncGen) orderNodes() []int {
	nodes := g.nodes
	start := g.f.EntryID
	seen := make([]bool, len(nodes))
	order := make([]int, 0, len(nodes))
	pending := []int{start}
	for len(pending) > 0 {
		cur := pending[len(pending)-1]
		pending = pending[:len(pending)-1]
		for cur >= 0 && !seen[cur] {
			seen[cur] = true
			order = append(order, cur)
			n := nodes[cur]
			nxt := g.preferredNext(n)
			for i := len(n.Succ) - 1; i >= 0; i-- {
				sx := n.Succ[i]
				if sx != nxt && !seen[sx] {
					pending = append(pending, sx)
				}
			}
			if nxt >= 0 && !seen[nxt] {
				cur = nxt
			} else {
				cur = -1
			}
		}
	}
	return order
}

// backEdgeTargets: loop headers: targets of DFS back edges.
func (g *FuncGen) backEdgeTargets() map[int]struct{} {
	nodes := g.nodes
	start := g.f.EntryID
	color := make([]uint8, len(nodes))
	heads := map[int]struct{}{}
	type fr struct{ i, j int }
	stack := []fr{{start, 0}}
	color[start] = 1
	for len(stack) > 0 {
		top := &stack[len(stack)-1]
		su := nodes[top.i].Succ
		if top.j < len(su) {
			s2 := su[top.j]
			top.j++
			switch color[s2] {
			case 0:
				color[s2] = 1
				stack = append(stack, fr{s2, 0})
			case 1:
				heads[s2] = struct{}{}
			}
		} else {
			color[top.i] = 2
			stack = stack[:len(stack)-1]
		}
	}
	return heads
}

// ----------------------------------------------------------------------- the function

func (g *FuncGen) orLive(n *Node) Bits {
	var o Bits
	for _, sx := range n.Succ {
		o = o.or(g.liveIn[sx])
	}
	return o
}

// Gen ports gen().
func (g *FuncGen) Gen() ([]string, error) {
	nodes := g.nodes
	g.collectSlots()
	order := g.orderNodes()
	_, yieldFn := g.E.Yield[g.e]
	g.heads = map[int]struct{}{}
	if yieldFn {
		g.heads = g.backEdgeTargets()
	}
	g.body = g.body[:0]

	goto_ := func(srcIdx int, tgt int) []string {
		nxt := -1
		if srcIdx+1 < len(order) {
			nxt = order[srcIdx+1]
		}
		if tgt == nxt {
			return nil
		}
		g.targets[tgt] = struct{}{}
		return []string{fmt.Sprintf("goto L%d;", tgt)}
	}

	for i, nid := range order {
		n := nodes[nid]
		k := n.Kind
		var L_ []string
		// alias state: inherited from the single predecessor when it was emitted earlier, empty at joins / loop headers
		g.al = 0
		if len(n.Pred) == 1 && g.aliasSet[n.Pred[0]] {
			g.al = g.aliasOut[n.Pred[0]]
		}
		switch k {
		case NN, NSlot:
			if !g.isDead(nid) {
				L_ = append(L_, g.lower(nid, n)...)
			}
			if k == NSlot {
				var pr *Node
				if len(n.Pred) > 0 {
					pr = nodes[n.Pred[0]]
				}
				if pr != nil && pr.Kind == NBr && !pr.Insn.Likely && len(n.Succ) == 2 {
					cv := fmt.Sprintf("c_%x", pr.PC)
					g.condVars[cv] = struct{}{}
					t_, f_ := n.Succ[0], n.Succ[1]
					g.targets[t_] = struct{}{}
					L_ = append(L_, fmt.Sprintf("if (%s) goto L%d;", cv, t_))
					L_ = append(L_, goto_(i, f_)...)
					g.body = append(g.body, bodyEnt{nid, L_})
					g.aliasOut[nid], g.aliasSet[nid] = g.al, true
					continue
				}
			}
			switch len(n.Succ) {
			case 1:
				L_ = append(L_, goto_(i, n.Succ[0])...)
			case 0:
				L_ = append(L_, fmt.Sprintf("pk_fatal(\"fell off the function\", %s);", hexw(n.PC)))
			default:
				return nil, fmt.Errorf("slot/n with %d succ at %08x", len(n.Succ), n.PC)
			}
		case NBr:
			d := n.Insn
			if d.Mn == "bltzal" || d.Mn == "bgezal" || d.Mn == "bltzall" || d.Mn == "bgezall" {
				return nil, &unsupportedError{fmt.Sprintf("branch and link at %08x", n.PC)}
			}
			if d.Likely {
				if len(n.Succ) == 2 {
					sl, fall := n.Succ[0], n.Succ[1]
					g.targets[sl] = struct{}{}
					ce, err := g.condExpr(d)
					if err != nil {
						return nil, err
					}
					L_ = append(L_, fmt.Sprintf("if (%s) goto L%d;", ce, sl))
					L_ = append(L_, goto_(i, fall)...)
				} else {
					L_ = append(L_, goto_(i, n.Succ[0])...)
				}
			} else {
				if n.Const == TriNone {
					cv := fmt.Sprintf("c_%x", n.PC)
					g.condVars[cv] = struct{}{}
					ce, err := g.condExpr(d)
					if err != nil {
						return nil, err
					}
					L_ = append(L_, fmt.Sprintf("%s = %s;", cv, ce))
				}
				L_ = append(L_, goto_(i, n.Succ[0])...)
			}
		case NJmp, NJr:
			L_ = append(L_, goto_(i, n.Succ[0])...)
		case NCall:
			d := n.Insn
			ol := g.orLive(n)
			if d.Mn == "jalr" {
				L_ = append(L_, fmt.Sprintf("jt = %s;", g.R(int(d.Rs))))
				g.needsJt = true
				if d.Rd != 0 && ol.bit(int(d.Rd)) {
					L_ = append(L_, fmt.Sprintf("%s = %s;", g.Wr(int(d.Rd)), u(d.PC+8)))
				}
			} else if ol.bit(RA) {
				L_ = append(L_, fmt.Sprintf("%s = %s;", g.Wr(RA), u(d.PC+8)))
			}
			L_ = append(L_, goto_(i, n.Succ[0])...)
		case NCont:
			if n.Noret {
				L_ = append(L_, g.callLines(n, false)...)
				L_ = append(L_, fmt.Sprintf("pk_fatal(\"noreturn function returned\", %s);", hexw(n.PC)))
			} else {
				L_ = append(L_, g.callLines(n, false)...)
				if g.hasVfpu {
					L_ = append(L_, fmt.Sprintf("LEAN_VFPU_ENTRY_CHECK(%s);", hexw(n.PC)))
				}
				if len(n.Succ) == 1 {
					L_ = append(L_, goto_(i, n.Succ[0])...)
				} else {
					L_ = append(L_, fmt.Sprintf("pk_fatal(\"call continuation missing\", %s);", hexw(n.PC)))
				}
			}
		case NTail:
			L_ = append(L_, g.callLines(n, true)...)
			L_ = append(L_, g.returnLines()...)
		case NRet:
			if !g.typed {
				L_ = append(L_, fmt.Sprintf("if (%s != ra0) pk_fatal(\"return through a foreign return address\", %s);", g.R(RA), hexw(n.PC)))
				L_ = append(L_, fmt.Sprintf("if (%s != sp0) pk_fatal(\"stack pointer not balanced at return\", %s);", g.R(SP), hexw(n.PC)))
			}
			L_ = append(L_, g.returnLines()...)
		case NSwitch:
			sl, err := g.switchLines(n)
			if err != nil {
				return nil, err
			}
			L_ = append(L_, sl...)
		case NIjmp:
			L_ = append(L_, fmt.Sprintf("pk_fatal(\"unresolved indirect jump\", %s);", hexw(n.PC)))
		case NFatal:
			what := n.Str()
			if what == "" {
				what = "fatal"
			}
			L_ = append(L_, fmt.Sprintf("pk_fatal(\"%s\", %s);", what, hexw(n.PC)))
		default:
			return nil, fmt.Errorf("unknown node kind %d", k)
		}
		g.body = append(g.body, bodyEnt{nid, L_})
		g.aliasOut[nid], g.aliasSet[nid] = g.al, true
	}
	return g.assemble(), nil
}

// ----------------------------------------------------------------------- assembly

func (g *FuncGen) assemble() []string {
	e := g.e
	s := g.s
	var out []string
	regfile := !g.typed
	if g.typed {
		out = append(out, g.L.signature(s)+" {")
	} else {
		out = append(out, fmt.Sprintf("void t_%08X() {", e))
	}
	var pre []string
	if regfile {
		// load every register read before written (full live-in), plus sp / ra for the exit checks
		li := g.liveIn[g.f.EntryID].reg().And(resMask)
		var load []int
		for _, r := range li.Regs() {
			load = append(load, r)
		}
		for _, r := range []int{SP, RA} {
			if !inList(r, load) {
				load = append(load, r)
			}
		}
		var kept []int
		for _, r := range load {
			if r != 0 && r != 28 && r != CC {
				kept = append(kept, r)
			}
		}
		sort.Ints(kept)
		for _, r := range kept {
			g.used[r] = true
		}
		g.loadRegs = kept
	}
	if g.usesHsp {
		g.used[SP] = true
	}
	if regfile {
		g.used[SP], g.used[RA] = true, true
	}
	// declarations
	var declI, declF []string
	var pset [NRES]bool
	for _, r := range g.ps {
		pset[r] = true
	}
	for r := 0; r < NRES; r++ {
		if !g.used[r] || pset[r] || r == 0 {
			continue
		}
		if r == CC || r == HI || r == LO || r < 32 {
			declI = append(declI, regName(r))
		} else {
			declF = append(declF, regName(r))
		}
	}
	if len(declI) > 0 {
		parts := make([]string, len(declI))
		for i, x := range declI {
			parts[i] = x + " = 0"
		}
		pre = append(pre, "uint32_t "+strings.Join(parts, ", ")+";")
	}
	if len(declF) > 0 {
		parts := make([]string, len(declF))
		for i, x := range declF {
			parts[i] = x + " = 0.0f"
		}
		pre = append(pre, "float "+strings.Join(parts, ", ")+";")
	}
	sidx := make([]int, 0, len(g.slotTypes))
	for i := range g.slotTypes {
		sidx = append(sidx, i)
	}
	sort.Ints(sidx)
	for _, i := range sidx {
		ty := g.slotTypes[i]
		init := "0"
		if ty == "float" {
			init = "0.0f"
		}
		pre = append(pre, fmt.Sprintf("%s %s = %s;", ty, slotRef(i), init))
	}
	if len(g.condVars) > 0 {
		cvs := make([]string, 0, len(g.condVars))
		for c := range g.condVars {
			cvs = append(cvs, c)
		}
		sort.Strings(cvs)
		for i, c := range cvs {
			cvs[i] = c + " = 0"
		}
		pre = append(pre, "uint32_t "+strings.Join(cvs, ", ")+";")
	}
	if g.needsJt {
		pre = append(pre, "uint32_t jt = 0;")
	}
	if regfile {
		for _, r := range g.loadRegs {
			pre = append(pre, fmt.Sprintf("%s = %s;", regName(r), cpuRef(r)))
		}
		pre = append(pre, "const uint32_t sp0 = sp, ra0 = r31;")
	}
	if g.aliasReg != 0 {
		var parts []string
		for r := 0; r < 32; r++ {
			if g.aliasReg>>uint(r)&1 != 0 {
				parts = append(parts, fmt.Sprintf("*b%d = nullptr", r))
			}
		}
		pre = append(pre, "uint8_t "+strings.Join(parts, ", ")+";")
	}
	if g.usesHsp {
		// the frame base is derived from the entry sp itself (not from a second pointer) so the compiler cannot fold the
		// displacements back into negative offsets from the entry sp
		if g.frameLo != 0 {
			pre = append(pre, fmt.Sprintf("uint8_t* const hb = g_ram + ((sp - %du) & LEAN_MASK);", -g.frameLo))
		} else {
			pre = append(pre, "uint8_t* const hb = g_ram + (sp & LEAN_MASK);")
		}
	}
	for _, x := range pre {
		if x != "" {
			out = append(out, "  "+x)
		}
	}
	var have [NRES]bool
	if g.typed {
		have = pset
	} else {
		for _, r := range g.loadRegs {
			have[r] = true
		}
	}
	tmask := 0
	targs := make([]string, 4)
	for i, r := range []int{4, 5, 6, 7} {
		if have[r] {
			tmask |= 1 << uint(i)
			targs[i] = regName(r)
		} else {
			targs[i] = "0u"
		}
	}
	out = append(out, fmt.Sprintf("  LEAN_TRC(0x%08XU, %d, %s);", e, tmask, strings.Join(targs, ", ")))
	for _, x := range g.E.hookPrologue(g) {
		out = append(out, "  "+x)
	}
	if g.hasVfpu {
		out = append(out, fmt.Sprintf("  LEAN_VFPU_ENTRY_CHECK(0x%08XU);", e))
	}
	for _, b := range g.body {
		if _, ok := g.targets[b.nid]; ok {
			out = append(out, fmt.Sprintf("L%d:;", b.nid))
			if _, h := g.heads[b.nid]; h {
				out = append(out, "  PK_YIELD();")
			}
		}
		for _, ln := range b.lines {
			if ln != "" {
				out = append(out, "  "+ln)
			}
		}
	}
	if regfile {
		out = append(out, "L_ret:;")
		var wr []int
		for r := 0; r < NRES; r++ {
			if g.written[r] && r != 0 && r != 28 && r != CC && r != SP && r != RA {
				wr = append(wr, r)
			}
		}
		for _, r := range wr {
			out = append(out, fmt.Sprintf("  %s = %s;", cpuRef(r), regName(r)))
		}
		g.storeRegs = wr
	}
	out = append(out, "}")
	return out
}
