package main

import (
	"bytes"
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"
)

// fakeELF builds a 32-bit little-endian ELF file: header, two program headers, two segments, then a "symbol table" and section
// headers that stand for what the linker's host leaves behind.
func fakeELF(symtab string) []byte {
	le := binary.LittleEndian
	b := make([]byte, 0, 4096)
	h := make([]byte, 52)
	copy(h, "\x7fELF\x01\x01\x01")
	le.PutUint32(h[28:], 52) // e_phoff
	le.PutUint16(h[42:], 32) // e_phentsize
	le.PutUint16(h[44:], 2)  // e_phnum
	b = append(b, h...)
	ph := func(off, size uint32) []byte {
		p := make([]byte, 32)
		le.PutUint32(p[0:], 1)
		le.PutUint32(p[4:], off)
		le.PutUint32(p[16:], size)
		return p
	}
	b = append(b, ph(128, 16)...)
	b = append(b, ph(160, 8)...)
	for len(b) < 128 {
		b = append(b, 0)
	}
	b = append(b, bytes.Repeat([]byte{0xAA}, 16)...)
	b = append(b, make([]byte, 16)...) // padding between the segments
	b = append(b, bytes.Repeat([]byte{0xBB}, 8)...)
	shoff := len(b)
	b = append(b, symtab...)
	le.PutUint32(b[32:], uint32(shoff)) // e_shoff
	le.PutUint16(b[46:], 40)
	le.PutUint16(b[48:], 5)
	le.PutUint16(b[50:], 4)
	return b
}

func TestStripELF32(t *testing.T) {
	a, err := stripELF32(fakeELF("symbols in one order"))
	if err != nil {
		t.Fatal(err)
	}
	b, err := stripELF32(fakeELF("the same symbols in another order, and longer"))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(a, b) {
		t.Fatal("files that differ only outside the segments must strip to the same bytes")
	}
	if len(a) != 168 {
		t.Fatalf("length %d, want the end of the last segment (168)", len(a))
	}
	if !bytes.Equal(a[128:144], bytes.Repeat([]byte{0xAA}, 16)) || !bytes.Equal(a[160:168], bytes.Repeat([]byte{0xBB}, 8)) {
		t.Fatal("segment bytes changed")
	}
	if !bytes.Equal(a[144:160], make([]byte, 16)) {
		t.Fatal("bytes between the segments are not zero")
	}
	for _, r := range [][2]int{{32, 36}, {46, 48}, {48, 50}, {50, 52}} {
		if !bytes.Equal(a[r[0]:r[1]], make([]byte, r[1]-r[0])) {
			t.Fatalf("header field at %d not zeroed", r[0])
		}
	}
	// stripping again changes nothing
	c, err := stripELF32(a)
	if err != nil || !bytes.Equal(a, c) {
		t.Fatalf("not idempotent: %v", err)
	}
	for _, bad := range [][]byte{nil, []byte("not an elf at all, not even close, really not an elf at all......"), fakeELF("")[:100]} {
		if _, err := stripELF32(bad); err == nil {
			t.Errorf("accepted %d bytes of rubbish", len(bad))
		}
	}
}

func TestStripVelfFile(t *testing.T) {
	p := filepath.Join(t.TempDir(), "x.velf")
	os.WriteFile(p, fakeELF("abc"), 0o644)
	if err := stripVelf(p); err != nil {
		t.Fatal(err)
	}
	got, _ := os.ReadFile(p)
	want, _ := stripELF32(fakeELF("zzz"))
	if !bytes.Equal(got, want) {
		t.Fatal("file not stripped")
	}
	if _, err := os.Stat(p + ".strip"); err == nil {
		t.Fatal("temporary file left behind")
	}
}
