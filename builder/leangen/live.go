// live.go mirrors recomp/leangen/live.py: liveness with dead-code elimination, interprocedural live-in sets A, demand R, return
// sets R', and the typed/regfile decision.
//
// Resources: bits 0..66 registers (see analysis.go), SlotBs+i promoted stack slots, vtgt (jalr target temp).
//
// Python kept these sets in arbitrary precision integers with the "weak" copy of every bit shifted up by 512. Bits is the same
// thing with a fixed width: the strong half is bits 0..255 (registers, promoted slots, the jalr target temporary), the weak half
// is the same layout shifted up by wsh = 256.
package leangen

import (
	"fmt"
	"strings"
)

const (
	bitsWords = 8
	wsh       = 256
	vtgtBit   = 250
	maxSlots  = vtgtBit - SlotBs
	maxParams = 24
)

// Bits is a live set: strong half in words 0..3, weak half in words 4..7.
type Bits [bitsWords]uint64

func (a Bits) or(b Bits) Bits {
	for i := range a {
		a[i] |= b[i]
	}
	return a
}

func (a Bits) andNot(b Bits) Bits {
	for i := range a {
		a[i] &^= b[i]
	}
	return a
}

func (a Bits) intersects(b Bits) bool {
	for i := range a {
		if a[i]&b[i] != 0 {
			return true
		}
	}
	return false
}

func (a Bits) bit(i int) bool { return a[i>>6]>>uint(i&63)&1 != 0 }

func (a *Bits) set(i int) { a[i>>6] |= 1 << uint(i&63) }

// reg returns the strong register part.
func (a Bits) reg() Mask { return Mask{a[0], a[1]} }

// weakReg returns the weak register part.
func (a Bits) weakReg() Mask { return Mask{a[4], a[5]} }

func strongBits(m Mask) Bits { return Bits{m.Lo, m.Hi} }
func weakBits(m Mask) Bits   { return Bits{0, 0, 0, 0, m.Lo, m.Hi} }

// CBits is a compact node set: a register mask plus at most one extra bit (a promoted slot or the jalr target temporary).
type CBits struct {
	lo uint64
	hi uint8 // bits 64..66 of the register mask
	x  int16 // extra bit, -1: none
}

func (c *CBits) setMask(m Mask) { c.lo, c.hi = m.Lo, uint8(m.Hi) }
func cbMask(m Mask) CBits       { return CBits{lo: m.Lo, hi: uint8(m.Hi), x: -1} }
func (c CBits) mask() Mask      { return Mask{c.lo, uint64(c.hi)} }

func (c CBits) strong() Bits {
	b := strongBits(c.mask())
	if c.x >= 0 {
		b.set(int(c.x))
	}
	return b
}

// both returns c | c << wsh (the "dk" form).
func (c CBits) both() Bits {
	b := strongBits(c.mask()).or(weakBits(c.mask()))
	if c.x >= 0 {
		b.set(int(c.x))
		b.set(int(c.x) + wsh)
	}
	return b
}

var (
	notOut       Mask
	demandNotOut Mask
	gpBit        = bitMask(GP)
	spBit        = bitMask(SP)
	raBit        = bitMask(RA)
)

func init() {
	notOut = resMask.AndNot(outEnc)
	demandNotOut = notOut.And(calleeSaved)
}

func maskOfSet(s RegSet) Mask {
	var m Mask
	for i := 0; i < s.Len(); i++ {
		m = m.Or(bitMask(s.At(i)))
	}
	return m
}

var sideKinds = map[Kind]bool{KAdd: true, KSub: true, KCtc1: true}

// insnUD ports insn_ud(): (uses, defs, side) for a plain instruction instance, honouring promoted stack slots.
func insnUD(d *Insn, mi MemInfo) (CBits, CBits, bool) {
	k := d.Kind
	if mi.Has && mi.Slot >= 0 {
		if mi.Slot >= maxSlots {
			panic(fmt.Sprintf("function has more than %d promoted stack slots", maxSlots))
		}
		slot := int16(SlotBs + mi.Slot)
		if k == KLw || k == KLwc1 {
			return CBits{x: slot}, cbMask(maskOfSet(d.Defs)), false
		}
		r := int(d.Rt)
		if k != KSw {
			r = F(int(d.Rt))
		}
		return cbMask(bitMask(r)), CBits{x: slot}, false
	}
	uses := maskOfSet(d.Uses).AndNot(gpBit)
	defs := maskOfSet(d.Defs)
	side := d.Mem == memSt || d.Vfpu || sideKinds[k] || k == KExtra
	return cbMask(uses), cbMask(defs), side
}

// FL holds the per-function static facts for the data-flow passes.
type FL struct {
	f        *Func
	fa       *FA
	use      []CBits
	defs     []CBits
	side     []bool
	md       []Mask
	mdOK     bool
	order    []int
	hasCalls bool
}

func newFL(f *Func, fa *FA) *FL {
	nodes := f.Nodes
	n := len(nodes)
	fl := &FL{f: f, fa: fa, use: make([]CBits, n), defs: make([]CBits, n), side: make([]bool, n)}
	for i := range fl.use {
		fl.use[i].x = -1
		fl.defs[i].x = -1
	}
	for _, nd := range nodes {
		i := nd.ID
		switch nd.Kind {
		case NN, NSlot:
			fl.use[i], fl.defs[i], fl.side[i] = insnUD(nd.Insn, fa.Mem[i])
		case NBr:
			d := nd.Insn
			u := maskOfSet(d.Uses).AndNot(gpBit)
			if strings.HasPrefix(d.Mn, "bc1") {
				u = u.Or(bitMask(CC))
			}
			fl.use[i].setMask(u)
			fl.defs[i].setMask(maskOfSet(d.Defs))
			fl.side[i] = true
		case NCall:
			d := nd.Insn
			if d.Mn == "jal" {
				fl.defs[i].setMask(raBit)
			} else {
				fl.use[i].setMask(bitMask(int(d.Rs)).AndNot(gpBit))
				fl.defs[i].setMask(bitMask(int(d.Rd)))
				fl.defs[i].x = vtgtBit
			}
		case NSwitch:
			ir := nd.Table().IdxReg
			if !nd.Table().Idx {
				ir = int(nd.Insn.Rs)
			}
			if ir != 0 {
				fl.use[i].setMask(bitMask(ir).AndNot(gpBit))
			}
			fl.side[i] = true
		}
	}
	for _, nd := range nodes {
		if nd.Kind == NCont || nd.Kind == NTail {
			fl.hasCalls = true
			break
		}
	}
	return fl
}

func (fl *FL) postorder() []int {
	if fl.order != nil {
		return fl.order
	}
	nodes := fl.f.Nodes
	seen := make([]bool, len(nodes))
	order := make([]int, 0, len(nodes))
	s0 := fl.f.EntryID
	type fr struct{ i, j int }
	stack := []fr{{s0, 0}}
	seen[s0] = true
	for len(stack) > 0 {
		top := &stack[len(stack)-1]
		su := nodes[top.i].Succ
		if top.j < len(su) {
			s := su[top.j]
			top.j++
			if !seen[s] {
				seen[s] = true
				stack = append(stack, fr{s, 0})
			}
		} else {
			order = append(order, top.i)
			stack = stack[:len(stack)-1]
		}
	}
	fl.order = order
	return order
}

// computeMD ports compute_md(): forward may-defined registers at node entry (instruction and call results).
func (fl *FL) computeMD(calleeDefs func(*Node) Mask) []Mask {
	nodes := fl.f.Nodes
	md := make([]Mask, len(nodes))
	ok := make([]bool, len(nodes))
	start := fl.f.EntryID
	ok[start] = true
	work := []int{start}
	inq := make([]bool, len(nodes))
	inq[start] = true
	for h := 0; h < len(work); h++ {
		i := work[h]
		inq[i] = false
		nd := nodes[i]
		cur := md[i]
		var out Mask
		switch nd.Kind {
		case NN, NSlot, NBr, NCall:
			out = cur.Or(fl.defs[i].mask())
		case NCont:
			dc := calleeDefs(nd)
			out = cur.AndNot(dc).Or(dc.And(outEnc))
		default:
			out = cur
		}
		for _, s := range nd.Succ {
			if !ok[s] {
				ok[s] = true
				md[s] = out
				if !inq[s] {
					inq[s] = true
					work = append(work, s)
				}
				continue
			}
			nw := md[s].Or(out)
			if nw != md[s] {
				md[s] = nw
				if !inq[s] {
					inq[s] = true
					work = append(work, s)
				}
			}
		}
	}
	fl.md = md
	fl.mdOK = true
	return md
}

func runLen(m Mask, regs []int) int {
	k := 0
	for _, r := range regs {
		if m.Has(r) {
			k++
		} else {
			break
		}
	}
	return k
}

// Solver ports the Solver class.
type Solver struct {
	P           *Program2
	S           map[uint32]*Summary
	Imports     map[uint32]Import
	full        bool
	forceRegs   Mask
	jalrSeen    Mask
	fl          map[uint32]*FL
	ar          map[uint32]Mask
	ImportArity map[uint32]Arity
	ExtraInputs map[uint32]Mask
	weakEntry   Mask
	jalrWeakAll bool
	Stats       map[string]int
	Runs        int
}

// NewSolver ports Solver.__init__ together with set_weak_args / set_jalr_weak.
func NewSolver(P *Program2, weakArgs int, jalrWeakAll bool) *Solver {
	L := &Solver{P: P, S: P.S, Imports: P.Imports, forceRegs: bitMask(V0), fl: map[uint32]*FL{}, ar: map[uint32]Mask{},
		ImportArity: map[uint32]Arity{}, ExtraInputs: map[uint32]Mask{}, Stats: map[string]int{}, jalrWeakAll: jalrWeakAll}
	for r := 4; r < 4+weakArgs; r++ {
		L.weakEntry = L.weakEntry.Or(bitMask(r))
	}
	for r := 12; r < 12+weakArgs; r++ {
		L.weakEntry = L.weakEntry.Or(bitMask(F(r)))
	}
	for _, e := range P.Keys {
		s := P.S[e]
		L.fl[e] = newFL(s.F, s.FA)
		s.A, s.Aw, s.R = Mask{}, Mask{}, Mask{}
	}
	return L
}

// ------------------------------------------------------------------ call model

func (L *Solver) calleeDefs(nd *Node) Mask {
	if nd.Kind == NCont && nd.Insn.Mn == "jalr" {
		return L.P.MwInd
	}
	if !nd.hasTgt() {
		return L.P.MwInd
	}
	return L.P.MwOf(uint32(nd.Tgt))
}

// callUses ports call_uses(): (strong, weak) register uses of a call-completion node (argument marshalling).
func (L *Solver) callUses(nd *Node, md Mask, mdOK bool) (Mask, Mask) {
	if !mdOK {
		md = argsMask
	}
	if nd.Kind == NCont && nd.Insn.Mn == "jalr" {
		if L.jalrWeakAll {
			return spBit, argsNoSP.And(md.Or(L.weakEntry))
		}
		return spBit, argsNoSP.And(md)
	}
	t := uint32(nd.Tgt)
	s := L.S[t]
	if s != nil {
		if s.Variadic {
			return s.A.And(spBit), s.A.Or(s.Aw).AndNot(spBit)
		}
		return s.A, s.Aw
	}
	if _, ok := L.Imports[t]; ok {
		return spBit, argsNoSP.And(md.Or(L.ar[t]))
	}
	return spBit, argsNoSP.And(md)
}

// prepare ports prepare(): import argument arity from the call sites.
func (L *Solver) prepare() {
	arity := map[uint32][2]int{}
	for _, e := range L.P.Keys {
		fl := L.fl[e]
		if !fl.hasCalls {
			continue
		}
		md := fl.computeMD(L.calleeDefs)
		for _, nd := range fl.f.Nodes {
			if (nd.Kind != NCont && nd.Kind != NTail) || !nd.hasTgt() {
				continue
			}
			if _, imp := L.Imports[uint32(nd.Tgt)]; !imp {
				continue
			}
			m := md[nd.ID]
			ni := runLen(m, argGPR)
			nf := runLen(m, argFPR)
			a := arity[uint32(nd.Tgt)]
			if ni > a[0] {
				a[0] = ni
			}
			if nf > a[1] {
				a[1] = nf
			}
			arity[uint32(nd.Tgt)] = a
		}
	}
	// The prototype of the syscall wins where it takes more than any call site defines: a thin wrapper that forwards its own
	// incoming arguments (the callee's second argument is the wrapper's a1) defines fewer registers than the call takes, and an
	// argument that is not marshalled reaches the HLE as stale g_cpu contents.
	for t, ar := range L.ImportArity {
		if _, imp := L.Imports[t]; !imp {
			continue
		}
		a := arity[t]
		if ar.GPR > a[0] || ar.FPR > a[1] {
			L.Stats["import_arity_raised"]++
		}
		if ar.GPR > a[0] {
			a[0] = ar.GPR
		}
		if ar.FPR > a[1] {
			a[1] = ar.FPR
		}
		arity[t] = a
	}
	for t, a := range arity {
		var m Mask
		for _, r := range argGPR[:a[0]] {
			m = m.Or(bitMask(r))
		}
		for _, r := range argFPR[:a[1]] {
			m = m.Or(bitMask(r))
		}
		L.ar[t] = m
	}
}

func (L *Solver) paramMask(e uint32) Mask { return argsMask.Or(L.ExtraInputs[e]) }

// ------------------------------------------------------------------ liveness

func (L *Solver) forceMask(s *Summary) Mask {
	if L.full {
		return outEnc
	}
	if s.At {
		return L.forceRegs
	}
	return Mask{}
}

// Rp ports rp().
func (L *Solver) Rp(s *Summary) Mask { return s.R.Or(L.forceMask(s)).And(s.Mw) }

// Liveness ports liveness(): backward live-variable analysis with DCE. Returns (A, Aw, live_in, md).
func (L *Solver) Liveness(e uint32, retExtra Mask) (Mask, Mask, []Bits, []Mask, bool) {
	s := L.S[e]
	fl := L.fl[e]
	f := s.F
	nodes := f.Nodes
	N := len(nodes)
	rpself := L.Rp(s)
	var md []Mask
	mdOK := false
	if fl.mdOK {
		md, mdOK = fl.md, true
	} else if fl.hasCalls {
		md, mdOK = fl.computeMD(L.calleeDefs), true
	}
	liveIn := make([]Bits, N)
	order := fl.postorder()
	work := make([]int, len(order), len(order)+N)
	copy(work, order)
	inq := make([]bool, N)
	for _, i := range order {
		inq[i] = true
	}
	retLive := strongBits(rpself.And(outEnc).Or(retExtra)).or(weakBits(rpself.And(notOut)))
	for h := 0; h < len(work); h++ {
		i := work[h]
		inq[i] = false
		nd := nodes[i]
		var out Bits
		for _, sx := range nd.Succ {
			out = out.or(liveIn[sx])
		}
		var nw Bits
		switch nd.Kind {
		case NN, NSlot:
			d := fl.defs[i].both()
			if !fl.side[i] && !d.intersects(out) {
				nw = out
			} else {
				nw = out.andNot(d).or(fl.use[i].strong())
			}
		case NCont:
			var mi Mask
			if mdOK {
				mi = md[i]
			}
			us, uw := L.callUses(nd, mi, mdOK)
			d0 := L.calleeDefs(nd)
			d := strongBits(d0).or(weakBits(d0))
			nw = out.andNot(d).or(strongBits(us)).or(weakBits(uw)).or(strongBits(out.reg().And(d0).And(demandNotOut)))
		case NRet:
			nw = retLive
		case NTail:
			var mi Mask
			if mdOK {
				mi = md[i]
			}
			us, uw := L.callUses(nd, mi, mdOK)
			d0 := L.calleeDefs(nd)
			d := strongBits(d0).or(weakBits(d0))
			nw = retLive.andNot(d).or(strongBits(us)).or(weakBits(uw)).or(weakBits(rpself.And(d0).And(demandNotOut)))
		case NFatal, NIjmp:
			nw = Bits{}
		case NBr, NCall, NSwitch:
			nw = out.andNot(fl.defs[i].both()).or(fl.use[i].strong())
		default: // jmp, jr
			nw = out
		}
		if nw != liveIn[i] {
			liveIn[i] = nw
			for _, p := range nd.Pred {
				if !inq[p] {
					inq[p] = true
					work = append(work, p)
				}
			}
		}
	}
	li := liveIn[f.EntryID]
	A := li.reg().And(L.paramMask(e))
	Aw := li.weakReg().And(L.weakEntry).AndNot(A)
	return A, Aw, liveIn, md, mdOK
}

// runFunction ports run_function(): one liveness pass; propagate A to callers and demand to callees. Returns (A changed,
// callees whose R grew).
func (L *Solver) runFunction(e uint32) (bool, []uint32) {
	s := L.S[e]
	A, Aw, liveIn, _, _ := L.Liveness(e, Mask{})
	changed := A != s.A || Aw != s.Aw
	s.A, s.Aw = A, Aw
	var grew []uint32
	rpself := L.Rp(s)
	for _, nd := range s.F.Nodes {
		k := nd.Kind
		if k != NCont && k != NTail {
			continue
		}
		var out Mask
		if k == NCont {
			var ob Bits
			for _, sx := range nd.Succ {
				ob = ob.or(liveIn[sx])
			}
			out = ob.reg()
		} else {
			out = rpself
		}
		if k == NCont && nd.Insn.Mn == "jalr" {
			L.jalrSeen = L.jalrSeen.Or(out.And(outEnc))
			continue // indirect callees return v0/v1/f0 only (contract); no demand
		}
		if !nd.hasTgt() {
			continue
		}
		t := uint32(nd.Tgt)
		ts := L.S[t]
		if ts == nil {
			continue
		}
		dem := out.And(ts.Mw).And(outEnc.Or(demandNotOut))
		if !dem.AndNot(ts.R).IsZero() {
			ts.R = ts.R.Or(dem)
			grew = append(grew, t)
		}
	}
	return changed, grew
}

// Solve ports solve(): fixpoint of liveness / demand; address-taken functions return v0 plus whatever any jalr site reads back.
func (L *Solver) Solve(progress func(done, total int)) int {
	L.prepare()
	total := L.solveOnce(progress)
	for {
		need := L.jalrSeen.And(outEnc).AndNot(L.forceRegs)
		if need.IsZero() {
			break
		}
		L.forceRegs = L.forceRegs.Or(need)
		total += L.solveOnce(progress)
	}
	L.Runs = total
	return total
}

func (L *Solver) solveOnce(progress func(done, total int)) int {
	order := L.P.postorder()
	q := newQueue(order)
	runs := 0
	for !q.empty() {
		e := q.pop()
		ch, grew := L.runFunction(e)
		runs++
		if ch {
			for _, c := range L.P.Callers[e] {
				q.push(c)
			}
		}
		for _, g := range grew {
			q.push(g)
		}
		if progress != nil && runs%256 == 0 {
			progress(len(order)-q.size(), len(order))
		}
	}
	return runs
}

// StrongRp ports strong_rp(): returned v0/v1/f0 per function when nothing is forced (no address-taken forcing): the registers some
// strong reader depends on. State is restored afterwards.
func (L *Solver) StrongRp() map[uint32]Mask {
	type saved struct{ r, a, aw Mask }
	sv := make(map[uint32]saved, len(L.S))
	for e, s := range L.S {
		sv[e] = saved{s.R, s.A, s.Aw}
	}
	keepForce, keepSeen, keepFull := L.forceRegs, L.jalrSeen, L.full
	for _, s := range L.S {
		s.R = Mask{}
	}
	L.forceRegs = Mask{}
	L.full = false
	L.solveOnce(nil)
	out := make(map[uint32]Mask, len(L.S))
	for e, s := range L.S {
		out[e] = L.Rp(s).And(outEnc)
	}
	for e, v := range sv {
		s := L.S[e]
		s.R, s.A, s.Aw = v.r, v.a, v.aw
	}
	L.forceRegs, L.jalrSeen, L.full = keepForce, keepSeen, keepFull
	return out
}

func regList(prefix string, regs []int) string {
	parts := make([]string, len(regs))
	for i, r := range regs {
		parts[i] = fmt.Sprintf("r%d", r)
	}
	return prefix + strings.Join(parts, ",")
}

// Decide ports decide(): the typed / regfile decision.
func (L *Solver) Decide() {
	srp := L.StrongRp() // v0 / v1 / f0 some caller really reads (not merely forced by an indirect call)
	for _, e := range L.P.Keys {
		s := L.S[e]
		var why []string
		if len(s.Reasons) > 0 {
			r := s.Reasons[0]
			if i := strings.Index(r, " at "); i >= 0 {
				r = r[:i]
			}
			why = append(why, "structural: "+r)
		}
		rp := L.Rp(s)
		s.RetUse = rp
		if x := rp.AndNot(outEnc); !x.IsZero() {
			why = append(why, regList("returns ", x.Regs()))
		} else if !srp[e].IsZero() {
			// a path that returns the caller's untouched v0 / v1 / f0 while a caller reads it: only the register file form can
			// hand the incoming value back (the typed form would return its zero-initialised local)
			_, _, li, _, _ := L.Liveness(e, Mask{})
			x := li[s.F.EntryID].reg().And(srp[e])
			if !x.IsZero() {
				why = append(why, regList("pass-through of ", x.Regs()))
			}
		}
		if s.A.Count() > maxParams {
			why = append(why, "too many params")
		}
		for _, x := range s.F.Issues {
			if strings.HasPrefix(x, "branch-and-link") {
				why = append(why, "branch-and-link")
				break
			}
		}
		s.Typed = len(why) == 0
		s.Why = why
	}
}
