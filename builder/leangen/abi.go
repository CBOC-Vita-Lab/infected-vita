// abi.go mirrors recomp/leangen/abi.py: calling-convention helpers shared by the emitter, the thunk generator and the manifest.
//
// Typed form   RET f_ADDR(u32 sp, u32 a0..a7, float f12..f19)     (only the registers in A | Aw, in this order)
// Regfile form void t_ADDR()                                        (g_cpu is the argument / result area)
// Thunk        void t_ADDR()   for typed functions: loads A|Aw from g_cpu, calls f_ADDR, stores the returned registers
//
// Return packing (R' = returned subset of v0, v1, f0):
//
//	{}            void
//	{v0} {v1}     uint32_t
//	{f0}          float
//	two of three  uint64_t   (lo = first, hi = second; f0 travels as its bit pattern)
//	all three     lean_r3    { uint64_t a = v0 | v1 << 32; uint32_t b = f0 bits }
package leangen

import (
	"fmt"
	"strings"
)

// paramsOf ports params(): ordered parameter registers of a typed function (m is A | Aw).
func paramsOf(m Mask) []int {
	regs := m.Regs()
	var out []int
	for _, r := range regs {
		if r == SP {
			out = append(out, r)
		}
	}
	for _, r := range regs {
		if 4 <= r && r <= 11 {
			out = append(out, r)
		}
	}
	for _, r := range regs {
		if F(12) <= r && r <= F(19) {
			out = append(out, r)
		}
	}
	return out
}

func params(s *Summary) []int { return paramsOf(s.A.Or(s.Aw)) }

// rset ports rset(): returned registers (typed form).
func (L *Solver) rset(s *Summary) []int {
	m := L.Rp(s).And(outEnc)
	var out []int
	for _, r := range []int{V0, V1, F0} {
		if m.Has(r) {
			out = append(out, r)
		}
	}
	return out
}

var regNames [NRES]string

func init() {
	for r := 0; r < NRES; r++ {
		switch {
		case r == 0:
			regNames[r] = "0u"
		case r == SP:
			regNames[r] = "sp"
		case r < 32:
			regNames[r] = fmt.Sprintf("r%d", r)
		case r < 64:
			regNames[r] = fmt.Sprintf("f%d", r-32)
		case r == 64:
			regNames[r] = "cc"
		case r == 65:
			regNames[r] = "hi"
		default:
			regNames[r] = "lo"
		}
	}
}

func regName(r int) string { return regNames[r] }

func isFP(r int) bool { return 32 <= r && r < 64 }

func ctype(r int) string {
	if isFP(r) {
		return "float"
	}
	return "uint32_t"
}

func retType(regs []int) string {
	switch len(regs) {
	case 0:
		return "void"
	case 1:
		if regs[0] == F0 {
			return "float"
		}
		return "uint32_t"
	case 2:
		return "uint64_t"
	}
	return "lean_r3"
}

func asBits(r int) string {
	if isFP(r) {
		return "lean_f2u(" + regName(r) + ")"
	}
	return regName(r)
}

func fromBits(r int, expr string) string {
	if isFP(r) {
		return "lean_u2f(" + expr + ")"
	}
	return expr
}

// packReturn ports pack_return(): C expression for the return value.
func packReturn(regs []int) string {
	switch len(regs) {
	case 0:
		return ""
	case 1:
		return regName(regs[0])
	case 2:
		return fmt.Sprintf("lean_pk2(%s, %s)", asBits(regs[0]), asBits(regs[1]))
	}
	return fmt.Sprintf("lean_r3{lean_pk2(%s, %s), %s}", asBits(regs[0]), asBits(regs[1]), asBits(regs[2]))
}

// unpackReturn ports unpack_return(): C statements assigning the returned registers from the call expression.
func unpackReturn(regs []int, expr string) []string {
	const tmp = "rv"
	switch len(regs) {
	case 0:
		return []string{expr + ";"}
	case 1:
		return []string{fmt.Sprintf("%s = %s;", regName(regs[0]), expr)}
	case 2:
		return []string{fmt.Sprintf("{ const uint64_t %s = %s;", tmp, expr),
			fmt.Sprintf("  %s = %s;", regName(regs[0]), fromBits(regs[0], "lean_lo("+tmp+")")),
			fmt.Sprintf("  %s = %s; }", regName(regs[1]), fromBits(regs[1], "lean_hi("+tmp+")"))}
	}
	return []string{fmt.Sprintf("{ const lean_r3 %s = %s;", tmp, expr),
		fmt.Sprintf("  %s = %s;", regName(regs[0]), fromBits(regs[0], "lean_lo("+tmp+".a)")),
		fmt.Sprintf("  %s = %s;", regName(regs[1]), fromBits(regs[1], "lean_hi("+tmp+".a)")),
		fmt.Sprintf("  %s = %s; }", regName(regs[2]), fromBits(regs[2], tmp+".b"))}
}

// cpuRef ports cpu_ref(): g_cpu storage of a register.
func cpuRef(r int) string {
	switch {
	case r < 32:
		return fmt.Sprintf("g_cpu.gpr[%d]", r)
	case r < 64:
		return fmt.Sprintf("g_cpu.fpr[%d]", r-32)
	case r == 65:
		return "g_cpu.hi"
	case r == 66:
		return "g_cpu.lo"
	}
	panic(fmt.Sprintf("no g_cpu storage for resource %d", r))
}

// signature ports signature().
func (L *Solver) signature(s *Summary) string {
	ps := params(s)
	args := make([]string, len(ps))
	for i, r := range ps {
		args[i] = ctype(r) + " " + regName(r)
	}
	a := strings.Join(args, ", ")
	if a == "" {
		a = "void"
	}
	return fmt.Sprintf("%s f_%08X(%s)", retType(L.rset(s)), s.Entry, a)
}
