// cfg.go mirrors recomp/leangen/cfg.py: per-function control-flow graphs with delay slots, plus jump table recovery.
//
// Node model (one node per executed 'instruction instance'):
//
//	kind 'n'     plain instruction                      key pc
//	kind 'br'    conditional branch decision            key pc        succ = [slot]            (non-likely branch)
//	                                                                  succ = [likely-slot, fall]   (likely branch)
//	kind 'jmp'   j  (intra-function)                    key pc        succ = [slot]
//	kind 'call'  jal/jalr call + slot                   key pc        succ = [slot];  slot -> ('C',pc) -> pc+8
//	kind 'jr'    jr rX (return / switch / indirect)     key pc        succ = [slot]
//	kind 'slot'  delay-slot instance                    key ('S',pc) or ('L',pc) (likely)
//	kind 'cont'  call completion                        key ('C',pc)  succ = [pc+8]
//	kind 'ret'   return after delay slot                key ('R',pc)
//	kind 'tail'  tail call after delay slot / fallthrough into another function   key ('T',pc)
//	kind 'switch' jump-table dispatch after delay slot  key ('W',pc)
//	kind 'ijmp'  unresolved indirect jump after slot    key ('J',pc)
//	kind 'fatal' break / undecodable / runs off         key pc
package leangen

import (
	"fmt"
	"sort"
)

// NodeKind is the kind of a CFG node.
type NodeKind uint8

// Node kinds.
const (
	NN NodeKind = iota
	NBr
	NJmp
	NCall
	NJr
	NSlot
	NCont
	NRet
	NTail
	NSwitch
	NIjmp
	NFatal
)

// Key identifies a node inside a function: t == 0 is a plain instruction address, otherwise one of 'S','L','C','R','T','W','J'.
type Key struct {
	T  byte
	PC uint32
}

// JumpTable is a recovered jump table (the 'data' dict of a switch node).
type JumpTable struct {
	Table   uint32
	Bound   int64 // -1: None
	IdxReg  int
	JrPC    uint32
	LwPC    uint32
	SllPC   uint32
	Entries []uint32
	Idx     bool // mode == 'idx' (False: 'val')
	Shifted bool
}

// Node is one CFG node.
type Node struct {
	ID    int
	Key   Key
	PC    uint32
	Kind  NodeKind
	Const Tri  // br: statically known outcome
	Noret bool // cont: call to a noreturn function
	Insn  *Insn
	Succ  []int
	Pred  []int
	Tgt   int64 // -1: None
	x     *nodeX
}

// nodeX holds what only a few nodes carry: the reason of a fatal / unresolved node and the jump table of a switch.
type nodeX struct {
	str   string
	table *JumpTable
}

func (n *Node) setStr(s string) {
	if n.x == nil {
		n.x = &nodeX{}
	}
	n.x.str = s
}

func (n *Node) setTable(t *JumpTable) {
	if n.x == nil {
		n.x = &nodeX{}
	}
	n.x.table = t
}

// Str is the 'data' string of a fatal / unresolved indirect jump node.
func (n *Node) Str() string {
	if n.x == nil {
		return ""
	}
	return n.x.str
}

// Table is the jump table of a switch node.
func (n *Node) Table() *JumpTable {
	if n.x == nil {
		return nil
	}
	return n.x.table
}

func (n *Node) hasTgt() bool { return n.Tgt >= 0 }

// Func is the control-flow graph of one function.
type Func struct {
	Entry      uint32
	Nodes      []*Node
	EntryID    int
	Issues     []string
	Tables     []*JumpTable
	PCs        []uint32 // sorted: all instruction addresses belonging to the function
	Callees    map[uint32]struct{}
	HasIndCall bool
	SizeWords  int
}

// HasPC reports whether pc is an instruction address of the function.
func (f *Func) HasPC(pc uint32) bool {
	i := sort.Search(len(f.PCs), func(i int) bool { return f.PCs[i] >= pc })
	return i < len(f.PCs) && f.PCs[i] == pc
}

func (f *Func) sortedCallees() []uint32 {
	out := make([]uint32, 0, len(f.Callees))
	for c := range f.Callees {
		out = append(out, c)
	}
	sort.Slice(out, func(i, j int) bool { return out[i] < out[j] })
	return out
}

// Program is the whole program under discovery.
type Program struct {
	Im           *Image
	insnCache    []*Insn
	Funcs        map[uint32]*Func
	Starts       map[uint32]struct{}
	StartReason  map[uint32]struct{} // starts with at least one recorded reason
	JalTargets   map[uint32]struct{}
	Noreturn     map[uint32]struct{}
	Reloc32      map[uint32]struct{}
	Profile      *Profile
	Data32, Hilo map[uint32]struct{}
}

// NewProgram ports Program.__init__.
func NewProgram(im *Image) *Program {
	p := &Program{Im: im}
	p.insnCache = make([]*Insn, im.NWords)
	p.Funcs = map[uint32]*Func{}
	p.Starts = map[uint32]struct{}{}
	p.StartReason = map[uint32]struct{}{}
	p.JalTargets = map[uint32]struct{}{}
	p.Noreturn = map[uint32]struct{}{}
	p.Reloc32 = map[uint32]struct{}{}
	for _, r := range im.Relocs {
		if r.Typ == 2 {
			p.Reloc32[r.Addr] = struct{}{}
		}
	}
	return p
}

// Ins returns the decoded instruction at pc (nil outside the code range or when misaligned).
func (p *Program) Ins(pc uint32) *Insn {
	if !(Base <= pc && pc < p.Im.StubEnd) || pc&3 != 0 {
		return nil
	}
	i := (pc - Base) >> 2
	d := p.insnCache[i]
	if d == nil {
		d = Decode(pc, p.Im.Words[i])
		p.insnCache[i] = d
	}
	return d
}

func (p *Program) sortedFuncs() []uint32 {
	out := make([]uint32, 0, len(p.Funcs))
	for e := range p.Funcs {
		out = append(out, e)
	}
	sort.Slice(out, func(i, j int) bool { return out[i] < out[j] })
	return out
}

// ---------------------------------------------------------------- jump table recovery

// findDef searches backwards (straight-line, same basic block approximation) for the instruction that defines reg before pc.
func (p *Program) findDef(pc uint32, reg int, limit int) *Insn {
	q := pc - 4
	for i := 0; i < limit; i++ {
		d := p.Ins(q)
		if d == nil {
			return nil
		}
		pd := p.Ins(q - 4)
		if d.Defs.Has(reg) && !d.Ctl {
			return d
		}
		if pd != nil && pd.Delay {
			if d.Defs.Has(reg) {
				return d
			}
			return nil
		}
		q -= 4
	}
	return nil
}

// constOf ports _const_of(): compile-time constant held by reg just before pc (lui / addiu / ori chains).
func (p *Program) constOf(pc uint32, reg int, depth int) (uint32, bool) {
	if reg == 0 {
		return 0, true
	}
	if depth > 4 {
		return 0, false
	}
	d := p.findDef(pc, reg, 24)
	if d == nil {
		return 0, false
	}
	switch d.Kind {
	case KLui:
		return d.Imm << 16, true
	case KAddiu:
		c, ok := p.constOf(d.PC, int(d.Rs), depth+1)
		if !ok {
			return 0, false
		}
		return c + uint32(d.Simm), true
	case KOri:
		c, ok := p.constOf(d.PC, int(d.Rs), depth+1)
		if !ok {
			return 0, false
		}
		return c | d.Imm, true
	case KOr, KAddu:
		if d.Rt == 0 {
			return p.constOf(d.PC, int(d.Rs), depth+1)
		}
	}
	return 0, false
}

func (p *Program) scaledIndex(pc uint32, reg int) (idxReg int, sllPC uint32, ok bool) {
	d := p.findDef(pc, reg, 24)
	if d != nil && d.Kind == KSll && d.Sa == 2 {
		return int(d.Rt), d.PC, true
	}
	return 0, 0, false
}

// recoverJumpTable ports recover_jump_table(): pc is the address of `jr rX` (rX != ra).
func (p *Program) recoverJumpTable(pc uint32, d *Insn) *JumpTable {
	rs := int(d.Rs)
	ld := p.findDef(pc, rs, 14)
	if ld == nil || ld.Kind != KLw {
		return nil
	}
	base := int(ld.Rs)
	off := ld.Simm
	var tbl uint32
	var idx int
	var sllPC uint32
	have := false
	ad := p.findDef(ld.PC, base, 14)
	if ad != nil && (ad.Kind == KAddu || ad.Kind == KAdd) {
		pairs := [2][2]int{{int(ad.Rs), int(ad.Rt)}, {int(ad.Rt), int(ad.Rs)}}
		for _, pr := range pairs {
			c, cok := p.constOf(ad.PC, pr[0], 0)
			ir, sp, sok := p.scaledIndex(ad.PC, pr[1])
			if cok && sok {
				tbl = c + uint32(off)
				idx, sllPC = ir, sp
				have = true
				break
			}
		}
	}
	if !have {
		return nil
	}
	bound := int64(-1)
	q := sllPC - 4
	for i := 0; i < 28; i++ {
		qi := p.Ins(q)
		if qi == nil {
			break
		}
		if qi.Kind == KSltiu && int(qi.Rs) == idx {
			bound = int64(uint32(qi.Simm))
			break
		}
		if qi.Defs.Has(idx) && !qi.Ctl {
			if ((qi.Kind == KOr || qi.Kind == KAddu) && qi.Rt == 0) || (qi.Kind == KAddiu && qi.Simm == 0) {
				idx = int(qi.Rs)
			} else {
				break
			}
		}
		q -= 4
	}
	return &JumpTable{Table: tbl, Bound: bound, IdxReg: idx, JrPC: pc, LwPC: ld.PC, SllPC: sllPC}
}

// switchMode decides how a jump table is lowered. 'idx': `switch (idx)` on the pre-scaling index register (the bound is known,
// equals the number of recovered entries and the register still holds the index at the jr, or holds idx << 2 when the sll is in
// place: Shifted). 'val': a value switch on the jr target register (always correct, slower).
func (p *Program) switchMode(t *JumpTable) {
	t.Idx = false
	t.Shifted = false
	if t.IdxReg == 0 || t.Bound < 0 || int64(len(t.Entries)) != t.Bound {
		return
	}
	pc := t.SllPC + 4
	for pc <= t.JrPC+4 {
		q := p.Ins(pc)
		if q == nil || q.Defs.Has(t.IdxReg) || q.Mn == "jal" || q.Mn == "jalr" {
			return
		}
		pc += 4
	}
	q0 := p.Ins(t.SllPC)
	if q0 == nil {
		return
	}
	t.Idx = true
	t.Shifted = q0.Defs.Has(t.IdxReg)
}

// tableEntries reads the jump table words. Entries must carry an R_MIPS_32 relocation and point into code.
// ok == false is Python's None (a bounded table with an entry that is not code).
func (p *Program) tableEntries(tbl uint32, bound int64) (ents []uint32, ok bool) {
	im := p.Im
	if !(Base <= tbl && int64(tbl) < int64(Base)+int64(im.ImgLen())) {
		return nil, true
	}
	n := int64(4096)
	if bound >= 0 {
		n = bound
	}
	for i := int64(0); i < n; i++ {
		a := tbl + uint32(4*i)
		if bound < 0 {
			if _, has := p.Reloc32[a]; !has {
				break
			}
		}
		v := im.Word(a)
		if !im.InCode(v) {
			if bound < 0 {
				break
			}
			return nil, false
		}
		ents = append(ents, v)
	}
	return ents, true
}

// ---------------------------------------------------------------- CFG construction

type pendingEdge struct {
	n  *Node
	pc uint32
}

// BuildFunction ports build_function().
func (p *Program) BuildFunction(entry uint32) *Func {
	f := &Func{Entry: entry, Callees: map[uint32]struct{}{}}
	starts := p.Starts
	byKey := map[Key]*Node{}
	var nodes []*Node

	mk := func(key Key, kind NodeKind, pc uint32, insn *Insn) *Node {
		if n := byKey[key]; n != nil {
			return n
		}
		n := &Node{ID: len(nodes), Key: key, Kind: kind, PC: pc, Insn: insn, Tgt: -1, Const: TriNone}
		nodes = append(nodes, n)
		byKey[key] = n
		return n
	}
	work := []uint32{entry}
	donePC := map[uint32]struct{}{}

	pcNode := func(pc uint32) *Node {
		if pc != entry {
			if _, ok := starts[pc]; ok {
				return mk(Key{'T', pc}, NTail, pc, nil)
			}
		}
		return nil
	}
	var pending []pendingEdge
	edgeTo := func(n *Node, pc uint32) {
		t := pcNode(pc)
		if t != nil {
			t.Tgt = int64(pc)
			f.Callees[pc] = struct{}{}
			n.Succ = append(n.Succ, t.ID)
		} else {
			work = append(work, pc)
			pending = append(pending, pendingEdge{n, pc})
		}
	}

	for len(work) > 0 {
		pc := work[len(work)-1]
		work = work[:len(work)-1]
		if _, ok := donePC[pc]; ok {
			continue
		}
		donePC[pc] = struct{}{}
		d := p.Ins(pc)
		if d == nil || d.Kind == KUnsupported {
			n := mk(Key{0, pc}, NFatal, pc, d)
			n.setStr("undecodable")
			continue
		}
		m := d.Mn
		if !d.Ctl {
			if d.Kind == KBreak {
				n := mk(Key{0, pc}, NFatal, pc, d)
				n.setStr("break")
				continue
			}
			if d.Kind == KSyscall {
				n := mk(Key{0, pc}, NFatal, pc, d)
				n.setStr("syscall")
				continue
			}
			n := mk(Key{0, pc}, NN, pc, d)
			edgeTo(n, pc+4)
			continue
		}
		// control instruction with delay slot
		sd := p.Ins(pc + 4)
		if sd == nil || sd.Kind == KUnsupported || sd.Ctl {
			n := mk(Key{0, pc}, NFatal, pc, d)
			n.setStr("bad delay slot")
			f.Issues = append(f.Issues, fmt.Sprintf("bad delay slot at %08x", pc))
			continue
		}
		if isBranch(m) {
			t := branchTarget(pc, d)
			bc := branchConst(d)
			n := mk(Key{0, pc}, NBr, pc, d)
			n.Tgt = int64(t)
			n.Const = bc
			if m == "bltzal" || m == "bgezal" || m == "bltzall" || m == "bgezall" {
				f.Issues = append(f.Issues, fmt.Sprintf("branch-and-link at %08x", pc))
			}
			if d.Likely {
				sl := mk(Key{'L', pc}, NSlot, pc+4, sd)
				edgeTo(sl, t)
				switch bc {
				case TriTrue:
					n.Succ = []int{sl.ID}
				case TriFalse:
					edgeTo(n, pc+8)
				default:
					n.Succ = []int{sl.ID}
					edgeTo(n, pc+8)
				}
			} else {
				sl := mk(Key{'S', pc}, NSlot, pc+4, sd)
				n.Succ = []int{sl.ID}
				switch bc {
				case TriTrue:
					edgeTo(sl, t)
				case TriFalse:
					edgeTo(sl, pc+8)
				default:
					edgeTo(sl, t)
					edgeTo(sl, pc+8)
				}
			}
			continue
		}
		if m == "j" {
			t := jumpTarget(pc, d)
			n := mk(Key{0, pc}, NJmp, pc, d)
			n.Tgt = int64(t)
			sl := mk(Key{'S', pc}, NSlot, pc+4, sd)
			n.Succ = []int{sl.ID}
			edgeTo(sl, t)
			continue
		}
		if m == "jal" || m == "jalr" {
			n := mk(Key{0, pc}, NCall, pc, d)
			sl := mk(Key{'S', pc}, NSlot, pc+4, sd)
			n.Succ = []int{sl.ID}
			c := mk(Key{'C', pc}, NCont, pc, d)
			sl.Succ = []int{c.ID}
			if m == "jal" {
				t := jumpTarget(pc, d)
				c.Tgt = int64(t)
				f.Callees[t] = struct{}{}
				p.JalTargets[t] = struct{}{}
				if _, nr := p.Noreturn[t]; nr {
					c.Noret = true
					continue
				}
			} else {
				f.HasIndCall = true
				if d.Rd != 31 {
					f.Issues = append(f.Issues, fmt.Sprintf("jalr with link reg %d at %08x", d.Rd, pc))
				}
			}
			edgeTo(c, pc+8)
			continue
		}
		if m == "jr" {
			n := mk(Key{0, pc}, NJr, pc, d)
			sl := mk(Key{'S', pc}, NSlot, pc+4, sd)
			n.Succ = []int{sl.ID}
			if d.Rs == 31 {
				r := mk(Key{'R', pc}, NRet, pc, d)
				sl.Succ = []int{r.ID}
			} else {
				jt := p.recoverJumpTable(pc, d)
				var ents []uint32
				if jt != nil {
					ents, _ = p.tableEntries(jt.Table, jt.Bound)
				}
				if jt != nil && len(ents) > 0 {
					w := mk(Key{'W', pc}, NSwitch, pc, d)
					sl.Succ = []int{w.ID}
					jt.Entries = ents
					w.setTable(jt)
					p.switchMode(jt)
					f.Tables = append(f.Tables, jt)
					seen := map[uint32]struct{}{}
					var uniq []uint32
					for _, e := range ents {
						if _, ok := seen[e]; !ok {
							seen[e] = struct{}{}
							uniq = append(uniq, e)
						}
					}
					for _, e := range uniq {
						edgeTo(w, e)
					}
				} else {
					j := mk(Key{'J', pc}, NIjmp, pc, d)
					sl.Succ = []int{j.ID}
					if jt == nil {
						j.setStr("no table")
					} else {
						j.setStr("empty table")
					}
				}
			}
			continue
		}
		panic("cfg: unexpected control instruction " + m)
	}
	for _, pe := range pending {
		t := byKey[Key{0, pe.pc}]
		if t == nil {
			panic(fmt.Sprintf("missing node %08x", pe.pc))
		}
		pe.n.Succ = append(pe.n.Succ, t.ID)
	}
	// prune nodes unreachable from the entry (annulled likely slots with constant branches etc.)
	seen := make([]bool, len(nodes))
	stack := []int{byKey[Key{0, entry}].ID}
	nseen := 0
	for len(stack) > 0 {
		i := stack[len(stack)-1]
		stack = stack[:len(stack)-1]
		if seen[i] {
			continue
		}
		seen[i] = true
		nseen++
		stack = append(stack, nodes[i].Succ...)
	}
	if nseen != len(nodes) {
		remap := make([]int, len(nodes))
		var kept []*Node
		for o, n := range nodes {
			if seen[o] {
				remap[o] = len(kept)
				kept = append(kept, n)
			}
		}
		for k, n := range kept {
			n.ID = k
			ns := n.Succ[:0:0]
			for _, x := range n.Succ {
				if seen[x] {
					ns = append(ns, remap[x])
				}
			}
			n.Succ = ns
		}
		nodes = kept
	}
	f.EntryID = 0
	for _, n := range nodes {
		if n.Key == (Key{0, entry}) {
			f.EntryID = n.ID
		}
	}
	for _, n := range nodes {
		for _, s := range n.Succ {
			nodes[s].Pred = append(nodes[s].Pred, n.ID)
		}
	}
	f.Nodes = nodes
	pcs := make([]uint32, 0, len(nodes))
	for _, n := range nodes {
		if n.Insn != nil && ((n.Key.T == 0) || n.Kind == NSlot) {
			pcs = append(pcs, n.PC)
		}
	}
	sort.Slice(pcs, func(i, j int) bool { return pcs[i] < pcs[j] })
	w := 0
	for i, x := range pcs {
		if i == 0 || x != pcs[i-1] {
			pcs[w] = x
			w++
		}
	}
	f.PCs = pcs[:w]
	f.SizeWords = len(f.PCs)
	return f
}
