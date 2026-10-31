// analysis.go mirrors recomp/leangen/analysis.py: whole-program analysis for the lean generator.
//
// Per function:  abstract interpretation (sp / saved registers / return address / stack slots)  ->  modified set MW,
//
//	structural checks, promotable stack slots.
//
// Per program:   fixpoint over the call graph for MW, then liveness (with dead-code elimination) for in-sets A, demand R,
//
//	return sets R', and the typed / regfile decision.
//
// Resource bits: 0..31 GPR, 32..63 FPR, 64 CC, 65 HI, 66 LO, 70+ promoted stack slots / switch index temporaries.
package leangen

import (
	"fmt"
	"math/bits"
	"sort"
)

// Mask is a set of registers (resource bits 0..127; Python used arbitrary precision ints for these).
type Mask struct{ Lo, Hi uint64 }

func bitMask(r int) Mask {
	if r < 64 {
		return Mask{Lo: 1 << uint(r)}
	}
	return Mask{Hi: 1 << uint(r-64)}
}

func (m Mask) Or(o Mask) Mask     { return Mask{m.Lo | o.Lo, m.Hi | o.Hi} }
func (m Mask) And(o Mask) Mask    { return Mask{m.Lo & o.Lo, m.Hi & o.Hi} }
func (m Mask) AndNot(o Mask) Mask { return Mask{m.Lo &^ o.Lo, m.Hi &^ o.Hi} }
func (m Mask) IsZero() bool       { return m.Lo == 0 && m.Hi == 0 }
func (m Mask) Has(r int) bool {
	if r < 64 {
		return m.Lo>>uint(r)&1 != 0
	}
	return m.Hi>>uint(r-64)&1 != 0
}
func (m Mask) Count() int { return bits.OnesCount64(m.Lo) + bits.OnesCount64(m.Hi) }

// Regs ports regs_in(): the set bits in ascending order.
func (m Mask) Regs() []int {
	var out []int
	for w := 0; w < 2; w++ {
		x := m.Lo
		if w == 1 {
			x = m.Hi
		}
		for x != 0 {
			b := bits.TrailingZeros64(x)
			out = append(out, w*64+b)
			x &= x - 1
		}
	}
	return out
}

func maskOf(regs ...int) Mask {
	var m Mask
	for _, r := range regs {
		m = m.Or(bitMask(r))
	}
	return m
}

const (
	NRES   = 67
	V0     = 2
	V1     = 3
	F0     = 32 // F(0)
	INF    = 1 << 30
	SlotBs = 70 // SLOT_BASE
)

var (
	resMask       Mask
	outEnc        Mask
	argsMask      Mask
	argsNoSP      Mask
	callerSaved   Mask
	calleeSaved   Mask
	importMW      Mask
	isCalleeSaved [NRES]bool
	argGPR        []int
	argFPR        []int
)

func init() {
	for r := 0; r < NRES; r++ {
		resMask = resMask.Or(bitMask(r))
	}
	outEnc = maskOf(V0, V1, F0)
	for r := 4; r < 12; r++ {
		argGPR = append(argGPR, r)
	}
	for i := 12; i < 20; i++ {
		argFPR = append(argFPR, F(i))
	}
	for _, r := range argGPR {
		argsMask = argsMask.Or(bitMask(r))
	}
	for _, r := range argFPR {
		argsMask = argsMask.Or(bitMask(r))
	}
	argsMask = argsMask.Or(bitMask(SP))
	argsNoSP = argsMask.AndNot(bitMask(SP))
	for r := 1; r < 16; r++ {
		callerSaved = callerSaved.Or(bitMask(r))
	}
	callerSaved = callerSaved.Or(maskOf(24, 25, HI, LO, CC))
	for i := 0; i < 20; i++ {
		callerSaved = callerSaved.Or(bitMask(F(i)))
	}
	for r := 16; r < 24; r++ {
		calleeSaved = calleeSaved.Or(bitMask(r))
		isCalleeSaved[r] = true
	}
	calleeSaved = calleeSaved.Or(bitMask(30))
	isCalleeSaved[30] = true
	for i := 20; i < 32; i++ {
		calleeSaved = calleeSaved.Or(bitMask(F(i)))
		isCalleeSaved[F(i)] = true
	}
	importMW = maskOf(V0, V1, F0)
}

// ----------------------------------------------------------------------------------------------- abstract state

// Val is an abstract register / stack slot value.
type Val struct {
	K byte // 'c' constant 0, 'o' original value of register N, 's' entry sp + N, 'T' unknown, 'P' possibly never stored
	N int32
}

var (
	valT = Val{'T', 0}
	valP = Val{'P', 0}
)

type slotEnt struct {
	key int
	v   Val
}

// State is (regs, slots): regs[r] == Val{'o', r} stands for "absent" (the register holds its entry value).
type State struct {
	regs  [NRES]Val
	slots []slotEnt
}

func initialState() State {
	var s State
	for r := range s.regs {
		s.regs[r] = Val{'o', int32(r)}
	}
	s.regs[SP] = Val{'s', 0}
	return s
}

func (s *State) get(r int) Val {
	if r == 0 {
		return Val{'c', 0}
	}
	return s.regs[r]
}

func (s *State) set(r int, v Val) {
	if r == 0 {
		return
	}
	s.regs[r] = v // an entry-value result is already Val{'o', r}
}

func (s *State) slot(key int) (Val, bool) {
	i := sort.Search(len(s.slots), func(i int) bool { return s.slots[i].key >= key })
	if i < len(s.slots) && s.slots[i].key == key {
		return s.slots[i].v, true
	}
	return Val{}, false
}

// setSlot copies the slot list (it is shared between states).
func (s *State) setSlot(key int, v Val) {
	i := sort.Search(len(s.slots), func(i int) bool { return s.slots[i].key >= key })
	if i < len(s.slots) && s.slots[i].key == key {
		ns := make([]slotEnt, len(s.slots))
		copy(ns, s.slots)
		ns[i].v = v
		s.slots = ns
		return
	}
	ns := make([]slotEnt, 0, len(s.slots)+1)
	ns = append(ns, s.slots[:i]...)
	ns = append(ns, slotEnt{key, v})
	ns = append(ns, s.slots[i:]...)
	s.slots = ns
}

func slotsEqual(a, b []slotEnt) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func statesEqual(a, b *State) bool { return a.regs == b.regs && slotsEqual(a.slots, b.slots) }

// joinStates ports join_states(a, b) (both non-nil).
func joinStates(a, b *State) State {
	var out State
	for r := range out.regs {
		if a.regs[r] == b.regs[r] {
			out.regs[r] = a.regs[r]
		} else {
			out.regs[r] = valT
		}
	}
	if slotsEqual(a.slots, b.slots) {
		out.slots = a.slots
		return out
	}
	var so []slotEnt
	i, j := 0, 0
	for i < len(a.slots) || j < len(b.slots) {
		switch {
		case j >= len(b.slots) || (i < len(a.slots) && a.slots[i].key < b.slots[j].key):
			so = append(so, slotEnt{a.slots[i].key, valP})
			i++
		case i >= len(a.slots) || b.slots[j].key < a.slots[i].key:
			so = append(so, slotEnt{b.slots[j].key, valP})
			j++
		default:
			va, vb := a.slots[i].v, b.slots[j].v
			var v Val
			if va == vb {
				v = va
			} else if va != valP && vb != valP {
				v = valT
			} else {
				v = valP
			}
			so = append(so, slotEnt{a.slots[i].key, v})
			i++
			j++
		}
	}
	out.slots = so
	return out
}

// memref ports memref(): (is_store, size, base, imm, cls); cls: 'w' word, 'p' partial, 'u' unaligned-pair, 'v' vector/VFPU,
// 'x' ll/sc. ok is false when the instruction has no memory operand.
type memRef struct {
	store bool
	size  int
	base  int
	imm   int
	cls   byte
}

func memrefOf(d *Insn) (memRef, bool) {
	switch d.Kind {
	case KLw, KLwc1:
		return memRef{false, 4, int(d.Rs), int(d.Simm), 'w'}, true
	case KSw, KSwc1:
		return memRef{true, 4, int(d.Rs), int(d.Simm), 'w'}, true
	case KLh, KLhu:
		return memRef{false, 2, int(d.Rs), int(d.Simm), 'p'}, true
	case KSh:
		return memRef{true, 2, int(d.Rs), int(d.Simm), 'p'}, true
	case KLb, KLbu:
		return memRef{false, 1, int(d.Rs), int(d.Simm), 'p'}, true
	case KSb:
		return memRef{true, 1, int(d.Rs), int(d.Simm), 'p'}, true
	case KLwl, KLwr:
		return memRef{false, 4, int(d.Rs), int(d.Simm), 'u'}, true
	case KSwl, KSwr:
		return memRef{true, 4, int(d.Rs), int(d.Simm), 'u'}, true
	case KLvs:
		return memRef{false, 4, int(d.Rs), int(s16(d.W & 0xFFFC)), 'v'}, true
	case KSvs:
		return memRef{true, 4, int(d.Rs), int(s16(d.W & 0xFFFC)), 'v'}, true
	case KLvq:
		return memRef{false, 16, int(d.Rs), int(s16(d.W & 0xFFFC)), 'v'}, true
	case KSvq:
		return memRef{true, 16, int(d.Rs), int(s16(d.W & 0xFFFC)), 'v'}, true
	case KExtra:
		switch d.Mn {
		case "lvl.q", "lvr.q":
			return memRef{false, 16, int(d.Rs), int(s16(d.W & 0xFFFC)), 'v'}, true
		case "svl.q", "svr.q":
			return memRef{true, 16, int(d.Rs), int(s16(d.W & 0xFFFC)), 'v'}, true
		case "ll":
			return memRef{false, 4, int(d.Rs), int(d.Simm), 'x'}, true
		case "sc":
			return memRef{true, 4, int(d.Rs), int(d.Simm), 'x'}, true
		}
	}
	return memRef{}, false
}

// track is the set of stack slot keys followed by the abstract interpretation (ALLK: every key).
type track struct {
	all bool
	set map[int]struct{}
}

func (t *track) has(key int) bool {
	if t.all {
		return true
	}
	_, ok := t.set[key]
	return ok
}

type absFlags struct {
	escape, escapeUp bool
	bad              map[int]struct{}
}

// applyInsn ports apply_insn(): the abstract effect of a non-control instruction (st is updated in place).
func applyInsn(d *Insn, st *State, tr *track, fl *absFlags) {
	k := d.Kind
	mr, hasMr := memrefOf(d)
	if hasMr {
		bv := st.get(mr.base)
		if bv.K == 's' && mr.store {
			key := int(bv.N) + mr.imm
			if mr.cls == 'w' {
				if tr.has(key) {
					var v Val
					if k == KSw {
						v = st.get(int(d.Rt))
					} else {
						v = st.get(F(int(d.Rt)))
					}
					st.setSlot(key, v)
				}
			} else {
				lo := key
				if mr.cls == 'u' {
					lo -= 3
				}
				hi := key + mr.size
				// slots are modified in place only when overlapping
				var changed []int
				for _, se := range st.slots {
					if se.key+4 > lo && se.key < hi {
						changed = append(changed, se.key)
					}
				}
				for _, tk := range changed {
					st.setSlot(tk, valT)
				}
			}
		}
	}
	var nv Val
	dst := -1
	switch {
	case k == KAddiu:
		rv := st.get(int(d.Rs))
		dst = int(d.Rt)
		if rv.K == 's' {
			nv = Val{'s', rv.N + d.Simm}
		} else if d.Simm == 0 {
			nv = rv
		} else {
			nv = valT
		}
		if nv.K == 's' && dst != SP && nv.N >= 0 {
			fl.escapeUp = true
		}
	case (k == KOr || k == KAddu) && (d.Rt == 0 || d.Rs == 0):
		dst = int(d.Rd)
		if d.Rt == 0 {
			nv = st.get(int(d.Rs))
		} else {
			nv = st.get(int(d.Rt))
		}
	case k == KLw || k == KLwc1:
		if k == KLw {
			dst = int(d.Rt)
		} else {
			dst = F(int(d.Rt))
		}
		bv := st.get(int(d.Rs))
		if bv.K == 's' {
			key := int(bv.N) + int(d.Simm)
			if tr.has(key) {
				v, ok := st.slot(key)
				if !ok || v == valP {
					fl.bad[key] = struct{}{}
					v = valT
				}
				nv = v
			} else {
				nv = valT
			}
		} else {
			nv = valT
		}
	case k == KMovz || k == KMovn:
		a := st.get(int(d.Rs))
		b := st.get(int(d.Rd))
		dst = int(d.Rd)
		if a == b {
			nv = a
		} else {
			nv = valT
		}
	}
	if dst >= 0 {
		if dst == 0 {
			return
		}
		if nv.K == 's' && dst != SP {
			fl.escape = true
			if nv.N >= 0 {
				fl.escapeUp = true
			}
		}
		st.set(dst, nv)
		return
	}
	// generic instruction: any use of a stack-derived value other than as memory base escapes it
	if d.Uses.Len() > 0 {
		dataReg := -1
		if hasMr && mr.store && k != KSwc1 {
			dataReg = int(d.Rt)
		}
		for i := 0; i < d.Uses.Len(); i++ {
			u := d.Uses.At(i)
			uv := st.regs[u]
			if uv.K == 's' {
				if hasMr && u == mr.base && u != dataReg {
					continue
				}
				fl.escape = true
				fl.escapeUp = true
			}
		}
	}
	for i := 0; i < d.Defs.Len(); i++ {
		if r := d.Defs.At(i); r != 0 {
			st.regs[r] = valT
		}
	}
}

// applyCall ports apply_call().
func applyCall(st *State, mw Mask) {
	for _, r := range mw.Regs() {
		if r > 0 && r < NRES {
			st.regs[r] = valT
		}
	}
	st.regs[RA] = valT
}

// ----------------------------------------------------------------------------------------------- per-function result

// MemInfo is the stack-relative access of a node: ('sp', key, slot index or None).
type MemInfo struct {
	Key  int32
	Slot int16 // -1: None
	Has  bool
}

// FA is the analysis result of one function.
type FA struct {
	F         *Func
	Mod       Mask
	Reasons   []string
	Escape    bool
	Promoted  map[int]int
	Mem       []MemInfo
	SpDynamic bool
	Variadic  bool
	W         int
	Tracked   map[int]struct{}
}

func (p *Program2) calleeMW(n *Node) Mask {
	if n.Kind == NCont && n.Insn.Mn == "jalr" {
		return p.MwInd
	}
	if !n.hasTgt() {
		return p.MwInd
	}
	return p.MwOf(uint32(n.Tgt))
}

type accEnt struct {
	loads, stores int
	bad           bool
	saveonly      bool
}

// runAbs runs the abstract interpretation to a fixpoint for the given track set; ports the inner loop of
// analyze_function_abs.
func (p *Program2) runAbs(f *Func, tr *track) ([]State, []bool, *absFlags) {
	nodes := f.Nodes
	fl := &absFlags{bad: map[int]struct{}{}}
	sin := make([]State, len(nodes))
	have := make([]bool, len(nodes))
	start := f.EntryID
	sin[start] = initialState()
	have[start] = true
	work := []int{start}
	inwork := make([]bool, len(nodes))
	inwork[start] = true
	for len(work) > 0 {
		nid := work[len(work)-1]
		work = work[:len(work)-1]
		inwork[nid] = false
		n := nodes[nid]
		out := sin[nid]
		switch n.Kind {
		case NN, NSlot:
			applyInsn(n.Insn, &out, tr, fl)
		case NCont:
			applyCall(&out, p.calleeMW(n))
		}
		for _, s := range n.Succ {
			if !have[s] {
				sin[s] = out
				have[s] = true
				if !inwork[s] {
					inwork[s] = true
					work = append(work, s)
				}
				continue
			}
			j := joinStates(&sin[s], &out)
			if !statesEqual(&j, &sin[s]) {
				sin[s] = j
				if !inwork[s] {
					inwork[s] = true
					work = append(work, s)
				}
			}
		}
	}
	return sin, have, fl
}

// analyzeFunctionAbs ports analyze_function_abs(f, S, mw_ind).
func (p *Program2) analyzeFunctionAbs(f *Func) *FA {
	fa := &FA{F: f}
	nodes := f.Nodes
	tr := &track{all: true}
	var sin []State
	var have []bool
	var fl *absFlags
	var saveonlyKeys map[int]struct{}
	wext := 0
	converged := false
	for round := 0; round < 6; round++ {
		sin, have, fl = p.runAbs(f, tr)
		// classify accesses
		acc := map[int]*accEnt{}
		type rng struct{ lo, hi int }
		var partial []rng
		wext = 0
		for _, n := range nodes {
			if (n.Kind != NN && n.Kind != NSlot) || !have[n.ID] {
				continue
			}
			mr, ok := memrefOf(n.Insn)
			if !ok {
				continue
			}
			st := &sin[n.ID]
			bv := st.get(mr.base)
			if bv.K != 's' {
				continue
			}
			key := int(bv.N) + mr.imm
			if key+mr.size > 0 && key+mr.size > wext {
				wext = key + mr.size
			}
			if mr.cls == 'w' && n.Insn.Kind != KExtra {
				a := acc[key]
				if a == nil {
					a = &accEnt{saveonly: true}
					acc[key] = a
				}
				if mr.store {
					a.stores++
					var v Val
					if n.Insn.Kind == KSw {
						v = st.get(int(n.Insn.Rt))
					} else {
						v = st.get(F(int(n.Insn.Rt)))
					}
					if !(v != valT && v.K == 'o' && (isCalleeSaved[v.N] || v.N == RA) && v.N != 0) {
						a.saveonly = false
					}
				} else {
					a.loads++
				}
				if key&3 != 0 {
					a.bad = true
				}
			} else {
				lo := key
				if mr.cls == 'u' {
					lo -= 3
				}
				partial = append(partial, rng{lo, key + mr.size})
			}
		}
		keys := make([]int, 0, len(acc))
		for k := range acc {
			keys = append(keys, k)
		}
		sort.Ints(keys)
		P := map[int]struct{}{}
		for ki, key := range keys {
			a := acc[key]
			if key >= 0 || a.bad || a.loads == 0 || a.stores == 0 {
				continue
			}
			if _, b := fl.bad[key]; b {
				continue
			}
			if fl.escape && !a.saveonly {
				continue
			}
			skip := false
			for _, r := range partial {
				if r.lo < key+4 && r.hi > key {
					skip = true
					break
				}
			}
			if skip {
				continue
			}
			if ki > 0 && keys[ki-1]+4 > key {
				continue
			}
			if ki+1 < len(keys) && keys[ki+1] < key+4 {
				continue
			}
			P[key] = struct{}{}
		}
		newtrack := P
		saveonlyKeys = map[int]struct{}{}
		for k := range P {
			if acc[k].saveonly {
				saveonlyKeys[k] = struct{}{}
			}
		}
		if !tr.all {
			nt := map[int]struct{}{}
			for k := range newtrack {
				if _, ok := tr.set[k]; ok {
					nt[k] = struct{}{}
				}
			}
			newtrack = nt
			if len(nt) == len(tr.set) { // newtrack is a subset of track: equal sizes mean equal sets
				converged = true
				break
			}
		}
		tr = &track{set: newtrack}
	}
	if !converged {
		// did not stabilise: give up promotion altogether (sound: untracked loads are T)
		tr = &track{set: map[int]struct{}{}}
		saveonlyKeys = map[int]struct{}{}
		sin, have, fl = p.runAbs(f, tr)
		fl.escape, fl.escapeUp = true, true
	}
	fa.Escape = fl.escape
	if fl.escapeUp {
		fa.W = INF
	} else {
		fa.W = wext
	}
	fa.Tracked = tr.set
	// pinned keys: slots a callee may read through its incoming-argument area
	pinnedAll := false
	type rng struct{ lo, hi int }
	var pinRanges []rng
	for _, n := range nodes {
		if n.Kind != NCont || !have[n.ID] {
			continue
		}
		sv := sin[n.ID].get(SP)
		var W int
		if n.Insn.Mn == "jalr" {
			W = p.WInd
		} else if _, imp := p.Imports[uint32(n.Tgt)]; n.hasTgt() && imp {
			W = 0
		} else {
			W = p.WOf(uint32(n.Tgt))
		}
		if W == 0 {
			continue
		}
		if sv.K != 's' {
			pinnedAll = true
			continue
		}
		pinRanges = append(pinRanges, rng{int(sv.N), int(sv.N) + W})
	}
	promoted := make([]int, 0, len(tr.set))
	for key := range tr.set {
		if _, so := saveonlyKeys[key]; so {
			promoted = append(promoted, key) // callee-saved / ra save slots are never an argument area
			continue
		}
		if pinnedAll {
			continue
		}
		hit := false
		for _, r := range pinRanges {
			if r.lo <= key+3 && key < r.hi {
				hit = true
				break
			}
		}
		if hit {
			continue
		}
		promoted = append(promoted, key)
	}
	sort.Ints(promoted)
	fa.Promoted = make(map[int]int, len(promoted))
	for i, key := range promoted {
		fa.Promoted[key] = i
	}
	var mod Mask
	fa.Mem = make([]MemInfo, len(nodes))
	for _, n := range nodes {
		if !have[n.ID] {
			continue
		}
		st := &sin[n.ID]
		switch n.Kind {
		case NN, NSlot:
			mr, ok := memrefOf(n.Insn)
			if ok {
				bv := st.get(mr.base)
				if bv.K == 's' {
					key := int(bv.N) + mr.imm
					slot := -1
					if mr.cls == 'w' && n.Insn.Kind != KExtra {
						if i, ok := fa.Promoted[key]; ok {
							slot = i
						}
					}
					fa.Mem[n.ID] = MemInfo{int32(key), int16(slot), true}
				}
			}
		case NRet, NTail:
			if n.Kind == NRet {
				rv := st.get(int(n.Insn.Rs))
				if rv != (Val{'o', RA}) {
					fa.Reasons = append(fa.Reasons, fmt.Sprintf("return through non-ra value at %08x", n.PC))
				}
			}
			sv := st.get(SP)
			if sv != (Val{'s', 0}) {
				if sv == valT {
					fa.SpDynamic = true
				}
				kind := "ret"
				if n.Kind == NTail {
					kind = "tail"
				}
				fa.Reasons = append(fa.Reasons, fmt.Sprintf("sp not balanced at %s %08x", kind, n.PC))
			}
			for r := 1; r < NRES; r++ {
				if r == SP || r == RA {
					continue
				}
				if st.get(r) != (Val{'o', int32(r)}) {
					mod = mod.Or(bitMask(r))
				}
			}
			if st.get(RA) != (Val{'o', RA}) && n.Kind == NTail {
				fa.Reasons = append(fa.Reasons, fmt.Sprintf("ra not restored at tail %08x", n.PC))
			}
			if n.Kind == NTail {
				mod = mod.Or(p.MwOf(uint32(n.Tgt)))
				if w := p.WOf(uint32(n.Tgt)); w > fa.W {
					fa.W = w
				}
			}
		}
	}
	fa.Mod = mod
	// va_start pattern: the prologue spills several incoming argument registers (a2..a7) with their entry values
	spilled := 0
	var seen [12]bool
	for _, n := range nodes {
		if (n.Kind != NN && n.Kind != NSlot) || !have[n.ID] {
			continue
		}
		d := n.Insn
		if d.Kind == KSw && d.Rt >= 6 && d.Rt <= 11 {
			if sin[n.ID].get(int(d.Rt)) == (Val{'o', int32(d.Rt)}) && !seen[d.Rt] {
				seen[d.Rt] = true
				spilled++
			}
		}
	}
	fa.Variadic = spilled >= 4
	return fa
}

// ================================================================================================= program level

// Summary is the per-function whole-program result.
type Summary struct {
	Entry    uint32
	F        *Func
	FA       *FA
	W        int
	Variadic bool
	Aw       Mask
	Mw       Mask
	A        Mask
	R        Mask
	Typed    bool
	Reasons  []string
	Size     int
	RetUse   Mask
	At       bool
	Why      []string
}

// Program2 is the container for the whole-program analysis results.
type Program2 struct {
	Prog     *Program
	Im       *Image
	S        map[uint32]*Summary
	Imports  map[uint32]Import
	MwInd    Mask
	WInd     int
	Callers  map[uint32][]uint32 // sorted
	JalrFunc []uint32            // sorted
	Keys     []uint32            // sorted entries of S
}

// MwOf ports mw_of().
func (p *Program2) MwOf(t uint32) Mask {
	if _, ok := p.Imports[t]; ok {
		return importMW
	}
	s := p.S[t]
	if s == nil {
		return resMask.AndNot(bitMask(SP))
	}
	return s.Mw
}

// WOf ports W_of().
func (p *Program2) WOf(t uint32) int {
	s := p.S[t]
	if s == nil {
		return INF
	}
	return s.W
}

// NewProgram2 ports Program2.__init__.
func NewProgram2(prog *Program) *Program2 {
	p := &Program2{Prog: prog, Im: prog.Im, S: map[uint32]*Summary{}, Imports: prog.Im.Imports, Callers: map[uint32][]uint32{}}
	p.MwInd = importMW.Or(callerSaved).Or(bitMask(1))
	for e, f := range prog.Funcs {
		p.S[e] = &Summary{Entry: e, F: f, R: bitMask(V0), Typed: true, Size: f.SizeWords}
	}
	p.Keys = make([]uint32, 0, len(p.S))
	for e := range p.S {
		p.Keys = append(p.Keys, e)
	}
	sort.Slice(p.Keys, func(i, j int) bool { return p.Keys[i] < p.Keys[j] })
	for _, e := range p.Keys {
		_, h := prog.Hilo[e]
		_, d := prog.Data32[e]
		if h || d || e == prog.Im.Entry {
			p.S[e].At = true
		}
	}
	for _, e := range p.Keys { // ascending, so every callers list is sorted
		f := prog.Funcs[e]
		for t := range f.Callees {
			p.Callers[t] = append(p.Callers[t], e)
		}
		if f.HasIndCall {
			p.JalrFunc = append(p.JalrFunc, e)
		}
	}
	return p
}

// postorder ports postorder(P): callee-first order of function entries (iterative DFS over direct call edges).
func (p *Program2) postorder() []uint32 {
	seen := map[uint32]struct{}{}
	var order []uint32
	type frame struct {
		e    uint32
		cs   []uint32
		next int
	}
	for _, root := range p.Keys {
		if _, ok := seen[root]; ok {
			continue
		}
		stack := []frame{{root, p.Prog.Funcs[root].sortedCallees(), 0}}
		seen[root] = struct{}{}
		for len(stack) > 0 {
			fr := &stack[len(stack)-1]
			pushed := false
			for fr.next < len(fr.cs) {
				t := fr.cs[fr.next]
				fr.next++
				if _, ok := p.S[t]; ok {
					if _, sn := seen[t]; !sn {
						seen[t] = struct{}{}
						stack = append(stack, frame{t, p.Prog.Funcs[t].sortedCallees(), 0})
						pushed = true
						break
					}
				}
			}
			if !pushed {
				order = append(order, fr.e)
				stack = stack[:len(stack)-1]
			}
		}
	}
	return order
}

// queue is a FIFO of function entries with a membership set (collections.deque + inq).
type queue struct {
	items []uint32
	head  int
	in    map[uint32]struct{}
}

func newQueue(order []uint32) *queue {
	q := &queue{items: append([]uint32(nil), order...), in: make(map[uint32]struct{}, len(order))}
	for _, e := range order {
		q.in[e] = struct{}{}
	}
	return q
}
func (q *queue) empty() bool { return q.head >= len(q.items) }
func (q *queue) pop() uint32 {
	e := q.items[q.head]
	q.head++
	delete(q.in, e)
	if q.head > 4096 && q.head*2 > len(q.items) {
		q.items = append([]uint32(nil), q.items[q.head:]...)
		q.head = 0
	}
	return e
}
func (q *queue) push(e uint32) {
	if _, ok := q.in[e]; !ok {
		q.in[e] = struct{}{}
		q.items = append(q.items, e)
	}
}
func (q *queue) size() int { return len(q.items) - q.head }

// Phase1 ports phase1(): MW fixpoint over the call graph (abstract interpretation of every function).
func (p *Program2) Phase1(progress func(done, total int)) int {
	order := p.postorder()
	q := newQueue(order)
	nRuns := 0
	for !q.empty() {
		e := q.pop()
		s := p.S[e]
		fa := p.analyzeFunctionAbs(s.F)
		nRuns++
		old := s.Mw
		s.FA = fa
		s.Reasons = append([]string(nil), fa.Reasons...)
		s.Variadic = fa.Variadic
		nw := old.Or(fa.Mod)
		if fa.W > s.W {
			s.W = fa.W
			for _, c := range p.Callers[e] {
				q.push(c)
			}
			if s.At && s.W > p.WInd {
				p.WInd = s.W
				for _, c := range p.JalrFunc {
					q.push(c)
				}
			}
		}
		if nw != old {
			s.Mw = nw
			for _, c := range p.Callers[e] {
				q.push(c)
			}
		}
		if progress != nil && nRuns%256 == 0 {
			progress(len(order)-q.size(), len(order))
		}
	}
	return nRuns
}
