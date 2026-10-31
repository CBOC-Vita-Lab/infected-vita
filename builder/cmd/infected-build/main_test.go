package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestParseArgs(t *testing.T) {
	o, err := parseArgs([]string{"--out", "/o", "--jobs=3", "--progress", "--keep", "--work", "/w", "--kit", "/k", "--verify", "Infected.iso"})
	if err != nil {
		t.Fatal(err)
	}
	if o.OutDir != "/o" || o.Jobs != 3 || !o.Progress || !o.Keep || o.WorkDir != "/w" || o.KitDir != "/k" || !o.Verify || o.ISO != "Infected.iso" {
		t.Fatalf("%+v", o)
	}
	// the disc image first, single-dash forms
	o, err = parseArgs([]string{"disc.iso", "-out", "x", "-j", "2"})
	if err != nil || o.ISO != "disc.iso" || o.OutDir != "x" || o.Jobs != 2 {
		t.Fatalf("%+v %v", o, err)
	}
	// the installer's argument order
	o, err = parseArgs([]string{"--progress", "--out", "/c/out dir", "/c/my games/Infected.iso"})
	if err != nil || o.ISO != "/c/my games/Infected.iso" || o.OutDir != "/c/out dir" {
		t.Fatalf("%+v %v", o, err)
	}
	for _, bad := range [][]string{{"--jobs", "0", "x.iso"}, {"--jobs", "many", "x.iso"}, {"--out"}, {"--bogus", "x.iso"}, {"a.iso", "b.iso"}} {
		if _, err := parseArgs(bad); err == nil {
			t.Errorf("%v: expected an error", bad)
		} else if strings.Contains(err.Error(), "\n") {
			t.Errorf("%v: several lines", bad)
		}
	}
	if o, _ := parseArgs([]string{"--help"}); !o.Help {
		t.Error("--help")
	}
}

func TestDefaultJobs(t *testing.T) {
	const gib = int64(1) << 30
	cases := []struct {
		cpus int
		ram  int64
		want int
	}{
		{8, 32 * gib, 8}, {8, 6 * gib, 4}, {8, 4 * gib, 2}, {2, 7 * gib, 2}, {4, 2 * gib, 1}, {4, gib / 2, 1}, {4, 0, 4}, {1, 64 * gib, 1}, {16, 24 * gib, 16},
	}
	for _, c := range cases {
		if got := defaultJobs(c.cpus, c.ram); got != c.want {
			t.Errorf("cpus %d ram %d GiB: got %d, want %d", c.cpus, c.ram/gib, got, c.want)
		}
	}
}

func TestCleanDroppedPath(t *testing.T) {
	for in, want := range map[string]string{
		"  C:\\My Games\\Infected.iso \r\n": `C:\My Games\Infected.iso`,
		`"C:\My Games\Infected.iso"` + "\n": `C:\My Games\Infected.iso`,
		"'/home/me/My Games/x.iso'\n":       "/home/me/My Games/x.iso",
		"/home/me/My\\ Games/x.iso\n":       "/home/me/My Games/x.iso",
	} {
		got := cleanDroppedPath(in)
		if filepath.Separator == '\\' {
			continue
		}
		if strings.Contains(in, `\ `) || !strings.Contains(in, `C:\`) {
			if got != want {
				t.Errorf("%q: got %q want %q", in, got, want)
			}
		}
	}
	if got := cleanDroppedPath(`"C:\a b\c.iso"` + "\r\n"); got != `C:\a b\c.iso` {
		t.Errorf("quoted windows path: %q", got)
	}
}

func TestMacOSCheck(t *testing.T) {
	if err := checkMacOS(12, 14); err == nil || !strings.HasPrefix(err.Error(), "Building Infected needs macOS 14 or newer on this Mac") || strings.Contains(err.Error(), "\n") {
		t.Fatalf("%v", err)
	}
	if checkMacOS(14, 14) != nil || checkMacOS(15, 14) != nil || checkMacOS(0, 14) != nil {
		t.Fatal("14 and newer, or unknown, must pass")
	}
	if parseMajor("14.2.1\n") != 14 || parseMajor("10.15.7") != 10 || parseMajor("") != 0 || parseMajor("26.0") != 26 {
		t.Fatal("parseMajor")
	}
}

func TestParseVMStat(t *testing.T) {
	out := "Mach Virtual Memory Statistics: (page size of 16384 bytes)\nPages free:                               10000.\nPages active:                            500000.\nPages inactive:                           20000.\nPages speculative:                         5000.\n"
	if got := parseVMStat(out); got != 35000*16384 {
		t.Fatalf("%d", got)
	}
}

func TestRoomFromLoads(t *testing.T) {
	// readelf -lW: LOAD Offset VirtAddr PhysAddr FileSiz MemSiz Flg Align
	l0 := strings.Fields("0x000094 0x81000000 0x81000000 0x4d6a24 0x4d6a24 R E 0x1000")
	l1 := strings.Fields("0x4d6ab8 0x814e0000 0x814e0000 0x01a000 0x2b4000 RW 0x1000")
	got, err := roomFromLoads([][]string{l0, l1})
	if err != nil || got != 0x814e0000-(0x81000000+0x4d6a24) {
		t.Fatalf("%d %v", got, err)
	}
	if got, _ := roomFromLoads([][]string{l0}); got != 1<<20 {
		t.Fatalf("single segment: %d", got)
	}
	if _, err := roomFromLoads([][]string{{"x"}, {"y"}}); err == nil {
		t.Fatal("garbage")
	}
}

// ---- reading a disc

func dirRecord(name string, lba, size uint32, isDir bool) []byte {
	n := []byte(name)
	l := 33 + len(n)
	if l%2 == 1 {
		l++
	}
	rec := make([]byte, l)
	rec[0] = byte(l)
	binary.LittleEndian.PutUint32(rec[2:], lba)
	binary.BigEndian.PutUint32(rec[6:], lba)
	binary.LittleEndian.PutUint32(rec[10:], size)
	binary.BigEndian.PutUint32(rec[14:], size)
	if isDir {
		rec[25] = 2
	}
	rec[32] = byte(len(n))
	copy(rec[33:], n)
	return rec
}

func makeSFO(discID string) []byte {
	key := "DISC_ID\x00"
	val := append([]byte(discID), 0)
	b := make([]byte, 20+16+len(key)+len(val))
	copy(b, "\x00PSF")
	binary.LittleEndian.PutUint32(b[4:], 0x101)
	binary.LittleEndian.PutUint32(b[8:], 36)
	binary.LittleEndian.PutUint32(b[12:], uint32(36+len(key)))
	binary.LittleEndian.PutUint32(b[16:], 1)
	binary.LittleEndian.PutUint16(b[20:], 0)
	binary.LittleEndian.PutUint16(b[22:], 0x0204)
	binary.LittleEndian.PutUint32(b[24:], uint32(len(val)))
	binary.LittleEndian.PutUint32(b[28:], uint32(len(val)))
	binary.LittleEndian.PutUint32(b[32:], 0)
	copy(b[36:], key)
	copy(b[36+len(key):], val)
	return b
}

func fakeISO(boot, sfo []byte) []byte {
	const sec = 2048
	img := make([]byte, 30*sec)
	pvd := img[16*sec:]
	pvd[0] = 1
	copy(pvd[1:], "CD001")
	copy(pvd[156:], dirRecord("\x00", 20, sec, true))
	put := func(lba int, recs ...[]byte) {
		o := lba * sec
		for _, r := range recs {
			copy(img[o:], r)
			o += len(r)
		}
	}
	put(20, dirRecord("\x00", 20, sec, true), dirRecord("PSP_GAME", 21, sec, true))
	put(21, dirRecord("\x00", 21, sec, true), dirRecord("SYSDIR", 22, sec, true), dirRecord("PARAM.SFO;1", 23, uint32(len(sfo)), false))
	put(22, dirRecord("\x00", 22, sec, true), dirRecord("BOOT.BIN;1", 24, uint32(len(boot)), false))
	copy(img[23*sec:], sfo)
	copy(img[24*sec:], boot)
	return img
}

func testBuilder(t *testing.T, isoBytes []byte) *Builder {
	t.Helper()
	dir := t.TempDir()
	iso := filepath.Join(dir, "d.iso")
	os.WriteFile(iso, isoBytes, 0o644)
	k, err := ParseKit(dir, []byte(sampleKit))
	if err != nil {
		t.Fatal(err)
	}
	return &Builder{Opt: Options{ISO: iso}, kit: k}
}

func TestReadDiscErrors(t *testing.T) {
	boot := []byte("not the real BOOT.BIN")
	cases := []struct {
		name string
		iso  []byte
		want string
	}{
		{"other region", fakeISO(boot, makeSFO("ULES-00001")), "This is not the US version of Infected (ULUS-10054, 1.02). Your disc is ULES-00001."},
		{"other release", fakeISO(boot, makeSFO("ULUS-10054")), "not the 1.02 release"},
		{"no sfo", fakeISO(boot, []byte("x")), "This is not the US version of Infected (ULUS-10054, 1.02)."},
		{"garbage", bytes.Repeat([]byte("x"), 80000), "does not look like a PSP disc image"},
		{"cso", append([]byte("CISO"), make([]byte, 100)...), "compressed (.cso)"},
	}
	for _, c := range cases {
		b := testBuilder(t, c.iso)
		sum := sha256.Sum256([]byte("something else"))
		b.kit.Game.BootSha256 = hex.EncodeToString(sum[:])
		_, _, err := b.readDisc()
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: got %v, want %q", c.name, err, c.want)
			continue
		}
		if strings.Contains(err.Error(), "\n") {
			t.Errorf("%s: several lines", c.name)
		}
	}
}

func TestParseSFO(t *testing.T) {
	if kv := parseSFO(makeSFO("ULUS-10054")); kv["DISC_ID"] != "ULUS-10054" {
		t.Fatalf("%v", kv)
	}
	if parseSFO([]byte("nope")) != nil || parseSFO(nil) != nil {
		t.Fatal("garbage")
	}
}

func TestPrepareErrors(t *testing.T) {
	// no kit next to the program
	b := &Builder{Opt: Options{ISO: "x.iso", KitDir: t.TempDir()}, Self: "/none/infected-build", Rep: NewReporter(&bytes.Buffer{}, true, false, 5)}
	_, err := b.Run(context.Background())
	if err == nil || !strings.Contains(err.Error(), "builder kit is missing") {
		t.Fatalf("%v", err)
	}
}

func TestWriteVPK(t *testing.T) {
	dir := t.TempDir()
	os.MkdirAll(filepath.Join(dir, "game", "vpk", "sce_sys"), 0o755)
	os.WriteFile(filepath.Join(dir, "game", "vpk", "sce_sys", "param.sfo"), []byte("sfo"), 0o644)
	eboot := filepath.Join(dir, "eboot.bin")
	os.WriteFile(eboot, bytes.Repeat([]byte("E"), 5000), 0o644)
	k, _ := ParseKit(dir, []byte(sampleKit))
	out1, out2 := filepath.Join(dir, "a.vpk"), filepath.Join(dir, "b.vpk")
	if err := writeVPK(k, eboot, out1); err != nil {
		t.Fatal(err)
	}
	if err := writeVPK(k, eboot, out2); err != nil {
		t.Fatal(err)
	}
	a, _ := os.ReadFile(out1)
	b, _ := os.ReadFile(out2)
	if !bytes.Equal(a, b) {
		t.Fatal("the package is not reproducible")
	}
	if _, err := os.Stat(out1 + ".tmp"); err == nil {
		t.Fatal("temporary file left")
	}
	zr, err := openZip(out1)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Join(zr, ",") != "sce_sys/param.sfo,eboot.bin" {
		t.Fatalf("entries %v", zr)
	}
}

func TestMakeProgram(t *testing.T) {
	short := func(s string) string { return s }
	copied := ""
	cp := func(dst string) error { copied = dst; return nil }
	kit := filepath.Join(string(filepath.Separator), "k", "Kit")
	if got := makeProgram(filepath.Join("/opt", "kit", "infected-build"), "/opt/kit", "/t", cp, short); got != filepath.Join("/opt", "kit", "infected-build") {
		t.Fatalf("plain path: %q", got)
	}
	// a space in the path of the program: relative to the kit folder
	self := filepath.Join(kit+" two", "infected-build")
	if got := makeProgram(self, kit+" two", "/t", cp, short); got != "./infected-build" {
		t.Fatalf("relative: %q", got)
	}
	// the program is not in the kit folder: a copy in a folder without a space
	if got := makeProgram(self, "/elsewhere", "/t", cp, short); got != filepath.Join("/t", "make") && got != filepath.Join("/t", "make.exe") || copied == "" {
		t.Fatalf("copy: %q %q", got, copied)
	}
	// nothing works but the short name
	if got := makeProgram(self, "/elsewhere", "/t x", cp, func(s string) string { return "/SHORT~1/ib" }); got != "/SHORT~1/ib" {
		t.Fatalf("short: %q", got)
	}
	if got := makeProgram(self, "/elsewhere", "/t x", cp, short); got != "" {
		t.Fatalf("none: %q", got)
	}
}

func TestSpacelessDir(t *testing.T) {
	ok := func(string) bool { return true }
	short := func(s string) string { return strings.ReplaceAll(s, "John Smith", "JOHNSM~1") }
	if got := spacelessDir([]string{"/Users/John Smith/Temp", "/tmp"}, short, ok); got != "/Users/JOHNSM~1/Temp" {
		t.Fatalf("short form: %q", got)
	}
	if got := spacelessDir([]string{"/a b", "", "/c"}, func(s string) string { return s }, ok); got != "/c" {
		t.Fatalf("next candidate: %q", got)
	}
	if got := spacelessDir([]string{"/a b"}, func(s string) string { return s }, ok); got != "" {
		t.Fatalf("none: %q", got)
	}
	if got := spacelessDir([]string{"/x", "/y"}, func(s string) string { return s }, func(p string) bool { return p == "/y" }); got != "/y" {
		t.Fatalf("usable: %q", got)
	}
}

func TestChildEnv(t *testing.T) {
	k, _ := ParseKit("/kit", []byte(sampleKit))
	env := childEnv(k, "/w/tmp", "/kit/infected-build", "/w/tmp/p.txt", func(s string) string {
		return map[string]string{"SystemRoot": `C:\Windows`}[s]
	})
	m := map[string]string{}
	for _, e := range env {
		kv := strings.SplitN(e, "=", 2)
		m[kv[0]] = kv[1]
	}
	if m["TMPDIR"] != "/w/tmp" || m["TEMP"] != "/w/tmp" || m["TMP"] != "/w/tmp" || m["MAKE"] != "/kit/infected-build" || m[makeModeEnv] != "1" ||
		m["PATH"] != filepath.Join("/kit", "toolchain", "bin") || m["SystemRoot"] != `C:\Windows` || m[progressFileEnv] != "/w/tmp/p.txt" {
		t.Fatalf("%v", m)
	}
	for _, bad := range []string{"VITASDK", "HOME", "USER"} {
		if _, ok := m[bad]; ok {
			t.Errorf("%s leaked into the environment of the tools", bad)
		}
	}
	// no usable make: MAKE is not set at all (the linker then has none and links serially)
	for _, e := range childEnv(k, "/w/tmp", "", "/p", func(string) string { return "" }) {
		if strings.HasPrefix(e, "MAKE=") {
			t.Fatalf("MAKE set: %s", e)
		}
	}
}

func TestGenOption(t *testing.T) {
	o, err := parseArgs([]string{"--gen", "/g", "--work", "/w", "--out", "/o", "--jobs", "2", "--keep"})
	if err != nil || o.GenDir != "/g" || o.ISO != "" || o.OutDir != "/o" {
		t.Fatalf("%+v %v", o, err)
	}
	if !strings.Contains(usageText, "--gen") {
		t.Fatal("--help must mention --gen (CI looks for it)")
	}
	// a folder without generated code is refused with one plain line
	kitDir := t.TempDir()
	b := &Builder{Opt: Options{GenDir: t.TempDir(), KitDir: kitDir}, Self: filepath.Join(kitDir, "infected-build")}
	// make a minimal valid kit.json so that prepare gets as far as the generated code check
	js := strings.Replace(sampleKit, `"files": {"game/profile.json": {"sha256": "SHA", "size": 5}}`, `"files": {}`, 1)
	os.WriteFile(filepath.Join(kitDir, "kit.json"), []byte(js), 0o644)
	os.MkdirAll(filepath.Join(kitDir, "toolchain", "bin"), 0o755)
	k, _ := ParseKit(kitDir, []byte(js))
	for _, n := range k.Tools {
		os.WriteFile(filepath.Join(kitDir, filepath.FromSlash(n)), []byte("x"), 0o755)
	}
	_, err = b.Run(context.Background())
	if err == nil || !strings.Contains(err.Error(), "has no generated code") {
		t.Fatalf("%v", err)
	}
}

func TestStageGenerated(t *testing.T) {
	gen, work, kitDir := t.TempDir(), t.TempDir(), t.TempDir()
	for _, n := range []string{"lean_000.cpp", "lean_table.cpp", "lean_decls.h", "lean_manifest.json", "other.txt"} {
		os.WriteFile(filepath.Join(gen, n), []byte(n), 0o644)
	}
	os.MkdirAll(filepath.Join(kitDir, "game", "include"), 0o755)
	os.WriteFile(filepath.Join(kitDir, "game", "include", "lean_abi.h"), []byte("h"), 0o644)
	os.MkdirAll(filepath.Join(work, "gen"), 0o755)
	os.MkdirAll(filepath.Join(work, "inc"), 0o755)
	k, _ := ParseKit(kitDir, []byte(sampleKit))
	b := &Builder{Opt: Options{GenDir: gen}, kit: k, work: work}
	if err := b.stageGenerated(context.Background()); err != nil {
		t.Fatal(err)
	}
	for _, n := range []string{"lean_000.cpp", "lean_table.cpp", "lean_decls.h"} {
		if _, err := os.Stat(filepath.Join(work, "gen", n)); err != nil {
			t.Errorf("%s not staged", n)
		}
	}
	for _, n := range []string{"lean_manifest.json", "other.txt"} {
		if _, err := os.Stat(filepath.Join(work, "gen", n)); err == nil {
			t.Errorf("%s should not be staged", n)
		}
	}
	if _, err := os.Stat(filepath.Join(work, "inc", "lean_abi.h")); err != nil {
		t.Error("kit headers not staged")
	}
}
