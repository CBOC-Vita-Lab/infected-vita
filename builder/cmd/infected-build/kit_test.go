package main

import (
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const sampleKit = `{
 "format": 1, "name": "Infected-Vita-builder", "os": "linux", "arch": "amd64",
 "game": {"titleId": "INFC00001", "name": "Infected", "version": "01.00", "bootSha256": "abc"},
 "tools": {"cxx": "toolchain/bin/arm-vita-eabi-g++", "ar": "toolchain/bin/arm-vita-eabi-gcc-ar", "nm": "toolchain/bin/arm-vita-eabi-nm",
           "readelf": "toolchain/bin/arm-vita-eabi-readelf", "elfCreate": "toolchain/bin/vita-elf-create", "fself": "toolchain/bin/vita-make-fself"},
 "profile": "game/profile.json",
 "compile": {"tool": "cxx", "args": ["-O2", "-frandom-seed={stem}", "-ffile-prefix-map={kit}=/kit", "-c", "gen/{stem}.cpp", "-o", "obj/{stem}.o"], "cwd": "work",
             "sources": ["lean_[0-9]*.cpp", "lean_table.cpp"]},
 "archive": {"tool": "ar", "args": ["rcs", "{lib}", "{objs}"], "order": ["lean_[0-9]*.o", "lean_table.o"], "deleteFirst": true},
 "link": {"tool": "cxx", "argsFile": "game/link/link.args", "jobs": 2, "extraArgs": ["-ffile-prefix-map={work}=."]},
 "postLinkCheck": {"tool": "nm", "args": ["{out}"], "failIfLineMatches": "^ +w (pthread_|sched_)", "message": "weak"},
 "textPad": {"object": "game/link/obj/infected/src/text_pad.cpp.o", "hint": 0, "minFreeBytes": 8192,
             "check": {"tool": "readelf", "args": ["-lW", "{elf}"]}, "fix": {"source": "x", "tool": "cxx", "args": ["-DINFECTED_TEXT_PAD={pad}", "-o", "{padObject}"]}},
 "elfCreate": {"tool": "elfCreate", "args": ["{elf}", "{velf}"]},
 "fself": {"tool": "fself", "args": ["-c", "{velf}", "{eboot}"]},
 "vpk": {"zip": "infected.vpk", "entries": ["sce_sys/param.sfo", "eboot.bin"], "eboot": "eboot.bin"},
 "minDiskBytes": 700000000,
 "files": {"game/profile.json": {"sha256": "SHA", "size": 5}}
}`

func TestParseKit(t *testing.T) {
	k, err := ParseKit("/kit", []byte(sampleKit))
	if err != nil {
		t.Fatal(err)
	}
	if k.Game.BootSha256 != "abc" || k.Compile.Args[1] != "-frandom-seed={stem}" || k.Link.Jobs != 2 || k.MinDiskBytes != 700000000 {
		t.Fatalf("fields not read: %+v", k)
	}
	if k.Vpk.FromKit != "game/vpk/" {
		t.Fatalf("default fromKit: %q", k.Vpk.FromKit)
	}
	if got := k.Tool("cxx"); got != filepath.Join("/kit", "toolchain", "bin", "arm-vita-eabi-g++") {
		t.Fatalf("tool path %q", got)
	}
	if k.requiredMacOS() != 14 {
		t.Fatal("default macOS requirement should be 14")
	}
}

func TestParseKitErrors(t *testing.T) {
	cases := []struct{ name, json, want string }{
		{"not json", `{`, "damaged"},
		{"format", `{"format": 2}`, "format 2"},
		{"incomplete", `{"format": 1}`, "incomplete"},
		{"no tool", strings.Replace(sampleKit, `"fself": "toolchain/bin/vita-make-fself"`, `"x": "y"`, 1), `no tool "fself"`},
		{"bad regexp", strings.Replace(sampleKit, `^ +w (pthread_|sched_)`, `(`, 1), "pattern"},
	}
	for _, c := range cases {
		_, err := ParseKit("/kit", []byte(c.json))
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: got %v, want message with %q", c.name, err, c.want)
		}
		if err != nil {
			if _, ok := err.(*userError); !ok {
				t.Errorf("%s: not a userError", c.name)
			}
			if strings.Contains(err.Error(), "\n") {
				t.Errorf("%s: error has several lines", c.name)
			}
		}
	}
}

func TestLoadKitMissing(t *testing.T) {
	_, err := LoadKit(t.TempDir())
	if err == nil || !strings.Contains(err.Error(), "kit.json") {
		t.Fatalf("got %v", err)
	}
}

func TestExpand(t *testing.T) {
	v := Vars{"stem": "lean_007", "kit": "/k", "work": "/w"}
	got, err := v.ExpandArgs([]string{"-frandom-seed={stem}", "-Iinc", "gen/{stem}.cpp", "-ffile-prefix-map={kit}=/kit", "{objs}", "{work}"},
		map[string][]string{"objs": {"/w/obj/a.o", "/w/obj/b.o"}})
	if err != nil {
		t.Fatal(err)
	}
	want := []string{"-frandom-seed=lean_007", "-Iinc", "gen/lean_007.cpp", "-ffile-prefix-map=/k=/kit", "/w/obj/a.o", "/w/obj/b.o", "/w"}
	if strings.Join(got, "|") != strings.Join(want, "|") {
		t.Fatalf("got %q", got)
	}
	if _, err := v.Expand("{nope}"); err == nil || !strings.Contains(err.Error(), "{nope}") {
		t.Fatalf("unknown placeholder: %v", err)
	}
	// braces that are not placeholders stay
	if s, err := v.Expand("-DX={1}"); err != nil || s != "-DX={1}" {
		t.Fatalf("%q %v", s, err)
	}
}

func TestVerify(t *testing.T) {
	dir := t.TempDir()
	data := []byte("hello")
	sum := sha256.Sum256(data)
	js := strings.Replace(sampleKit, `"SHA"`, `"`+hex.EncodeToString(sum[:])+`"`, 1)
	k, err := ParseKit(dir, []byte(js))
	if err != nil {
		t.Fatal(err)
	}
	if err := k.Verify(false); err == nil || !strings.Contains(err.Error(), "game/profile.json is missing") {
		t.Fatalf("missing file: %v", err)
	}
	os.MkdirAll(filepath.Join(dir, "game"), 0o755)
	os.WriteFile(filepath.Join(dir, "game", "profile.json"), data, 0o644)
	if err := k.Verify(false); err == nil || !strings.Contains(err.Error(), "cxx") && !strings.Contains(err.Error(), "arm-vita-eabi-g++") {
		t.Fatalf("missing tool should be reported: %v", err)
	}
	os.MkdirAll(filepath.Join(dir, "toolchain", "bin"), 0o755)
	for _, n := range k.Tools {
		os.WriteFile(filepath.Join(dir, filepath.FromSlash(n)), []byte("x"), 0o755)
	}
	if err := k.Verify(true); err != nil {
		t.Fatal(err)
	}
	os.WriteFile(filepath.Join(dir, "game", "profile.json"), []byte("HELLO"), 0o644)
	if err := k.Verify(false); err != nil {
		t.Fatalf("same size passes the quick check: %v", err)
	}
	if err := k.Verify(true); err == nil || !strings.Contains(err.Error(), "checksum") {
		t.Fatalf("deep check should fail: %v", err)
	}
	os.WriteFile(filepath.Join(dir, "game", "profile.json"), []byte("hi"), 0o644)
	if err := k.Verify(false); err == nil || !strings.Contains(err.Error(), "wrong size") {
		t.Fatalf("size: %v", err)
	}
}

func TestLinkArgs(t *testing.T) {
	dir := t.TempDir()
	k, _ := ParseKit(dir, []byte(sampleKit))
	if _, err := k.LinkArgs(); err == nil {
		t.Fatal("missing link.args should fail")
	}
	os.MkdirAll(filepath.Join(dir, "game", "link"), 0o755)
	os.WriteFile(filepath.Join(dir, "game", "link", "link.args"), []byte("-o\n{out}\n\n{lean}\n-flto={jobs}\n"), 0o644)
	args, err := k.LinkArgs()
	if err != nil {
		t.Fatal(err)
	}
	if strings.Join(args, " ") != "-o {out} {lean} -flto={jobs} -ffile-prefix-map={work}=." {
		t.Fatalf("%q", args)
	}
}
