// discover.go mirrors recomp/leangen/discover.py (whole-program function discovery) and the discovery half of
// recomp/leangen/pipeline.py.
package leangen

import "sort"

// prologueSeeds ports prologue_seeds(): gcc-style prologues `addiu sp, sp, -N` right after a `jr ra` + delay slot (+ nop padding)
// or a `j` + slot.
func prologueSeeds(im *Image) map[uint32]struct{} {
	W := im.Words
	out := map[uint32]struct{}{}
	for i := 2; i < len(W)-2; i++ {
		w := W[i]
		if (w>>26) == 9 && ((w>>16)&31) == 29 && ((w>>21)&31) == 29 && (w&0x8000) != 0 {
			j := i - 1
			for j > 0 && W[j] == 0 {
				j--
			}
			if j >= 1 && (W[j-1] == 0x03E00008 || W[j] == 0x03E00008 || (W[j-1]>>26) == 2) {
				out[Base+uint32(4*i)] = struct{}{}
			}
		}
	}
	return out
}

func sortedSet(m map[uint32]struct{}) []uint32 {
	out := make([]uint32, 0, len(m))
	for k := range m {
		out = append(out, k)
	}
	sort.Slice(out, func(i, j int) bool { return out[i] < out[j] })
	return out
}

// Discover ports discover().
func Discover(im *Image, profile *Profile) *Program {
	prog := NewProgram(im)
	prog.Profile = profile
	for _, a := range profile.Noreturn {
		prog.Noreturn[a] = struct{}{}
	}
	prog.Starts[im.Entry] = struct{}{}
	cp := im.CodePointers()
	hilo := map[uint32]struct{}{}
	data32 := map[uint32]struct{}{}
	for a, k := range cp {
		if k&ptrHilo != 0 {
			hilo[a] = struct{}{}
		}
		if k&(ptrData32|ptrText32) != 0 {
			data32[a] = struct{}{}
		}
	}
	for a := range hilo {
		prog.Starts[a] = struct{}{}
	}
	// R_MIPS_26 jal targets declared by relocation + decoded jal instructions
	for _, r := range im.Relocs {
		if r.Typ == 4 && r.Addr < im.StubEnd {
			w := im.Word(r.Addr)
			if (w >> 26) == 3 {
				tg := (Base & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
				if im.InCode(tg) {
					prog.Starts[tg] = struct{}{}
					prog.StartReason[tg] = struct{}{}
				}
			}
		}
	}
	seeds := prologueSeeds(im)
	for a := range seeds {
		prog.Starts[a] = struct{}{}
	}
	prog.Starts[Base] = struct{}{}
	// imports are not functions to translate
	for a := range im.Imports {
		delete(prog.Starts, a)
	}
	prog.Data32 = data32
	prog.Hilo = hilo
	prog.StartReason[im.Entry] = struct{}{}
	for a := range hilo {
		prog.StartReason[a] = struct{}{}
	}
	for a := range seeds {
		prog.StartReason[a] = struct{}{}
	}
	prog.StartReason[Base] = struct{}{}
	return prog
}

// BuildAll ports build_all(): build CFGs for every start; iterate until no new function starts (tail targets, jal targets,
// unclaimed pointers).
func BuildAll(prog *Program) map[uint32]*Func {
	im := prog.Im
	built := map[uint32]*Func{}
	pending := map[uint32]struct{}{}
	for a := range prog.Data32 {
		pending[a] = struct{}{}
	}
	for {
		var newS []uint32
		for _, s := range sortedSet(prog.Starts) {
			if _, ok := built[s]; !ok && !im.IsStub(s) {
				newS = append(newS, s)
			}
		}
		if len(newS) == 0 && len(pending) == 0 {
			break
		}
		prog.Funcs = built
		for _, s := range newS {
			built[s] = prog.BuildFunction(s)
		}
		// jump-table claimed words
		claimed := map[uint32]struct{}{}
		for _, f := range built {
			for _, tb := range f.Tables {
				for _, e := range tb.Entries {
					claimed[e] = struct{}{}
				}
			}
		}
		// data32 pointers not claimed by any table become function starts
		for _, p := range sortedSet(pending) {
			if _, ok := claimed[p]; ok {
				continue
			}
			if _, ok := prog.Starts[p]; ok || im.IsStub(p) {
				continue
			}
			prog.Starts[p] = struct{}{}
		}
		pending = map[uint32]struct{}{}
		// newly seen jal targets
		for _, t := range sortedSet(prog.JalTargets) {
			if _, ok := prog.Starts[t]; !ok && !im.IsStub(t) {
				prog.Starts[t] = struct{}{}
			}
		}
	}
	prog.Funcs = built
	// rebuild functions that now contain another start inside their body (those edges must become tail edges)
	for it := 0; it < 4; it++ {
		var redo []uint32
		for _, s := range sortedFuncKeys(built) {
			f := built[s]
			for _, pc := range f.PCs {
				if pc == s {
					continue
				}
				if _, ok := prog.Starts[pc]; ok {
					redo = append(redo, s)
					break
				}
			}
		}
		if len(redo) == 0 {
			break
		}
		for _, s := range redo {
			built[s] = prog.BuildFunction(s)
		}
		prog.Funcs = built
		// new starts can appear again
		for _, s := range sortedSet(prog.JalTargets) {
			if _, ok := prog.Starts[s]; !ok && !im.IsStub(s) {
				prog.Starts[s] = struct{}{}
			}
		}
		for _, s := range sortedSet(prog.Starts) {
			if _, ok := built[s]; !ok && !im.IsStub(s) {
				built[s] = prog.BuildFunction(s)
			}
		}
		prog.Funcs = built
	}
	pruneTableLabels(prog, built)
	inferNoreturn(prog, built)
	return built
}

func sortedFuncKeys(m map[uint32]*Func) []uint32 {
	out := make([]uint32, 0, len(m))
	for k := range m {
		out = append(out, k)
	}
	sort.Slice(out, func(i, j int) bool { return out[i] < out[j] })
	return out
}

// returnsSomewhere: does any ret / tail node stay reachable when calls to noreturn functions end their path?
func returnsSomewhere(f *Func, noret map[uint32]struct{}) bool {
	nodes := f.Nodes
	seen := make([]bool, len(nodes))
	st := []int{f.EntryID}
	seen[st[0]] = true
	for len(st) > 0 {
		i := st[len(st)-1]
		st = st[:len(st)-1]
		n := nodes[i]
		if n.Kind == NRet {
			return true
		}
		if n.Kind == NTail {
			if _, nr := noret[uint32(n.Tgt)]; !nr {
				return true
			}
		}
		if n.Kind == NCont && n.hasTgt() {
			if _, nr := noret[uint32(n.Tgt)]; nr {
				continue
			}
		}
		for _, s := range n.Succ {
			if !seen[s] {
				seen[s] = true
				st = append(st, s)
			}
		}
	}
	return false
}

// inferNoreturn: functions with no reachable return never come back to the caller; the code after a call to them is dead.
func inferNoreturn(prog *Program, built map[uint32]*Func) {
	noret := map[uint32]struct{}{}
	for e := range prog.Noreturn {
		noret[e] = struct{}{}
	}
	for it := 0; it < 8; it++ {
		var newS []uint32
		for e, f := range built {
			if _, ok := noret[e]; !ok && !returnsSomewhere(f, noret) {
				newS = append(newS, e)
			}
		}
		if len(newS) == 0 {
			break
		}
		for _, e := range newS {
			noret[e] = struct{}{}
		}
	}
	prog.Noreturn = noret
	var redo []uint32
	for _, e := range sortedFuncKeys(built) {
		for _, n := range built[e].Nodes {
			if n.Kind == NCont && n.hasTgt() && !n.Noret {
				if _, ok := noret[uint32(n.Tgt)]; ok {
					redo = append(redo, e)
					break
				}
			}
		}
	}
	for _, e := range redo {
		built[e] = prog.BuildFunction(e)
	}
	prog.Funcs = built
}

func tableWordSet(built map[uint32]*Func) map[uint32]struct{} {
	tw := map[uint32]struct{}{}
	for _, f := range built {
		for _, tb := range f.Tables {
			for i := range tb.Entries {
				tw[tb.Table+uint32(4*i)] = struct{}{}
			}
		}
	}
	return tw
}

// pruneTableLabels: starts that exist only because jump-table words (R_MIPS_32) point at them are case labels, not functions.
func pruneTableLabels(prog *Program, built map[uint32]*Func) {
	im := prog.Im
	for it := 0; it < 6; it++ {
		tw := tableWordSet(built)
		var drop []uint32
		for _, s := range sortedSet(prog.Starts) {
			if _, ok := built[s]; !ok {
				continue
			}
			if _, r := prog.StartReason[s]; r {
				continue
			}
			if _, j := prog.JalTargets[s]; j {
				continue
			}
			srcs := im.PtrSrc[s]
			if len(srcs) > 0 {
				all := true
				for _, a := range srcs {
					if _, ok := tw[a]; !ok {
						all = false
						break
					}
				}
				if all {
					drop = append(drop, s)
				}
			}
		}
		if len(drop) == 0 {
			break
		}
		dropset := map[uint32]struct{}{}
		for _, s := range drop {
			delete(prog.Starts, s)
			delete(built, s)
			dropset[s] = struct{}{}
		}
		var redo []uint32
		for _, e := range sortedFuncKeys(built) {
			for c := range built[e].Callees {
				if _, ok := dropset[c]; ok {
					redo = append(redo, e)
					break
				}
			}
		}
		for _, e := range redo {
			built[e] = prog.BuildFunction(e)
		}
		prog.Funcs = built
	}
}
