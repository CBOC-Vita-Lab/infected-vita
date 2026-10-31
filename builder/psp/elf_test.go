package psp

import (
	"encoding/binary"
	"testing"
)

// buildPRX makes a PSP PRX (type 0xFFA0) with one PT_LOAD segment at vaddr 0 holding body and one relocation section.
func buildPRX(body []byte, rels [][2]uint32) []byte {
	le := binary.LittleEndian
	const ehdr, phdr, shdr = 52, 32, 40
	strtab := []byte("\x00.rel.text\x00.shstrtab\x00")
	bodyOff := ehdr + phdr
	relOff := bodyOff + len(body)
	strOff := relOff + 8*len(rels)
	shOff := (strOff + len(strtab) + 3) &^ 3
	out := make([]byte, shOff+3*shdr)
	copy(out, []byte{0x7F, 'E', 'L', 'F', 1, 1, 1})
	le.PutUint16(out[16:], 0xFFA0)
	le.PutUint16(out[18:], 8)
	le.PutUint32(out[24:], 0)
	le.PutUint32(out[28:], ehdr)
	le.PutUint32(out[32:], uint32(shOff))
	le.PutUint16(out[40:], ehdr)
	le.PutUint16(out[42:], phdr)
	le.PutUint16(out[44:], 1)
	le.PutUint16(out[46:], shdr)
	le.PutUint16(out[48:], 3)
	le.PutUint16(out[50:], 2)
	p := out[ehdr:]
	le.PutUint32(p[0:], 1)
	le.PutUint32(p[4:], uint32(bodyOff))
	le.PutUint32(p[16:], uint32(len(body)))
	le.PutUint32(p[20:], uint32(len(body)))
	copy(out[bodyOff:], body)
	for i, r := range rels {
		le.PutUint32(out[relOff+8*i:], r[0])
		le.PutUint32(out[relOff+8*i+4:], r[1])
	}
	copy(out[strOff:], strtab)
	sh := func(i int, name, typ, off, size uint32) {
		s := out[shOff+i*shdr:]
		le.PutUint32(s[0:], name)
		le.PutUint32(s[4:], typ)
		le.PutUint32(s[16:], off)
		le.PutUint32(s[20:], size)
	}
	sh(1, 1, sectionPspRel, uint32(relOff), uint32(8*len(rels)))
	sh(2, 11, 3, uint32(strOff), uint32(len(strtab)))
	return out
}

func TestRelocatedImage(t *testing.T) {
	le := binary.LittleEndian
	const n = 16
	body := make([]byte, 4*n)
	put := func(i int, w uint32) { le.PutUint32(body[4*i:], w) }
	put(0, 0x3C040000)   // lui a0, %hi
	put(1, 0x24848000+4) // addiu a0, a0, %lo (negative low half, so the high half is corrected)
	put(2, 0x0C000010)   // jal 0x40
	put(3, 0x00000100)   // .word target (R_MIPS_32)
	// relocation entries: offset, info = type | patch segment << 8 | target segment << 16
	rels := [][2]uint32{
		{0, rMipsHi16}, {4, rMipsLo16}, {8, rMips26}, {12, rMips32},
	}
	elf, err := ParseElf(buildPRX(body, rels))
	if err != nil {
		t.Fatal(err)
	}
	img, st, err := elf.RelocatedImage(DefaultBase)
	if err != nil {
		t.Fatal(err)
	}
	if len(img) != len(body) {
		t.Fatalf("image is %d bytes, want %d", len(img), len(body))
	}
	if st.Total != 4 || st.Mips32 != 1 || st.Mips26 != 1 || st.MipsHi16 != 1 || st.MipsLo16 != 1 || st.Invalid != 0 {
		t.Fatalf("stats %+v", st)
	}
	w := func(i int) uint32 { return le.Uint32(img[4*i:]) }
	// hi/lo pair: (0 << 16) + int16(0x8004) + 0x08804000 = 0x087FC004; the high half is corrected for the negative low half
	// (0xC004) to 0x0880
	if got := w(0); got != 0x3C040880 {
		t.Errorf("hi16 word %08x, want 3c040880", got)
	}
	if got := w(1); got != 0x2484C004 {
		t.Errorf("lo16 word %08x, want 2484c004", got)
	}
	if got, want := w(2), uint32(0x0C000010)+(0x08804000>>2); got != want {
		t.Errorf("jal word %08x, want %08x", got, want)
	}
	if got, want := w(3), uint32(0x100+0x08804000); got != want {
		t.Errorf("data word %08x, want %08x", got, want)
	}
	if w(4) != 0 {
		t.Errorf("untouched word changed")
	}
}

func TestParseElfErrors(t *testing.T) {
	cases := map[string][]byte{
		"short":     {1, 2, 3},
		"not elf":   make([]byte, 64),
		"encrypted": append([]byte("~PSP"), make([]byte, 60)...),
		"pbp":       append([]byte("\x00PBP"), make([]byte, 60)...),
	}
	for name, b := range cases {
		if _, err := ParseElf(b); err == nil {
			t.Errorf("%s: expected an error", name)
		}
	}
	if _, err := BuildImage([]byte("junk")); err == nil {
		t.Error("BuildImage accepted junk")
	}
}
