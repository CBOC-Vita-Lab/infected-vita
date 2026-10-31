// vfpu.go mirrors recomp/leangen/vfpu.py: VFPU lowering, step 2: static tracking of the prefix registers and selection of the
// specialised forms.
//
// The three prefix registers (S, T, D = g_cpu.vfpu_ctrl[0..2]) are tracked per function over the node graph as a product lattice
// (constant | unknown) per register. At function entry and after every call the state is the architectural default
// (0xE4, 0xE4, 0): the guest compiler never leaves a prefix pending across a call or a return, which is checked here for the
// functions we can see (calls / tail calls / returns whose in-state is not the default are counted and listed in the manifest) and
// dynamically by LEAN_VFPU_ENTRY_CHECK in test builds.
//
// An instruction whose state is fully known and whose kind has a specialised form in lean_vfpu_fast.h is emitted as
// lean_vk::form<W, S, T, D>(args); everything else stays on the generic lean_vf:: helper, which reads the live registers.
package leangen

import "fmt"

// vstate is the (S, T, D) prefix state; a negative entry is TOP (unknown).
type vstate [3]int32

const vTop = -1

var vDef = vstate{0xE4, 0xE4, 0}

func vJoin(a, b vstate) vstate {
	var o vstate
	for i := range a {
		if a[i] == b[i] {
			o[i] = a[i]
		} else {
			o[i] = vTop
		}
	}
	return o
}

func isNoPrefix(k Kind) bool {
	switch k {
	case KLvs, KSvs, KLvq, KSvq, KMfv, KMtv:
		return true
	}
	return false
}

func isNoPrefixExtra(mn string) bool {
	switch mn {
	case "lvl.q", "lvr.q", "svl.q", "svr.q", "ll", "sc", "vmfvc":
		return true
	}
	return false
}

var fastState = map[Kind]string{KVfpuVec3: "vvec3", KVdot: "vdot", KVscl: "vscl", KVfpuHorizontal: "vhoriz", KVfpuCompare3: "vcmp3",
	KVfpuUnary: "vunary", KVidt: "vidt", KVfpuVectorInit: "vinit", KViim: "viim", KVf2i: "vf2i", KVi2f: "vi2f",
	KVmmul: "vmmul", KVtfm: "vtfm", KVmscl: "vmscl"}

func isVfpuInsn(d *Insn) bool { return d.Vfpu || isNoPrefix(d.Kind) }

// fastName ports fast_name(): name of the lean_vk template for this instruction, or "".
func fastName(d *Insn) string {
	k := d.Kind
	if n, ok := fastState[k]; ok {
		return n
	}
	if k == KVi2x {
		if (d.W>>16)&31 == 28 {
			return "vi2uc"
		}
		return ""
	}
	if k == KExtra && d.Mn == "vi2c" {
		return "vi2c"
	}
	return ""
}

// vstep ports step(): state after instruction d (generic or specialised, they agree) given the state before it.
func vstep(d *Insn, st vstate) vstate {
	k := d.Kind
	if k == KVpfx {
		c := (d.W >> 24) & 3
		data := d.W & 0xFFFFF
		if c == 2 {
			data &= 0xFFF
		}
		st[c] = int32(data)
		return st
	}
	if k == KVflush {
		if d.W&0xFFFF0000 == 0xFFFF0000 {
			return st
		}
		return vDef
	}
	if k == KMtv {
		r := d.W & 0xFF
		if 128 <= r && r < 131 {
			st[r-128] = vTop
		}
		return st
	}
	if isNoPrefix(k) {
		return st
	}
	if k == KExtra {
		if isNoPrefixExtra(d.Mn) {
			return st
		}
		if d.Mn == "vmtvc" {
			return vstate{vTop, vTop, vTop}
		}
	}
	if fastName(d) != "" {
		return vDef
	}
	if st == vDef {
		return vDef
	}
	return vJoin(st, vDef) // a generic compute instruction either consumes the pending prefixes or leaves them
}

type vIssue struct {
	PC    uint32
	Kind  string
	State vstate
}

// vfpuAnalyze ports analyze(): in_state by node id (nil: no VFPU code) and the issues.
func vfpuAnalyze(f *Func) (map[int]vstate, []vIssue) {
	nodes := f.Nodes
	any := false
	for _, n := range nodes {
		if n.Insn != nil && (n.Kind == NN || n.Kind == NSlot) && isVfpuInsn(n.Insn) {
			any = true
			break
		}
	}
	if !any {
		return nil, nil
	}
	ins := map[int]vstate{}
	var insOrder []int
	entry := f.EntryID
	ins[entry] = vDef
	insOrder = append(insOrder, entry)
	work := []int{entry}
	for len(work) > 0 {
		nid := work[len(work)-1]
		work = work[:len(work)-1]
		n := nodes[nid]
		st := ins[nid]
		var out vstate
		if n.Kind == NCont {
			out = vDef
		} else if (n.Kind == NN || n.Kind == NSlot) && n.Insn != nil {
			out = vstep(n.Insn, st)
		} else {
			out = st
		}
		for _, sx := range n.Succ {
			old, had := ins[sx]
			nw := out
			if had {
				nw = vJoin(old, out)
			}
			if !had || nw != old {
				if !had {
					insOrder = append(insOrder, sx)
				}
				ins[sx] = nw
				work = append(work, sx)
			}
		}
	}
	var issues []vIssue
	for _, nid := range insOrder {
		st := ins[nid]
		n := nodes[nid]
		if st != vDef && (n.Kind == NCont || n.Kind == NTail || n.Kind == NRet) {
			kind := map[NodeKind]string{NCont: "cont", NTail: "tail", NRet: "ret"}[n.Kind]
			issues = append(issues, vIssue{n.PC, kind, st})
		}
	}
	return ins, issues
}

// fastLines ports fast_lines(): C++ statements of the specialised form of d, or ok == false when the generic helper has to be
// used.
func (fg *FuncGen) fastLines(d *Insn, st vstate, hasSt bool) ([]string, bool) {
	k := d.Kind
	w := fmt.Sprintf("0x%08Xu", d.W)
	if k == KVpfx {
		return []string{fmt.Sprintf("lean_vk::vpfx<%s>();", w)}, true
	}
	switch k {
	case KLvs, KSvs, KLvq, KSvq:
		off := int(s16(d.W & 0xFFFC))
		a := fg.addr(int(d.Rs), uint32(int32(off)))
		nm := map[Kind]string{KLvs: "lvs", KSvs: "svs", KLvq: "lvq", KSvq: "svq"}[k]
		return []string{fmt.Sprintf("lean_vk::%s<%s>(%s);", nm, w, a)}, true
	case KMtv:
		return []string{fmt.Sprintf("lean_vk::mtv<%s>(%s);", w, fg.R(int(d.Rt)))}, true
	case KMfv:
		if d.Rt != 0 {
			return []string{fmt.Sprintf("%s = lean_vk::mfv<%s>();", fg.Wr(int(d.Rt)), w)}, true
		}
		return []string{}, true
	}
	name := fastName(d)
	if name == "" || !hasSt || st[0] == vTop || st[1] == vTop || st[2] == vTop {
		return nil, false
	}
	if k == KVfpuCompare3 && !(5 <= ((d.W>>23)&7) && ((d.W>>23)&7) <= 7) {
		return nil, false
	}
	return []string{fmt.Sprintf("lean_vk::%s<%s, 0x%Xu, 0x%Xu, 0x%Xu>();", name, w, st[0], st[1], st[2])}, true
}
