// fuse.go mirrors recomp/leangen/fuse.py: fusion of unaligned access pairs.
//
// `lwr rt,o(b) ... lwl rt,o+3(b)` (either order) is one unaligned word load; `swr/swl` likewise one unaligned store. The
// interpreter executes the halves separately. Fusing is valid when the second half is reached from the first along a single
// straight-line chain of plain instructions (each with exactly one predecessor) that neither read nor write `rt`, nor write the
// base `b`, and that cannot change the memory the pair touches (no stores between a load pair; no memory access at all between a
// store pair), and rt != base.
//
// For loads the data flow changes: the pair reads only `b` and defines `rt` (the halves' mutual read of `rt` is not a real
// input). The first half becomes a no-op and the second half performs the whole access.
// Marks Insn.Fuse (1 = first half, 2 = second half, which does the work).
package leangen

import "sort"

const fuseWindow = 10

func isLoadHalf(k Kind) bool  { return k == KLwl || k == KLwr }
func isStoreHalf(k Kind) bool { return k == KSwl || k == KSwr }

type fusePair struct{ d, e *Insn }

// ApplyFuse ports apply(): returns the number of fused pairs.
func ApplyFuse(prog *Program) int {
	cand := map[uint32]fusePair{}
	bad := map[uint32]struct{}{}
	for _, fe := range sortedFuncKeys(prog.Funcs) {
		f := prog.Funcs[fe]
		nodes := f.Nodes
		for _, n := range nodes {
			d := n.Insn
			if d == nil || !(isLoadHalf(d.Kind) || isStoreHalf(d.Kind)) {
				continue
			}
			if n.Kind != NN || n.Key.T != 0 {
				bad[d.PC] = struct{}{}
				continue
			}
			isLoad := isLoadHalf(d.Kind)
			cur := n
			var found *Insn
			for i := 0; i < fuseWindow; i++ {
				if len(cur.Succ) != 1 {
					break
				}
				nx := nodes[cur.Succ[0]]
				if nx.Kind != NN || nx.Key.T != 0 || nx.Insn == nil || !(len(nx.Pred) == 1 && nx.Pred[0] == cur.ID) {
					break
				}
				e := nx.Insn
				same := (isLoad && isLoadHalf(e.Kind)) || (!isLoad && isStoreHalf(e.Kind))
				if same && e.Kind != d.Kind && e.Rt == d.Rt && e.Rs == d.Rs {
					found = e
					break
				}
				// intermediate instruction must be transparent
				if e.Ctl || e.Kind == KBreak || e.Kind == KSyscall {
					break
				}
				if e.Uses.Has(int(d.Rt)) || e.Defs.Has(int(d.Rt)) || e.Defs.Has(int(d.Rs)) {
					break
				}
				if e.Kind == KLwl || e.Kind == KLwr || e.Kind == KSwl || e.Kind == KSwr {
					break
				}
				mk := e.Mem
				if mk == memSt || (!isLoad && mk != memNone) {
					break
				}
				if e.Vfpu && !isLoad {
					break
				}
				cur = nx
			}
			if found == nil {
				bad[d.PC] = struct{}{}
				continue
			}
			e := found
			if d.Rt == 0 || d.Rs == d.Rt {
				bad[d.PC] = struct{}{}
				continue
			}
			lr := e
			if d.Kind == KLwr || d.Kind == KSwr {
				lr = d
			}
			ll := d
			if lr == d {
				ll = e
			}
			if ll.Simm != lr.Simm+3 {
				bad[d.PC] = struct{}{}
				continue
			}
			if prev, ok := cand[d.PC]; ok && prev.e != e {
				bad[d.PC] = struct{}{}
			}
			cand[d.PC] = fusePair{d, e}
		}
	}
	var pairs []fusePair
	used := map[uint32]struct{}{}
	pcs := make([]uint32, 0, len(cand))
	for pc := range cand {
		pcs = append(pcs, pc)
	}
	sort.Slice(pcs, func(i, j int) bool { return pcs[i] < pcs[j] })
	for _, pc := range pcs {
		if _, b := bad[pc]; b {
			continue
		}
		c := cand[pc]
		d, e := c.d, c.e
		if _, u := used[pc]; u {
			continue
		}
		if _, u := used[e.PC]; u {
			continue
		}
		// the second half must not itself be a first half of another pair
		if _, isCand := cand[e.PC]; isCand {
			if _, b := bad[e.PC]; !b {
				continue
			}
		}
		used[pc] = struct{}{}
		used[e.PC] = struct{}{}
		pairs = append(pairs, fusePair{d, e})
	}
	for _, p := range pairs {
		d, e := p.d, p.e
		d.Fuse = 1
		e.Fuse = 2
		if isLoadHalf(d.Kind) {
			d.Uses = RegSet{}
			d.Defs = RegSet{}
			e.Uses = mkSet(int(e.Rs))
			e.Defs = mkSet(int(e.Rt))
		} else {
			d.Uses = RegSet{}
		}
	}
	return len(pairs)
}
