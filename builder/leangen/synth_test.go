package leangen

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/CBOC-Vita-Lab/infected-vita/builder/psp"
)

// synthPRX builds a tiny PSP PRX: .text with the words, an 8 byte .sceStub.text, .rodata.sceModuleInfo and one PT_LOAD segment.
func synthPRX(words []uint32) []byte {
	le := binary.LittleEndian
	textSize := 4 * len(words)
	stubAddr := textSize
	modAddr := stubAddr + 8
	bodyLen := modAddr + 52
	names := []string{"", ".text", ".sceStub.text", ".rodata.sceModuleInfo", ".shstrtab"}
	var strtab []byte
	nameOff := make([]uint32, len(names))
	for i, n := range names {
		nameOff[i] = uint32(len(strtab))
		strtab = append(strtab, n...)
		strtab = append(strtab, 0)
	}
	const ehdr, phdr, shdr = 52, 32, 40
	bodyOff := ehdr + phdr
	strOff := bodyOff + bodyLen
	shOff := (strOff + len(strtab) + 3) &^ 3
	out := make([]byte, shOff+len(names)*shdr)
	copy(out, []byte{0x7F, 'E', 'L', 'F', 1, 1, 1})
	le.PutUint16(out[16:], 0xFFA0)
	le.PutUint16(out[18:], 8)
	le.PutUint32(out[28:], ehdr)
	le.PutUint32(out[32:], uint32(shOff))
	le.PutUint16(out[40:], ehdr)
	le.PutUint16(out[42:], phdr)
	le.PutUint16(out[44:], 1)
	le.PutUint16(out[46:], shdr)
	le.PutUint16(out[48:], uint16(len(names)))
	le.PutUint16(out[50:], uint16(len(names)-1))
	p := out[ehdr:]
	le.PutUint32(p[0:], 1)
	le.PutUint32(p[4:], uint32(bodyOff))
	le.PutUint32(p[16:], uint32(bodyLen))
	le.PutUint32(p[20:], uint32(bodyLen))
	for i, w := range words {
		le.PutUint32(out[bodyOff+4*i:], w)
	}
	copy(out[bodyOff+modAddr+4:], "synth")
	copy(out[strOff:], strtab)
	sh := func(i int, typ, addr, off, size uint32) {
		s := out[shOff+i*shdr:]
		le.PutUint32(s[0:], nameOff[i])
		le.PutUint32(s[4:], typ)
		le.PutUint32(s[12:], addr)
		le.PutUint32(s[16:], off)
		le.PutUint32(s[20:], size)
	}
	sh(1, 1, 0, uint32(bodyOff), uint32(textSize))
	sh(2, 1, uint32(stubAddr), uint32(bodyOff+stubAddr), 8)
	sh(3, 1, uint32(modAddr), uint32(bodyOff+modAddr), 52)
	sh(4, 3, 0, uint32(strOff), uint32(len(strtab)))
	return out
}

var synthWords = []uint32{
	0x27BDFFF0, // f_08804000: addiu sp, sp, -16
	0xAFBF000C, // sw ra, 12(sp)
	0x0E20100A, // jal 0x08804028
	0x00000000, // nop
	0x8FBF000C, // lw ra, 12(sp)
	0x03E00008, // jr ra
	0x27BD0010, // addiu sp, sp, 16
	0, 0, 0,
	0x03E00008, // f_08804028: jr ra
	0x00851021, // addu v0, a0, a1
}

func runSynth(t *testing.T, out string) {
	t.Helper()
	boot := synthPRX(synthWords)
	img, err := psp.BuildImage(boot)
	if err != nil {
		t.Fatal(err)
	}
	if err := Generate(img, boot, []byte(`{"comment": "synthetic"}`), out, nil); err != nil {
		t.Fatal(err)
	}
}

func TestGenerateSynthetic(t *testing.T) {
	dir := t.TempDir()
	runSynth(t, filepath.Join(dir, "a"))
	runSynth(t, filepath.Join(dir, "b"))
	var names []string
	ents, err := os.ReadDir(filepath.Join(dir, "a"))
	if err != nil {
		t.Fatal(err)
	}
	for _, e := range ents {
		names = append(names, e.Name())
		x, _ := os.ReadFile(filepath.Join(dir, "a", e.Name()))
		y, _ := os.ReadFile(filepath.Join(dir, "b", e.Name()))
		if !bytes.Equal(x, y) {
			t.Errorf("%s differs between two runs", e.Name())
		}
	}
	want := "lean_000.cpp lean_decls.h lean_table.cpp lean_test_meta.inc manifest.json"
	if got := strings.Join(names, " "); got != want {
		t.Fatalf("files %q, want %q", got, want)
	}
	decls, _ := os.ReadFile(filepath.Join(dir, "a", "lean_decls.h"))
	for _, s := range []string{"f_08804000(", "t_08804000", "f_08804028(uint32_t r4, uint32_t r5)", "uint32_t f_08804028"} {
		if !strings.Contains(string(decls), s) {
			t.Errorf("lean_decls.h lacks %q:\n%s", s, decls)
		}
	}
	cpp, _ := os.ReadFile(filepath.Join(dir, "a", "lean_000.cpp"))
	if !strings.Contains(string(cpp), "return (r4 + r5);") && !strings.Contains(string(cpp), "r2 = (r4 + r5);") {
		t.Errorf("the addu was not translated:\n%s", cpp)
	}
	if !strings.Contains(string(cpp), "f_08804028(") {
		t.Errorf("the call was not translated:\n%s", cpp)
	}
	var m struct {
		Functions []struct {
			Addr uint32
			Form string
		}
		Summary struct {
			Functions int
			Typed     int
			Regfile   int
		}
	}
	raw, _ := os.ReadFile(filepath.Join(dir, "a", "manifest.json"))
	if err := json.Unmarshal(raw, &m); err != nil {
		t.Fatalf("manifest.json is not JSON: %v", err)
	}
	if m.Summary.Functions != 2 || m.Summary.Typed != 2 || len(m.Functions) != 2 || m.Functions[0].Addr != 0x08804000 || m.Functions[1].Addr != 0x08804028 {
		t.Errorf("manifest: %+v", m)
	}
	if !bytes.HasPrefix(raw, []byte("{\n\"functions\": [\n{\n\"addr\": 142622720,\n")) {
		t.Errorf("manifest layout differs from json.dump(indent=0): %q", raw[:60])
	}
}

func TestGenerateRejectsGarbage(t *testing.T) {
	dir := t.TempDir()
	if err := Generate([]byte("not an image"), []byte("not an elf"), nil, dir, nil); err == nil {
		t.Fatal("garbage was accepted")
	}
	boot := synthPRX(synthWords)
	if err := Generate(make([]byte, 8), boot, nil, dir, nil); err == nil {
		t.Fatal("an image shorter than the code range was accepted")
	}
	boot2 := synthPRX(synthWords)
	img, _ := psp.BuildImage(boot2)
	if err := Generate(img, boot2, []byte("{not json"), dir, nil); err == nil {
		t.Fatal("a broken profile was accepted")
	}
}

func TestProgressMonotonic(t *testing.T) {
	boot := synthPRX(synthWords)
	img, _ := psp.BuildImage(boot)
	last, calls := -1, 0
	err := Generate(img, boot, nil, t.TempDir(), func(done, total int) {
		if done < last || done > total {
			t.Errorf("progress went from %d to %d of %d", last, done, total)
		}
		last = done
		calls++
	})
	if err != nil {
		t.Fatal(err)
	}
	if last != 1000 || calls == 0 {
		t.Errorf("progress ended at %d after %d calls", last, calls)
	}
}
