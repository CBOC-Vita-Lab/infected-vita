// mips.go mirrors recomp/leangen/mips.py: the Allegrex (MIPS32 + FPU + VFPU) decoder of the lean generator.
//
// The classification mirrors PortableKit's src/decoder.cpp (decode_allegrex) and src/extra_instructions.cpp (classify) one to one;
// Kind is the OpcodeKind name there, Mn the mnemonic.
//
// Resources (registers) used by the data-flow analysis:
//
//	0..31   GPR           32..63  FPR (f0..f31)      64 CC (FPU condition)      65 HI     66 LO
//
// VFPU state (g_cpu.vfpu / g_cpu.vfpu_ctrl) is memory-resident and is not a data-flow resource.
package leangen

const (
	HI = 65
	LO = 66
	CC = 64
)

// F is the resource number of floating point register n.
func F(n int) int { return 32 + n }

func s16(x uint32) int32 {
	x &= 0xFFFF
	if x&0x8000 != 0 {
		return int32(x) - 0x10000
	}
	return int32(x)
}

// register roles (o32-like PSP ABI as used by the Allegrex gcc)
const (
	RA = 31
	SP = 29
	GP = 28
	FP = 30
)

// RegSet is a tiny set of resources (an instruction reads or writes at most four).
type RegSet struct {
	n uint8
	r [4]uint8
}

func mkSet(rs ...int) RegSet {
	var s RegSet
	for _, r := range rs {
		if r == 0 || s.Has(r) {
			continue
		}
		s.r[s.n] = uint8(r)
		s.n++
	}
	return s
}

// Has reports whether r is in the set.
func (s RegSet) Has(r int) bool {
	for i := uint8(0); i < s.n; i++ {
		if int(s.r[i]) == r {
			return true
		}
	}
	return false
}

// Len is the number of members.
func (s RegSet) Len() int { return int(s.n) }

// At returns the i-th member (in insertion order).
func (s RegSet) At(i int) int { return int(s.r[i]) }

// Insn is a decoded instruction.
type Insn struct {
	PC, W  uint32
	Imm    uint32
	Tgt    uint32
	Simm   int32
	Uses   RegSet
	Defs   RegSet
	Mn     string
	Kind   Kind
	Op     uint8
	Rs     uint8
	Rt     uint8
	Rd     uint8
	Sa     uint8
	Mem    uint8 // memNone, memLd, memSt
	Fuse   uint8 // 1/2: first/second half of a fused lwl/lwr (swl/swr) pair, see fuse.go
	Delay  bool
	Likely bool
	Ctl    bool
	Vfpu   bool
}

const (
	memNone = 0
	memLd   = 1
	memSt   = 2
)

var brCond = map[string]bool{"beq": true, "bne": true, "blez": true, "bgtz": true, "bltz": true, "bgez": true, "bc1f": true, "bc1t": true,
	"bvf": true, "bvt": true, "bltzal": true, "bgezal": true}
var brLikely = map[string]bool{"beql": true, "bnel": true, "blezl": true, "bgtzl": true, "bltzl": true, "bgezl": true, "bc1fl": true,
	"bc1tl": true, "bvfl": true, "bvtl": true, "bltzall": true, "bgezall": true}

func isBranch(mn string) bool { return brCond[mn] || brLikely[mn] }
func isCtrl(mn string) bool {
	return isBranch(mn) || mn == "j" || mn == "jal" || mn == "jr" || mn == "jalr"
}

// extraName ports extra_name(): the instructions PortableKit's extra_instructions.cpp classifies ("" for none).
func extraName(w uint32) string {
	op := w >> 26
	switch op {
	case 0x30:
		return "ll"
	case 0x38:
		return "sc"
	case 0x35:
		if w&2 == 0 {
			return "lvl.q"
		}
		return "lvr.q"
	case 0x3D:
		if w&2 == 0 {
			return "svl.q"
		}
		return "svr.q"
	case 0x18:
		if (w>>23)&7 == 2 {
			return "vsbn"
		}
		return ""
	case 0x19:
		if (w>>23)&7 == 6 {
			return "vdet"
		}
		return ""
	case 0x34:
		group := (w >> 21) & 31
		operation := (w >> 16) & 31
		if group >= 24 {
			return "vwbn"
		}
		if group == 1 {
			switch operation {
			case 0:
				return "vrnds"
			case 22:
				return "vsbz"
			case 23:
				return "vlgb"
			case 29:
				return "vi2c"
			case 30:
				return "vi2us"
			}
		}
		if group == 2 {
			switch operation {
			case 0:
				return "vsrt1"
			case 1:
				return "vsrt2"
			case 3:
				return "vbfy2"
			case 8:
				return "vsrt3"
			case 9:
				return "vsrt4"
			case 16:
				return "vmfvc"
			case 17:
				return "vmtvc"
			case 25:
				return "vt4444"
			case 26:
				return "vt5551"
			}
		}
	}
	return ""
}

func vfpuLength(w uint32) uint32 { return (((w >> 7) & 1) | (((w >> 15) & 1) << 1)) + 1 }

type km struct {
	k  Kind
	mn string
}

var specialTable = map[uint32]km{
	0x00: {KSll, "sll"}, 0x03: {KSra, "sra"}, 0x04: {KSllv, "sllv"}, 0x07: {KSrav, "srav"}, 0x08: {KJr, "jr"},
	0x09: {KJalr, "jalr"}, 0x0A: {KMovz, "movz"}, 0x0B: {KMovn, "movn"}, 0x0C: {KSyscall, "syscall"},
	0x0D: {KBreak, "break"}, 0x0F: {KSync, "sync"}, 0x10: {KMfhi, "mfhi"}, 0x11: {KMthi, "mthi"},
	0x12: {KMflo, "mflo"}, 0x13: {KMtlo, "mtlo"}, 0x16: {KClz, "clz"}, 0x17: {KClo, "clo"}, 0x18: {KMult, "mult"},
	0x19: {KMultu, "multu"}, 0x1A: {KDiv, "div"}, 0x1B: {KDivu, "divu"}, 0x1C: {KMadd, "madd"}, 0x1D: {KMaddu, "maddu"},
	0x20: {KAdd, "add"}, 0x21: {KAddu, "addu"}, 0x22: {KSub, "sub"}, 0x23: {KSubu, "subu"}, 0x24: {KAnd, "and"},
	0x25: {KOr, "or"}, 0x26: {KXor, "xor"}, 0x27: {KNor, "nor"}, 0x2A: {KSlt, "slt"}, 0x2B: {KSltu, "sltu"},
	0x2C: {KMax, "max"}, 0x2D: {KMin, "min"}, 0x2E: {KMsub, "msub"}, 0x2F: {KMsubu, "msubu"},
}

var regimmTable = map[uint32]km{0: {KBltz, "bltz"}, 1: {KBgez, "bgez"}, 2: {KBltzl, "bltzl"}, 3: {KBgezl, "bgezl"},
	0x10: {KBltzal, "bltzal"}, 0x11: {KBgezal, "bgezal"}, 0x12: {KBltzall, "bltzall"}, 0x13: {KBgezall, "bgezall"}}

var simpleTable = map[uint32]km{
	0x02: {KJ, "j"}, 0x03: {KJal, "jal"}, 0x04: {KBeq, "beq"}, 0x05: {KBne, "bne"}, 0x06: {KBlez, "blez"}, 0x07: {KBgtz, "bgtz"},
	0x14: {KBeql, "beql"}, 0x15: {KBnel, "bnel"}, 0x16: {KBlezl, "blezl"}, 0x17: {KBgtzl, "bgtzl"},
	0x08: {KAddiu, "addi"}, 0x09: {KAddiu, "addiu"}, 0x0A: {KSlti, "slti"}, 0x0B: {KSltiu, "sltiu"}, 0x0C: {KAndi, "andi"},
	0x0D: {KOri, "ori"}, 0x0E: {KXori, "xori"}, 0x0F: {KLui, "lui"},
	0x20: {KLb, "lb"}, 0x21: {KLh, "lh"}, 0x22: {KLwl, "lwl"}, 0x23: {KLw, "lw"}, 0x24: {KLbu, "lbu"}, 0x25: {KLhu, "lhu"},
	0x26: {KLwr, "lwr"}, 0x28: {KSb, "sb"}, 0x29: {KSh, "sh"}, 0x2A: {KSwl, "swl"}, 0x2B: {KSw, "sw"}, 0x2E: {KSwr, "swr"},
	0x31: {KLwc1, "lwc1"}, 0x32: {KLvs, "lv.s"}, 0x39: {KSwc1, "swc1"}, 0x3A: {KSvs, "sv.s"}, 0x2F: {KCache, "cache"},
	0x36: {KLvq, "lv.q"}, 0x3E: {KSvq, "sv.q"}, 0x3F: {KVflush, "vflush"},
}

var cop1sTable = map[uint32]km{0: {KAddS, "add.s"}, 1: {KSubS, "sub.s"}, 2: {KMulS, "mul.s"}, 3: {KDivS, "div.s"}, 4: {KSqrtS, "sqrt.s"},
	5: {KAbsS, "abs.s"}, 6: {KMovS, "mov.s"}, 7: {KNegS, "neg.s"}, 0x0C: {KRoundWS, "round.w.s"},
	0x0D: {KTruncWS, "trunc.w.s"}, 0x0E: {KCeilWS, "ceil.w.s"}, 0x0F: {KFloorWS, "floor.w.s"}, 0x24: {KCvtWS, "cvt.w.s"}}

var allegrexTable = map[uint32]km{0x02: {KWsbh, "wsbh"}, 0x03: {KWsbw, "wsbw"}, 0x10: {KSeb, "seb"}, 0x14: {KBitrev, "bitrev"}, 0x18: {KSeh, "seh"}}

var unary0Table = map[uint32]km{0: {KVfpuUnary, "vmov"}, 1: {KVfpuUnary, "vabs"}, 2: {KVfpuUnary, "vneg"}, 3: {KVidt, "vidt"}, 4: {KVfpuUnary, "vsat0"},
	5: {KVfpuUnary, "vsat1"}, 6: {KVfpuVectorInit, "vzero"}, 7: {KVfpuVectorInit, "vone"}, 16: {KVfpuUnary, "vrcp"},
	17: {KVfpuUnary, "vrsq"}, 18: {KVfpuUnary, "vsin"}, 19: {KVfpuUnary, "vcos"}, 20: {KVfpuUnary, "vexp2"},
	21: {KVfpuUnary, "vlog2"}, 22: {KVfpuUnary, "vsqrt"}, 23: {KVfpuUnary, "vasin"}, 24: {KVfpuUnary, "vnrcp"},
	26: {KVfpuUnary, "vnsin"}, 28: {KVfpuUnary, "vrexp2"}}

// classify ports classify(): -> (kind, mn) with kind == OpcodeKind name of PortableKit's decoder.
func classify(w uint32) (Kind, string) {
	op := w >> 26
	rs := (w >> 21) & 31
	rt := (w >> 16) & 31
	sa := (w >> 6) & 31
	if w == 0 {
		return KNop, "nop"
	}
	if ex := extraName(w); ex != "" {
		return KExtra, ex
	}
	if op == 0x00 {
		fn := w & 0x3F
		if fn == 0x02 {
			if rs == 1 {
				return KRotr, "rotr"
			}
			return KSrl, "srl"
		}
		if fn == 0x06 {
			if sa == 1 {
				return KRotrv, "rotrv"
			}
			return KSrlv, "srlv"
		}
		if t, ok := specialTable[fn]; ok {
			return t.k, t.mn
		}
		return KUnsupported, "special?"
	}
	if op == 0x01 {
		if t, ok := regimmTable[rt]; ok {
			return t.k, t.mn
		}
		return KUnsupported, "regimm?"
	}
	if t, ok := simpleTable[op]; ok {
		return t.k, t.mn
	}
	if op == 0x11 {
		switch rs {
		case 0:
			return KMfc1, "mfc1"
		case 4:
			return KMtc1, "mtc1"
		case 2:
			return KCfc1, "cfc1"
		case 6:
			return KCtc1, "ctc1"
		case 8:
			return [4]km{{KBc1f, "bc1f"}, {KBc1t, "bc1t"}, {KBc1fl, "bc1fl"}, {KBc1tl, "bc1tl"}}[rt&3].k, [4]string{"bc1f", "bc1t", "bc1fl", "bc1tl"}[rt&3]
		case 0x10:
			fn := w & 0x3F
			if t, ok := cop1sTable[fn]; ok {
				return t.k, t.mn
			}
			if fn&0x30 == 0x30 {
				return KFpuCompare, "c.cond.s"
			}
			return KUnsupported, "cop1.s?"
		}
		if rs == 0x14 && (w&0x3F) == 0x20 {
			return KCvtSW, "cvt.s.w"
		}
		return KUnsupported, "cop1?"
	}
	if op == 0x1F {
		fn := w & 0x3F
		if fn == 0 {
			return KExt, "ext"
		}
		if fn == 4 {
			return KIns, "ins"
		}
		if fn == 0x18 || fn == 0x20 {
			if t, ok := allegrexTable[sa]; ok {
				return t.k, t.mn
			}
			return KUnsupported, "allegrex0?"
		}
		return KUnsupported, "special3?"
	}
	if op == 0x12 {
		if rs == 3 {
			return KMfv, "mfv"
		}
		if rs == 7 {
			return KMtv, "mtv"
		}
		if rs == 8 {
			return [4]Kind{KBvf, KBvt, KBvfl, KBvtl}[rt&3], [4]string{"bvf", "bvt", "bvfl", "bvtl"}[rt&3]
		}
		return KVfpu, "cop2/vfpu"
	}
	if op == 0x18 {
		o := (w >> 23) & 7
		switch o {
		case 0:
			return KVfpuVec3, "vadd"
		case 1:
			return KVfpuVec3, "vsub"
		case 7:
			return KVfpuVec3, "vdiv"
		}
		return KVfpu, "vfpu0"
	}
	if op == 0x19 {
		o := (w >> 23) & 7
		switch {
		case o == 0:
			return KVfpuVec3, "vmul"
		case o == 1:
			return KVdot, "vdot"
		case o == 2:
			return KVscl, "vscl"
		case o == 4:
			return KVhdp, "vhdp"
		case o == 5 && vfpuLength(w) == 3:
			return KVcrs, "vcrs.t"
		}
		return KVfpu, "vfpu1"
	}
	if op == 0x1B {
		o := (w >> 23) & 7
		if o == 0 {
			return KVcmp, "vcmp"
		}
		if o == 2 || o == 3 {
			if o == 2 {
				return KVminmax, "vmin"
			}
			return KVminmax, "vmax"
		}
		if o >= 5 {
			return KVfpuCompare3, map[uint32]string{5: "vscmp", 6: "vsge", 7: "vslt"}[o]
		}
		return KVfpu, "vfpu3"
	}
	if op == 0x35 || op == 0x3D {
		return KVfpu, "vfpu"
	}
	if op == 0x37 {
		t := (w >> 23) & 7
		if t <= 5 {
			if t <= 1 {
				return KVpfx, "vpfxs"
			}
			if t <= 3 {
				return KVpfx, "vpfxt"
			}
			return KVpfx, "vpfxd"
		}
		if t == 6 {
			return KViim, "viim.s"
		}
		return KVfim, "vfim.s"
	}
	if op == 0x34 {
		group := (w >> 21) & 31
		operation := (w >> 16) & 31
		if group == 3 {
			return KVcst, "vcst"
		}
		if group == 1 && 1 <= operation && operation <= 3 {
			return KVrnd, []string{"vrndi", "vrndf1", "vrndf2"}[operation-1]
		}
		if group == 1 && (operation == 28 || operation == 31) {
			if operation == 28 {
				return KVi2x, "vi2uc"
			}
			return KVi2x, "vi2s"
		}
		if group == 2 && operation == 27 {
			return KVt5650, "vt5650"
		}
		if group == 1 && operation == 18 {
			return KVf2h, "vf2h"
		}
		if group == 1 && operation == 19 {
			return KVh2f, "vh2f"
		}
		if group == 1 && 24 <= operation && operation <= 27 {
			return KVx2i, []string{"vuc2i", "vc2i", "vus2i", "vs2i"}[operation-24]
		}
		if 16 <= group && group <= 19 {
			return KVf2i, []string{"vf2in", "vf2iz", "vf2iu", "vf2id"}[group-16]
		}
		if group == 20 {
			return KVi2f, "vi2f"
		}
		if group == 21 {
			if (w>>19)&1 == 0 {
				return KVcmov, "vcmovt"
			}
			return KVcmov, "vcmovf"
		}
		if group == 2 && operation == 4 {
			return KVocp, "vocp"
		}
		if group == 2 && operation == 5 {
			return KVsocp, "vsocp"
		}
		if group == 2 && operation == 2 {
			return KVbfy1, "vbfy1"
		}
		if group == 2 && operation == 10 {
			return KVsgn, "vsgn"
		}
		if group == 2 && (operation == 6 || operation == 7) {
			if operation == 6 {
				return KVfpuHorizontal, "vfad"
			}
			return KVfpuHorizontal, "vavg"
		}
		if group == 0 {
			if t, ok := unary0Table[operation]; ok {
				return t.k, t.mn
			}
			return KVfpu, "vfpu4"
		}
		return KVfpu, "vfpu4"
	}
	if op == 0x3C {
		group := (w >> 21) & 31
		switch {
		case group <= 3:
			return KVmmul, "vmmul"
		case 4 <= group && group <= 15:
			return KVtfm, "vtfm"
		case 16 <= group && group <= 19:
			return KVmscl, "vmscl"
		case 20 <= group && group <= 23:
			sc := ((w >> 7) & 1) | (((w >> 15) & 1) << 1)
			switch sc {
			case 2:
				return KVcrossQuat, "vcrsp"
			case 3:
				return KVcrossQuat, "vqmul"
			}
			return KVcrossQuat, "vcrsp/vqmul"
		case group == 28:
			m := (w >> 16) & 15
			switch m {
			case 0:
				return KVmmov, "vmmov"
			case 3:
				return KVmidT, "vmidt"
			case 6:
				return KVfpuMatrixInit, "vmzero"
			case 7:
				return KVfpuMatrixInit, "vmone"
			}
			return KVfpu, "vfpu-matrix1"
		case group == 29:
			return KVrot, "vrot"
		}
		return KVfpu, "vfpu6"
	}
	return KUnsupported, "unknown"
}

var vfpuKinds [256]bool

func init() {
	for _, k := range []Kind{KMfv, KMtv, KVpfx, KViim, KVfim, KVh2f, KVf2h, KVf2i, KVi2f, KVx2i, KVi2x, KVrnd, KVt5650, KVmidT, KVmmov,
		KVfpuMatrixInit, KVidt, KVcst, KVocp, KVfpuHorizontal, KVrot, KVtfm, KVfpuVectorInit, KVfpuVec3, KVscl, KVdot, KVhdp,
		KVcmp, KVminmax, KVfpuCompare3, KVcmov, KVfpuUnary, KVmmul, KVmscl, KVcrossQuat, KVcrs, KLvs, KSvs, KLvq, KSvq,
		KVsgn, KVsocp, KVbfy1, KVflush, KBvf, KBvt, KBvfl, KBvtl, KVfpu} {
		vfpuKinds[k] = true
	}
}

// Decode ports decode().
func Decode(pc, w uint32) *Insn {
	d := &Insn{}
	d.PC = pc
	d.W = w
	d.Op = uint8(w >> 26)
	d.Rs = uint8((w >> 21) & 31)
	d.Rt = uint8((w >> 16) & 31)
	d.Rd = uint8((w >> 11) & 31)
	d.Sa = uint8((w >> 6) & 31)
	d.Imm = w & 0xFFFF
	d.Simm = s16(d.Imm)
	d.Tgt = (w & 0x3FFFFFF) << 2
	d.Kind, d.Mn = classify(w)
	mn := d.Mn
	d.Likely = brLikely[mn]
	d.Ctl = isCtrl(mn)
	d.Delay = d.Ctl
	d.Vfpu = vfpuKinds[d.Kind] || (d.Kind == KExtra && mn != "ll" && mn != "sc")
	d.Mem = memNone
	d.Fuse = 0
	d.Uses, d.Defs = usedef(d)
	return d
}

// usedef ports _usedef(): (uses, defs) of the instruction proper (for control instructions: the operand reads at the branch
// point). Link-register writes of jal/jalr are NOT included (handled by the call model).
func usedef(d *Insn) (RegSet, RegSet) {
	m := d.Mn
	k := d.Kind
	rs, rt, rd, sa := int(d.Rs), int(d.Rt), int(d.Rd), int(d.Sa)
	none := RegSet{}
	switch k {
	case KNop, KSync, KCache, KVflush, KVpfx, KBreak, KSyscall:
		return none, none
	case KSll, KSrl, KSra, KRotr:
		return mkSet(rt), mkSet(rd)
	case KSllv, KSrlv, KSrav, KRotrv:
		return mkSet(rt, rs), mkSet(rd)
	case KMovz, KMovn:
		return mkSet(rs, rt, rd), mkSet(rd)
	case KAdd, KAddu, KSub, KSubu, KAnd, KOr, KXor, KNor, KSlt, KSltu, KMax, KMin:
		return mkSet(rs, rt), mkSet(rd)
	case KAddiu, KSlti, KSltiu, KAndi, KOri, KXori:
		return mkSet(rs), mkSet(rt)
	case KLui:
		return none, mkSet(rt)
	case KClz, KClo:
		return mkSet(rs), mkSet(rd)
	case KExt:
		return mkSet(rs), mkSet(rt)
	case KIns:
		return mkSet(rs, rt), mkSet(rt)
	case KSeb, KSeh, KBitrev, KWsbh, KWsbw:
		return mkSet(rt), mkSet(rd)
	case KLb, KLh, KLw, KLbu, KLhu:
		d.Mem = memLd
		return mkSet(rs), mkSet(rt)
	case KLwl, KLwr:
		d.Mem = memLd
		return mkSet(rs, rt), mkSet(rt)
	case KSb, KSh, KSw, KSwl, KSwr:
		d.Mem = memSt
		return mkSet(rs, rt), none
	case KLwc1:
		d.Mem = memLd
		return mkSet(rs), mkSet(F(rt))
	case KSwc1:
		d.Mem = memSt
		return mkSet(rs, F(rt)), none
	case KMult, KMultu, KDiv, KDivu:
		return mkSet(rs, rt), mkSet(HI, LO)
	case KMadd, KMaddu, KMsub, KMsubu:
		return mkSet(rs, rt, HI, LO), mkSet(HI, LO)
	case KMfhi:
		return mkSet(HI), mkSet(rd)
	case KMflo:
		return mkSet(LO), mkSet(rd)
	case KMthi:
		return mkSet(rs), mkSet(HI)
	case KMtlo:
		return mkSet(rs), mkSet(LO)
	case KMfc1:
		return mkSet(F(rd)), mkSet(rt)
	case KMtc1:
		return mkSet(rt), mkSet(F(rd))
	case KCfc1:
		return mkSet(CC), mkSet(rt)
	case KCtc1:
		return mkSet(rt), mkSet(CC)
	case KAddS, KSubS, KMulS, KDivS:
		return mkSet(F(rd), F(rt)), mkSet(F(sa))
	case KSqrtS, KAbsS, KMovS, KNegS, KRoundWS, KTruncWS, KCeilWS, KFloorWS, KCvtWS, KCvtSW:
		return mkSet(F(rd)), mkSet(F(sa))
	case KFpuCompare:
		return mkSet(F(rd), F(rt)), mkSet(CC)
	case KLvs, KLvq:
		d.Mem = memLd
		return mkSet(rs), none
	case KSvs, KSvq:
		d.Mem = memSt
		return mkSet(rs), none
	case KMtv:
		return mkSet(rt), none
	case KMfv:
		return none, mkSet(rt)
	case KExtra:
		switch d.Mn {
		case "lvl.q", "lvr.q":
			d.Mem = memLd
			return mkSet(rs), none
		case "svl.q", "svr.q":
			d.Mem = memSt
			return mkSet(rs), none
		case "ll":
			d.Mem = memLd
			return mkSet(rs), mkSet(rt)
		case "sc":
			d.Mem = memSt
			return mkSet(rs, rt), mkSet(rt)
		}
		return none, none
	}
	if vfpuKinds[k] && !(k == KBvf || k == KBvt || k == KBvfl || k == KBvtl) {
		return none, none
	}
	// control
	switch m {
	case "beq", "bne", "beql", "bnel":
		return mkSet(rs, rt), none
	case "blez", "bgtz", "bltz", "bgez", "blezl", "bgtzl", "bltzl", "bgezl":
		return mkSet(rs), none
	case "bltzal", "bgezal", "bltzall", "bgezall":
		return mkSet(rs), mkSet(RA)
	case "bc1f", "bc1t", "bc1fl", "bc1tl":
		return mkSet(CC), none
	case "bvf", "bvt", "bvfl", "bvtl":
		return none, none
	case "jr", "jalr":
		return mkSet(rs), none
	}
	return none, none
}

func branchTarget(pc uint32, d *Insn) uint32 { return pc + 4 + uint32(d.Simm<<2) }
func jumpTarget(pc uint32, d *Insn) uint32   { return ((pc + 4) & 0xF0000000) | d.Tgt }

// Tri is Python's True / False / None.
type Tri int8

const (
	TriNone  Tri = -1
	TriFalse Tri = 0
	TriTrue  Tri = 1
)

// branchConst ports branch_const(): True/False when the outcome of a branch is statically known, else None.
func branchConst(d *Insn) Tri {
	m := d.Mn
	if d.Likely {
		m = m[:len(m)-1]
	}
	rs, rt := d.Rs, d.Rt
	if m == "beq" && rs == rt {
		return TriTrue
	}
	if m == "bne" && rs == rt {
		return TriFalse
	}
	if rs == 0 && (m == "blez" || m == "bgez") {
		return TriTrue
	}
	if rs == 0 && (m == "bgtz" || m == "bltz") {
		return TriFalse
	}
	if (m == "bltzal" || m == "bgezal") && rs == 0 {
		if m == "bgezal" {
			return TriTrue
		}
		return TriFalse
	}
	return TriNone
}
