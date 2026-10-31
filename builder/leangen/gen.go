// gen.go mirrors recomp/lean_gen.py (and recomp/leangen/pipeline.py): the total static recompiler (guest PSP MIPS / Allegrex ->
// C++), "lean" form.
//
// Generate writes into the output directory:
//
//	lean_decls.h        prototypes of every f_ADDR / t_ADDR
//	lean_NNN.cpp        translation units (functions in address order)
//	lean_table.cpp      g_lean_entry[], g_lean_entry_count, hook switches
//	lean_test_meta.inc  rows for the differential harness
//	manifest.json       per function: address, size, form, signature, reasons, hooks, TU
//
// Every guest function gets a typed or regfile body plus a t_ADDR thunk and a g_lean_entry slot; a direct call target that is not a
// known function would become an explicit pk_guest_call(pc) fallback listed in the manifest (none today).
package leangen

import (
	"bufio"
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"runtime/debug"
	"sort"
	"strings"
)

// WordsPerTU is the default number of guest words per translation unit (--words-per-tu).
const WordsPerTU = 9000

// Fallback is a direct call to an address that is not a known function.
type Fallback struct {
	Caller, Target uint32
	Reason         string
}

type ordCounter struct {
	keys []string
	m    map[string]int
}

func newCounter() *ordCounter { return &ordCounter{m: map[string]int{}} }

func (c *ordCounter) add(k string) {
	if _, ok := c.m[k]; !ok {
		c.keys = append(c.keys, k)
	}
	c.m[k]++
}

func (c *ordCounter) json() jobj {
	o := jobj{}
	for _, k := range c.keys {
		o = append(o, jkv{k, jint(c.m[k])})
	}
	return o
}

type vfpuIssueRec struct {
	Func uint32
	vIssue
}

// Emitter ports the Emitter class of lean_gen.py.
type Emitter struct {
	P           *Program2
	L           *Solver
	Profile     *Profile
	stats       *ordCounter
	Fallbacks   []Fallback
	NoVfpuFast  bool
	NoAlias     bool
	vfpuForms   *ordCounter
	vfpuGeneric *ordCounter
	vfpuIssues  []vfpuIssueRec
	Yield       map[uint32]struct{}
	hooks       map[uint32]Hook
}

type genError struct{ err error }

func fail(format string, a ...interface{}) { panic(genError{fmt.Errorf(format, a...)}) }

type hookDef struct {
	flag, fn string
	regs     []int
}

var hookTable = map[string]hookDef{
	"memcpy": {"LEAN_HOOK_MEMCPY", "lean_hook_memcpy", []int{4, 5, 6}},
	"memset": {"LEAN_HOOK_MEMSET", "lean_hook_memset", []int{4, 5, 6}},
	"strcmp": {"LEAN_HOOK_STRCMP", "lean_hook_strcmp", []int{4, 5}},
}

func newEmitter(P *Program2, L *Solver, profile *Profile) *Emitter {
	return &Emitter{P: P, L: L, Profile: profile, stats: newCounter(), vfpuForms: newCounter(), vfpuGeneric: newCounter(),
		Yield: profile.Yield, hooks: profile.Hooks}
}

func (E *Emitter) hookPrologue(fg *FuncGen) []string {
	h, ok := E.hooks[fg.e]
	if !ok || !h.Enabled {
		return nil
	}
	hd, ok := hookTable[h.Name]
	if !ok {
		fail("unknown hook %q at %08x", h.Name, fg.e)
	}
	if !fg.typed || len(fg.rsRet) != 1 || fg.rsRet[0] != V0 {
		fail("hook %s at %08x needs a typed function returning v0 only", h.Name, fg.e)
	}
	for _, r := range hd.regs {
		if !inList(r, fg.ps) {
			fail("hook %s at %08x: parameter r%d missing from the signature", h.Name, fg.e, r)
		}
	}
	for _, r := range hd.regs {
		fg.R(r)
	}
	fg.R(V0)
	E.stats.add("hooks")
	args := make([]string, len(hd.regs))
	for i, r := range hd.regs {
		args[i] = fmt.Sprintf("r%d", r)
	}
	return []string{fmt.Sprintf("if (LEAN_LIKELY(!(g_lean_hook_off & %s))) return %s(%s);", hd.flag, hd.fn, strings.Join(args, ", "))}
}

type mInfo struct {
	addr, size          uint32
	typed, at           bool
	reasons             []string
	hook                string
	hasHook             bool
	stmts               int
	hasVfpu             bool
	vfFast, vfGen       int
	signature           string
	params, returns     []string
	regLoads, regStores []string
	tu                  int
}

func (m *mInfo) json() jobj {
	form := "regfile"
	if m.typed {
		form = "typed"
	}
	var hook jnode = jnull{}
	if m.hasHook {
		hook = jstr(m.hook)
	}
	o := jobj{{"addr", jint(m.addr)}, {"size", jint(m.size)}, {"form", jstr(form)}, {"at", jbool(m.at)},
		{"reasons", strArr(m.reasons)}, {"hook", hook}, {"stmts", jint(m.stmts)}}
	if m.hasVfpu {
		o = append(o, jkv{"vfpu", jobj{{"fast", jint(m.vfFast)}, {"generic", jint(m.vfGen)}}})
	}
	o = append(o, jkv{"signature", jstr(m.signature)}, jkv{"params", strArr(m.params)}, jkv{"returns", strArr(m.returns)})
	if !m.typed {
		o = append(o, jkv{"regfile_loads", strArr(m.regLoads)}, jkv{"regfile_stores", strArr(m.regStores)})
	}
	o = append(o, jkv{"tu", jint(m.tu)})
	return o
}

// function ports Emitter.function and thunk: -> (lines, info, thunk text).
func (E *Emitter) function(e uint32) ([]string, *mInfo, string, error) {
	s := E.P.S[e]
	fg := NewFuncGen(E, e)
	lines, err := fg.Gen()
	if err != nil {
		return nil, nil, "", err
	}
	if fg.hasVfpu {
		for _, is := range fg.vissues {
			E.vfpuIssues = append(E.vfpuIssues, vfpuIssueRec{e, is})
		}
	}
	info := &mInfo{addr: e, size: uint32(s.Size), typed: s.Typed, at: s.At, reasons: append([]string{}, s.Why...)}
	if h, ok := E.hooks[e]; ok && h.Enabled {
		info.hook, info.hasHook = h.Name, true
	}
	for _, l := range lines {
		if strings.TrimSpace(l) != "" {
			info.stmts++
		}
	}
	if fg.hasVfpu {
		info.hasVfpu = true
		info.vfFast, info.vfGen = fg.vfFast, fg.vfGen
	}
	thunk := ""
	if s.Typed {
		info.signature = E.L.signature(s)
		for _, r := range params(s) {
			info.params = append(info.params, regName(r))
		}
		for _, r := range E.L.rset(s) {
			info.returns = append(info.returns, regName(r))
		}
		thunk = E.thunk(e)
	} else {
		info.signature = fmt.Sprintf("void t_%08X()", e)
		for _, r := range s.A.Or(s.Aw).Regs() {
			info.params = append(info.params, regName(r))
		}
		for _, r := range E.L.Rp(s).Regs() {
			if r < 67 && r != SP {
				info.returns = append(info.returns, regName(r))
			}
		}
		for _, r := range fg.loadRegs {
			info.regLoads = append(info.regLoads, regName(r))
		}
		for _, r := range fg.storeRegs {
			info.regStores = append(info.regStores, regName(r))
		}
	}
	return lines, info, thunk, nil
}

func (E *Emitter) thunk(e uint32) string {
	s := E.P.S[e]
	ps := params(s)
	as := make([]string, len(ps))
	for i, r := range ps {
		as[i] = cpuRef(r)
	}
	call := fmt.Sprintf("f_%08X(%s)", e, strings.Join(as, ", "))
	rr := E.L.rset(s)
	var out []string
	out = append(out, fmt.Sprintf("void t_%08X() {", e))
	switch len(rr) {
	case 0:
		out = append(out, fmt.Sprintf("  %s;", call))
	case 1:
		out = append(out, fmt.Sprintf("  %s = %s;", cpuRef(rr[0]), call))
	case 2:
		out = append(out, fmt.Sprintf("  const uint64_t rv = %s;", call))
		for i, r := range rr {
			x := "lean_hi(rv)"
			if i == 0 {
				x = "lean_lo(rv)"
			}
			if isFP(r) {
				x = "lean_u2f(" + x + ")"
			}
			out = append(out, fmt.Sprintf("  %s = %s;", cpuRef(r), x))
		}
	default:
		out = append(out, fmt.Sprintf("  const lean_r3 rv = %s;", call))
		out = append(out, "  g_cpu.gpr[2] = lean_lo(rv.a); g_cpu.gpr[3] = lean_hi(rv.a); g_cpu.fpr[0] = lean_u2f(rv.b);")
	}
	out = append(out, "}")
	return strings.Join(out, "\n")
}

// writeIfChanged writes a generated file only when its content changed (keeps timestamps, so incremental builds stay
// incremental).
func writeIfChanged(path string, data []byte) error {
	if old, err := os.ReadFile(path); err == nil && bytes.Equal(old, data) {
		return nil
	}
	return os.WriteFile(path, data, 0o644)
}

// Generate runs the whole recompiler: image is the relocated load image of the game's PRX (psp.BuildImage), boot the bytes of
// BOOT.BIN, profile the generator profile JSON (recomp/infected_profile.json). The generated C++ is written into outDir.
// progress, if not nil, is called with (done, total) over the whole run.
func Generate(image, boot []byte, profile []byte, outDir string, progress func(done, total int)) (err error) {
	defer func() {
		if r := recover(); r != nil {
			if ge, ok := r.(genError); ok {
				err = ge.err
				return
			}
			err = fmt.Errorf("internal error in the translator: %v", r)
		}
	}()
	rep := func(done int) {
		if progress != nil {
			progress(done, progressTotal)
		}
	}
	// the translator keeps the whole program in memory: trade a little speed for a smaller peak
	defer debug.SetGCPercent(debug.SetGCPercent(50))
	prof, err := ParseProfile(profile)
	if err != nil {
		return err
	}
	im, err := NewImage(image, boot)
	if err != nil {
		return err
	}
	rep(0)
	prog := Discover(im, prof)
	BuildAll(prog)
	ApplyFuse(prog)
	rep(progDiscover)
	P := NewProgram2(prog)
	P.Phase1(func(d, t int) { rep(progDiscover + (progPhase1-progDiscover)*d/t) })
	L := NewSolver(P, prof.WeakArgs, prof.JalrWeakAll)
	for k, v := range prof.ImportArity {
		L.ImportArity[k] = v
	}
	for k, v := range prof.ExtraInputs {
		L.ExtraInputs[k] = maskOf(v...)
	}
	rep(progPhase1)
	L.Solve(func(d, t int) { rep(progPhase1 + (progSolve-progPhase1)*d/t) })
	L.Decide()
	rep(progSolve)
	if err := os.MkdirAll(outDir, 0o755); err != nil {
		return err
	}
	E := newEmitter(P, L, prof)
	entries := P.Keys
	ntu := 0
	var cur []string
	curWords := 0
	var manifest []*mInfo
	var decls []string
	for n, e := range entries {
		lines, info, thunk, err := E.function(e)
		if err != nil {
			return err
		}
		s := P.S[e]
		if s.Typed {
			decls = append(decls, L.signature(s)+";")
			decls = append(decls, fmt.Sprintf("void t_%08X();", e))
		} else {
			decls = append(decls, fmt.Sprintf("void t_%08X();", e))
		}
		text := strings.Join(lines, "\n") + "\n"
		if s.Typed {
			text += thunk + "\n"
		}
		cur = append(cur, text)
		sz := s.Size
		if sz < 1 {
			sz = 1
		}
		curWords += sz
		manifest = append(manifest, info)
		if curWords >= WordsPerTU {
			if err := writeTU(outDir, ntu, cur); err != nil {
				return err
			}
			ntu++
			cur = nil
			curWords = 0
		}
		if (n+1)%64 == 0 {
			rep(progSolve + (progTotalDone-progSolve)*(n+1)/len(entries))
		}
	}
	if len(cur) > 0 {
		if err := writeTU(outDir, ntu, cur); err != nil {
			return err
		}
		ntu++
	}
	// fix tu index of the last chunk
	idx, acc := 0, 0
	for _, info := range manifest {
		info.tu = idx
		sz := int(info.size)
		if sz < 1 {
			sz = 1
		}
		acc += sz
		if acc >= WordsPerTU {
			idx++
			acc = 0
		}
	}
	hdr := []string{"// generated by recomp/lean_gen.py - do not edit", "#pragma once", "#include \"lean_abi.h\"", ""}
	if err := writeIfChanged(filepath.Join(outDir, "lean_decls.h"), []byte(strings.Join(append(hdr, decls...), "\n")+"\n")); err != nil {
		return err
	}
	if err := writeTable(outDir, P, entries); err != nil {
		return err
	}
	if err := writeTestMeta(outDir, P, L, entries); err != nil {
		return err
	}
	if err := writeManifest(outDir, P, manifest, E, entries); err != nil {
		return err
	}
	rep(progressTotal)
	return nil
}

func writeTU(outDir string, i int, chunk []string) error {
	var b strings.Builder
	b.WriteString("// generated by recomp/lean_gen.py - do not edit\n#include \"lean_decls.h\"\n\n")
	b.WriteString(strings.Join(chunk, "\n"))
	return writeIfChanged(filepath.Join(outDir, fmt.Sprintf("lean_%03d.cpp", i)), []byte(b.String()))
}

// progress milestones (of progressTotal)
const (
	progressTotal = 1000
	progDiscover  = 200
	progPhase1    = 450
	progSolve     = 600
	progTotalDone = 980
)

func writeTable(out string, P *Program2, entries []uint32) error {
	words := int(P.Im.StubEnd-Base) / 4
	ent := make(map[int]uint32, len(entries))
	for _, e := range entries {
		ent[int((e-Base)>>2)] = e
	}
	var b strings.Builder
	b.WriteString("// generated by recomp/lean_gen.py - do not edit\n#include \"lean_decls.h\"\n#include <stdlib.h>\n#include <string.h>\n\n")
	fmt.Fprintf(&b, "extern \"C\" {\nconst uint32_t g_lean_entry_count = %du;\n", words)
	b.WriteString("uint32_t g_lean_hook_off = 0;\n")
	b.WriteString("void lean_init_hooks_from_env(void) {\n  const char* v = getenv(\"LEAN_NO_HOOKS\");\n  if (!v) return;\n" +
		"  if (strstr(v, \"memcpy\") || strstr(v, \"all\")) g_lean_hook_off |= LEAN_HOOK_MEMCPY;\n" +
		"  if (strstr(v, \"memset\") || strstr(v, \"all\")) g_lean_hook_off |= LEAN_HOOK_MEMSET;\n" +
		"  if (strstr(v, \"strcmp\") || strstr(v, \"all\")) g_lean_hook_off |= LEAN_HOOK_STRCMP;\n}\n")
	fmt.Fprintf(&b, "void (*const g_lean_entry[%d])() = {\n", words)
	nrow := 0
	for i := 0; i < words; i++ {
		if nrow > 0 {
			b.WriteByte(',')
		}
		if e, ok := ent[i]; ok {
			fmt.Fprintf(&b, "t_%08X", e)
		} else {
			b.WriteByte('0')
		}
		nrow++
		if nrow == 16 {
			b.WriteString(",\n")
			nrow = 0
		}
	}
	if nrow > 0 {
		b.WriteString(",\n")
	}
	b.WriteString("};\n}\n")
	return writeIfChanged(filepath.Join(out, "lean_table.cpp"), []byte(b.String()))
}

func writeTestMeta(out string, P *Program2, L *Solver, entries []uint32) error {
	var b strings.Builder
	b.WriteString("// generated by recomp/lean_gen.py - do not edit\n")
	b.WriteString("// addr, form (0 typed, 1 regfile), returned gpr mask, returned fpr mask, address-taken, size in words,\n")
	b.WriteString("// strongly demanded gpr / fpr masks (v0, v1, f0 are junk pass-through where only forced by jalr read-back),\n")
	b.WriteString("// parameter gpr / fpr masks (everything else is not part of the calling contract)\n")
	strong := L.StrongRp()
	fmt.Fprintf(&b, "static const uint32_t kCodeBegin = 0x%08XU, kCodeEnd = 0x%08XU;      // guest code: a store here is self-modifying code\n", P.Im.Base, P.Im.StubEnd)
	fmt.Fprintf(&b, "static const uint32_t kGp = 0x%08XU;      // the constant $gp of the module\n", P.Im.Gp)
	b.WriteString("static const TestRow kRows[] = {\n")
	for _, e := range entries {
		s := P.S[e]
		var rr []int
		if s.Typed {
			rr = L.rset(s)
		} else {
			for _, r := range L.Rp(s).Regs() {
				if r < 64 && r != SP {
					rr = append(rr, r)
				}
			}
		}
		var gm, fm uint64
		for _, r := range rr {
			if r < 32 {
				gm |= 1 << uint(r)
			}
			if 32 <= r && r < 64 {
				fm |= 1 << uint(r-32)
			}
		}
		st := strong[e]
		sgm := gm &^ 0xC
		for _, r := range []int{2, 3} {
			if st.Has(r) {
				sgm |= 1 << uint(r)
			}
		}
		sfm := fm &^ 1
		if st.Has(32) {
			sfm |= 1
		}
		var ps []int
		if s.Typed {
			ps = params(s)
		} else {
			for _, r := range s.A.Or(s.Aw).Regs() {
				if r != SP {
					ps = append(ps, r)
				}
			}
		}
		var pgm, pfm uint64
		for _, r := range ps {
			if r < 32 {
				pgm |= 1 << uint(r)
			}
			if 32 <= r && r < 64 {
				pfm |= 1 << uint(r-32)
			}
		}
		form, at := 1, 0
		if s.Typed {
			form = 0
		}
		if s.At {
			at = 1
		}
		fmt.Fprintf(&b, "{0x%08XU,%d,0x%XU,0x%XU,%d,%d,0x%XU,0x%XU,0x%XU,0x%XU},\n", e, form, gm, fm, at, s.Size, sgm, sfm, pgm, pfm)
	}
	b.WriteString("};\n")
	// per import: the arity every call site really defines (min over sites) -- the fake HLE of the harness hashes only those
	// arguments, because a site that defines fewer registers than the stub's widest site leaves the rest undefined (the oracle
	// sees junk there, lean code sees 0)
	amin := map[uint32][2]int{}
	for _, e := range P.Keys {
		s := P.S[e]
		fl := L.fl[e]
		if !fl.hasCalls {
			continue
		}
		md := fl.md
		if !fl.mdOK {
			md = fl.computeMD(L.calleeDefs)
		}
		for _, nd := range s.F.Nodes {
			if (nd.Kind == NCont || nd.Kind == NTail) && nd.hasTgt() {
				if _, imp := P.Imports[uint32(nd.Tgt)]; imp {
					have := md[nd.ID]
					ng, nf := runLen(have, argGPR), runLen(have, argFPR)
					o, ok := amin[uint32(nd.Tgt)]
					if !ok {
						amin[uint32(nd.Tgt)] = [2]int{ng, nf}
					} else {
						if ng < o[0] {
							o[0] = ng
						}
						if nf < o[1] {
							o[1] = nf
						}
						amin[uint32(nd.Tgt)] = o
					}
				}
			}
		}
	}
	b.WriteString("// stub, gpr args marshalled (widest site), fpr args marshalled, gpr args defined at every site, fpr likewise\n")
	b.WriteString("static const StubRow kStubs[] = {\n")
	stubs := make([]uint32, 0, len(P.Imports))
	for a := range P.Imports {
		stubs = append(stubs, a)
	}
	sort.Slice(stubs, func(i, j int) bool { return stubs[i] < stubs[j] })
	for _, stub := range stubs {
		ar := L.ar[stub]
		ng, nf := 0, 0
		for i := 4; i < 12; i++ {
			if ar.Has(i) {
				ng++
			}
		}
		for i := 12; i < 20; i++ {
			if ar.Has(32 + i) {
				nf++
			}
		}
		m := amin[stub]
		mg, mf := m[0], m[1]
		if ng < mg {
			mg = ng
		}
		if nf < mf {
			mf = nf
		}
		fmt.Fprintf(&b, "{0x%08XU,%d,%d,%d,%d},\n", stub, ng, nf, mg, mf)
	}
	b.WriteString("};\n")
	return writeIfChanged(filepath.Join(out, "lean_test_meta.inc"), []byte(b.String()))
}

func pyHex(x uint32) string { return fmt.Sprintf("0x%x", x) }

func writeManifest(out string, P *Program2, manifest []*mInfo, E *Emitter, entries []uint32) error {
	// summarize(): coverage numbers and the unclaimed words of the code range (null table slots: the runtime interprets those)
	type rng struct{ a, b uint32 }
	claimed := make([]rng, len(manifest))
	for i, m := range manifest {
		claimed[i] = rng{m.addr, m.addr + 4*m.size}
	}
	sort.Slice(claimed, func(i, j int) bool {
		if claimed[i].a != claimed[j].a {
			return claimed[i].a < claimed[j].a
		}
		return claimed[i].b < claimed[j].b
	})
	var gaps []rng
	cur := uint32(Base)
	for _, c := range claimed {
		if c.a > cur {
			gaps = append(gaps, rng{cur, c.a})
		}
		if c.b > cur {
			cur = c.b
		}
	}
	if cur < P.Im.StubEnd {
		gaps = append(gaps, rng{cur, P.Im.StubEnd})
	}
	cTyped, cReg, wTyped, wReg := 0, 0, 0, 0
	for _, m := range manifest {
		if m.typed {
			cTyped++
			wTyped += int(m.size)
		} else {
			cReg++
			wReg += int(m.size)
		}
	}
	unclaimed := 0
	for _, g := range gaps {
		unclaimed += int(g.b-g.a) / 4
	}
	var ranges jarr
	for i, g := range gaps {
		if i >= 200 {
			break
		}
		ranges = append(ranges, jarr{jstr(pyHex(g.a)), jstr(pyHex(g.b))})
	}
	summary := jobj{{"functions", jint(len(manifest))}, {"typed", jint(cTyped)}, {"regfile", jint(cReg)}, {"fallback", jint(len(E.Fallbacks))},
		{"words_typed", jint(wTyped)}, {"words_regfile", jint(wReg)}, {"code_range", jarr{jstr(pyHex(Base)), jstr(pyHex(P.Im.StubEnd))}},
		{"table_slots", jint(int(P.Im.StubEnd-Base) / 4)}, {"table_slots_non_null", jint(len(entries))}, {"unclaimed_words", jint(unclaimed)},
		{"unclaimed_ranges", ranges}, {"unclaimed_ranges_total", jint(len(gaps))}}
	funcs := make(jarr, len(manifest))
	for i, m := range manifest {
		funcs[i] = m.json()
	}
	fbs := jarr{}
	for _, f := range E.Fallbacks {
		fbs = append(fbs, jobj{{"caller", jstr(pyHex(f.Caller))}, {"target", jstr(pyHex(f.Target))}, {"reason", jstr(f.Reason)}})
	}
	issues := jarr{}
	for _, is := range E.vfpuIssues {
		st := make(jarr, 3)
		for i, x := range is.State {
			if x < 0 {
				st[i] = jnull{}
			} else {
				st[i] = jint(x)
			}
		}
		issues = append(issues, jobj{{"func", jint(is.Func)}, {"pc", jint(is.PC)}, {"kind", jstr(is.Kind)}, {"state", st}})
	}
	doc := jobj{{"functions", funcs}, {"fallbacks", fbs}, {"summary", summary}, {"stats", E.stats.json()},
		{"vfpu", jobj{{"fast_forms", E.vfpuForms.json()}, {"generic_kinds", E.vfpuGeneric.json()}, {"impure_sites", issues}}}}
	var buf bytes.Buffer
	w := bufio.NewWriterSize(&buf, 1<<20)
	doc.write(w)
	if err := w.Flush(); err != nil {
		return err
	}
	return writeIfChanged(filepath.Join(out, "manifest.json"), buf.Bytes())
}
