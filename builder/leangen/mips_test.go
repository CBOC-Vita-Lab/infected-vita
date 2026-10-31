package leangen

import (
	"bufio"
	"bytes"
	"testing"
)

func TestDecodeKnownWords(t *testing.T) {
	type want struct {
		w      uint32
		kind   Kind
		mn     string
		uses   []int
		defs   []int
		ctl    bool
		mem    uint8
		likely bool
		simm   int32
		vfpu   bool
	}
	cases := []want{
		{0x00000000, KNop, "nop", nil, nil, false, memNone, false, 0, false},
		{0x27BDFFE0, KAddiu, "addiu", []int{29}, []int{29}, false, memNone, false, -32, false},
		{0x03E00008, KJr, "jr", []int{31}, nil, true, memNone, false, 8, false},
		{0x0C000040, KJal, "jal", nil, nil, true, memNone, false, 0x40, false},
		{0xAFBF001C, KSw, "sw", []int{29, 31}, nil, false, memSt, false, 28, false},
		{0x8FBF001C, KLw, "lw", []int{29}, []int{31}, false, memLd, false, 28, false},
		{0x3C041234, KLui, "lui", nil, []int{4}, false, memNone, false, 0x1234, false},
		{0x00851021, KAddu, "addu", []int{4, 5}, []int{2}, false, memNone, false, 0x1021, false},
		{0x1000FFFF, KBeq, "beq", nil, nil, true, memNone, false, -1, false},
		{0x5040FFFE, KBeql, "beql", []int{2}, nil, true, memNone, true, -2, false},
		{0x46000006, KMovS, "mov.s", []int{32}, []int{32}, false, memNone, false, 6, false},
		{0xD8000000, KLvq, "lv.q", nil, nil, false, memLd, false, 0, true},
		{0xF8840010, KSvq, "sv.q", []int{4}, nil, false, memSt, false, 16, true},
		{0xDC000000, KVpfx, "vpfxs", nil, nil, false, memNone, false, 0, true},
		{0x70431000, KUnsupported, "special?", nil, nil, false, memNone, false, 0, false},
	}
	for _, c := range cases {
		d := Decode(0x08804000, c.w)
		if d.Kind != c.kind || (c.mn != "" && d.Mn != c.mn && !(c.kind == KUnsupported)) {
			t.Errorf("%08x: kind %v mn %q, want %v %q", c.w, d.Kind, d.Mn, c.kind, c.mn)
			continue
		}
		set := func(s RegSet) map[int]bool {
			m := map[int]bool{}
			for i := 0; i < s.Len(); i++ {
				m[s.At(i)] = true
			}
			return m
		}
		check := func(name string, got RegSet, want []int) {
			g := set(got)
			if len(g) != len(want) {
				t.Errorf("%08x %s = %v, want %v", c.w, name, g, want)
				return
			}
			for _, r := range want {
				if !g[r] {
					t.Errorf("%08x %s = %v, want %v", c.w, name, g, want)
				}
			}
		}
		if c.kind != KUnsupported {
			check("uses", d.Uses, c.uses)
			check("defs", d.Defs, c.defs)
			if d.Simm != c.simm && c.kind != KJr && c.kind != KMovS && c.kind != KAddu {
				t.Errorf("%08x simm %d, want %d", c.w, d.Simm, c.simm)
			}
		}
		if d.Ctl != c.ctl || d.Delay != c.ctl || d.Mem != c.mem || d.Likely != c.likely || d.Vfpu != c.vfpu {
			t.Errorf("%08x ctl %v mem %d likely %v vfpu %v", c.w, d.Ctl, d.Mem, d.Likely, d.Vfpu)
		}
	}
}

func TestBranchHelpers(t *testing.T) {
	b := Decode(0x08804010, 0x1000FFFF) // beq zero, zero, -1 word: an unconditional branch to itself
	if got := branchTarget(b.PC, b); got != 0x08804010 {
		t.Errorf("branch target %08x", got)
	}
	if branchConst(b) != TriTrue {
		t.Errorf("beq r0,r0 should be always taken")
	}
	bne := Decode(0, 0x14000001) // bne zero, zero
	if branchConst(bne) != TriFalse {
		t.Errorf("bne r0,r0 should never be taken")
	}
	beq := Decode(0, 0x10850001)
	if branchConst(beq) != TriNone {
		t.Errorf("beq a0,a1 is not constant")
	}
	j := Decode(0x08804000, 0x0A201000) // j 0x08804000
	if got := jumpTarget(j.PC, j); got != 0x08804000 {
		t.Errorf("jump target %08x", got)
	}
}

func TestExtraNames(t *testing.T) {
	cases := map[uint32]string{0xC0000000: "ll", 0xE0000000: "sc", 0xD4000000: "lvl.q", 0xD4000002: "lvr.q", 0xF4000000: "svl.q", 0x00000000: ""}
	for w, want := range cases {
		if got := extraName(w); got != want {
			t.Errorf("extraName(%08x) = %q, want %q", w, got, want)
		}
	}
}

func TestFormatting(t *testing.T) {
	if u(7) != "7u" || u(10) != "0xAu" || u(0xFFFFFFFF) != "0xFFFFFFFFu" {
		t.Errorf("u(): %s %s %s", u(7), u(10), u(0xFFFFFFFF))
	}
	if hexw(0x8804000) != "0x08804000u" {
		t.Errorf("hexw: %s", hexw(0x8804000))
	}
	if regName(0) != "0u" || regName(29) != "sp" || regName(4) != "r4" || regName(44) != "f12" || regName(66) != "lo" {
		t.Error("register names")
	}
	if pyHex(0x88b616c) != "0x88b616c" {
		t.Error("pyHex")
	}
}

func TestJSONLayoutMatchesPython(t *testing.T) {
	doc := jobj{{"a", jint(1)}, {"b", jarr{}}, {"c", jobj{}}, {"d", jarr{jstr("x\"y\\z\n"), jnull{}, jbool(true), jarr{jint(2)}}}, {"e", jstr("é\u2028😀")}}
	var buf bytes.Buffer
	w := bufio.NewWriter(&buf)
	doc.write(w)
	w.Flush()
	// json.dump(doc, fh, indent=0) of the same document
	want := "{\n\"a\": 1,\n\"b\": [],\n\"c\": {},\n\"d\": [\n\"x\\\"y\\\\z\\n\",\nnull,\ntrue,\n[\n2\n]\n],\n\"e\": \"\\u00e9\\u2028\\ud83d\\ude00\"\n}"
	if buf.String() != want {
		t.Errorf("got  %q\nwant %q", buf.String(), want)
	}
}

func TestMaskAndBits(t *testing.T) {
	m := maskOf(2, 3, 32, 66)
	if !m.Has(66) || m.Has(65) || m.Count() != 4 {
		t.Fatalf("mask %+v", m)
	}
	got := m.Regs()
	if len(got) != 4 || got[0] != 2 || got[3] != 66 {
		t.Fatalf("regs %v", got)
	}
	if !argsMask.Has(SP) || argsNoSP.Has(SP) || !argsNoSP.Has(F(19)) || argsNoSP.Has(F(20)) || !outEnc.Has(F0) {
		t.Fatal("ABI masks")
	}
	if !calleeSaved.Has(16) || !calleeSaved.Has(F(31)) || calleeSaved.Has(F(19)) || !callerSaved.Has(HI) {
		t.Fatal("callee / caller saved masks")
	}
	// the weak half of a live set is the strong half shifted up
	c := cbMask(maskOf(5))
	c.x = SlotBs + 3
	b := c.both()
	if !b.bit(5) || !b.bit(wsh+5) || !b.bit(SlotBs+3) || !b.bit(wsh+SlotBs+3) || b.bit(6) {
		t.Fatalf("both(): %v", b)
	}
	if !b.weakReg().Has(5) || !b.reg().Has(5) || b.reg().Has(6) {
		t.Fatal("weak / strong register halves")
	}
	if b.andNot(strongBits(maskOf(5))).bit(5) {
		t.Fatal("andNot")
	}
}

func TestJoinStates(t *testing.T) {
	a := initialState()
	b := initialState()
	b.regs[8] = Val{'s', -16}
	a.setSlot(-8, Val{'o', 16})
	b.setSlot(-8, Val{'o', 16})
	a.setSlot(-4, Val{'o', 31})
	j := joinStates(&a, &b)
	if j.regs[8] != valT || j.regs[SP] != (Val{'s', 0}) || j.regs[9] != (Val{'o', 9}) {
		t.Errorf("registers: %+v %+v %+v", j.regs[8], j.regs[SP], j.regs[9])
	}
	if v, _ := j.slot(-8); v != (Val{'o', 16}) {
		t.Errorf("equal slot: %+v", v)
	}
	if v, _ := j.slot(-4); v != valP { // stored on only one path
		t.Errorf("one-sided slot: %+v", v)
	}
}

func TestProfileParsing(t *testing.T) {
	p, err := ParseProfile([]byte(`{"noreturn": ["0x08A2B794"], "weak_args": 8, "yield": ["0x10"], "hooks": {"0x08A2F228": {"name": "memcpy"}, "0x20": {"name": "strcmp", "enabled": false}},
		"import_arity": {"0x08A89984": {"name": "x", "gpr": 4, "fpr": 1}}, "extra_inputs": {"0x30": [4, 5]}, "jalr_weak_all": false}`))
	if err != nil {
		t.Fatal(err)
	}
	if len(p.Noreturn) != 1 || p.Noreturn[0] != 0x08A2B794 || p.WeakArgs != 8 || p.JalrWeakAll {
		t.Errorf("%+v", p)
	}
	if h := p.Hooks[0x08A2F228]; h.Name != "memcpy" || !h.Enabled {
		t.Errorf("hook %+v", h)
	}
	if h := p.Hooks[0x20]; h.Enabled {
		t.Errorf("disabled hook %+v", h)
	}
	if a := p.ImportArity[0x08A89984]; a.GPR != 4 || a.FPR != 1 {
		t.Errorf("arity %+v", a)
	}
	if _, ok := p.Yield[0x10]; !ok || len(p.ExtraInputs[0x30]) != 2 {
		t.Errorf("yield / extra inputs")
	}
	e, err := ParseProfile(nil)
	if err != nil || e.WeakArgs != 4 || !e.JalrWeakAll {
		t.Errorf("empty profile: %+v %v", e, err)
	}
	if _, err := ParseProfile([]byte(`{"noreturn": ["zz"]}`)); err == nil {
		t.Error("a bad address was accepted")
	}
}
