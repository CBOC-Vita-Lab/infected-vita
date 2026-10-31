package main

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"
)

// TestMain lets the test binary act as the "compiler" that the makefile recipes run: `<test binary> -helper OUTFILE [fail]`.
func TestMain(m *testing.M) {
	treeHelper()
	if len(os.Args) >= 3 && os.Args[1] == "-helper" {
		out := os.Args[2]
		running := out + ".running"
		os.WriteFile(running, nil, 0o644)
		time.Sleep(400 * time.Millisecond)
		n := 0
		if l, err := filepath.Glob(filepath.Join(filepath.Dir(out), "*.running")); err == nil {
			n = len(l) // how many helpers were alive at the same time
		}
		os.Remove(running)
		if len(os.Args) > 3 && os.Args[3] == "fail" {
			fmt.Fprintln(os.Stderr, "helper: failing on purpose")
			os.Exit(3)
		}
		os.WriteFile(out, []byte(fmt.Sprint(n)), 0o644)
		os.Exit(0)
	}
	os.Exit(m.Run())
}

// maxConcurrent is the largest number of helpers that were alive together, from the files they wrote.
func maxConcurrent(dir string) int {
	max := 0
	l, _ := filepath.Glob(filepath.Join(dir, "*.ltrans.o"))
	for _, f := range l {
		b, _ := os.ReadFile(f)
		var n int
		fmt.Sscan(string(b), &n)
		if n > max {
			max = n
		}
	}
	return max
}

// ltoMakefile is what GCC 15's lto-wrapper writes (copied from a real link, paths shortened), with this test binary as the compiler.
func ltoMakefile(dir string, n int, failIndex int) string {
	exe, _ := os.Executable()
	var b strings.Builder
	var outs []string
	for i := 0; i < n; i++ {
		out := filepath.Join(dir, fmt.Sprintf("ccAbCdEf.ltrans%d.ltrans.o", i))
		outs = append(outs, out)
		fail := ""
		if i == failIndex {
			fail = " 'fail'"
		}
		fmt.Fprintf(&b, "%s:\n\t@%s  '-helper' '%s'%s '-xlto' '-c' '-fno-openmp' '--param=lto-partitions=4' '-fltrans'\n", out, exe, out, fail)
	}
	b.WriteString(".PHONY: all\nall: \\\n")
	for i, o := range outs {
		b.WriteString("\t" + o)
		if i < len(outs)-1 {
			b.WriteString(" \\")
		}
		b.WriteString("\n")
	}
	return b.String()
}

func runMakeText(t *testing.T, text string, args ...string) (int, string, time.Duration) {
	t.Helper()
	dir := t.TempDir()
	mk := filepath.Join(dir, "cc.mk")
	os.WriteFile(mk, []byte(text), 0o644)
	var out, errb bytes.Buffer
	t0 := time.Now()
	code := runMake(append([]string{"-f", mk}, args...), &out, &errb, func(string) string { return "" })
	return code, out.String() + errb.String(), time.Since(t0)
}

func TestMakeParallel(t *testing.T) {
	dir := t.TempDir()
	text := ltoMakefile(dir, 4, -1)
	code, out, _ := runMakeText(t, text, "-j4", "all")
	if code != 0 {
		t.Fatalf("exit %d: %s", code, out)
	}
	for i := 0; i < 4; i++ {
		if _, err := os.Stat(filepath.Join(dir, fmt.Sprintf("ccAbCdEf.ltrans%d.ltrans.o", i))); err != nil {
			t.Errorf("partition %d not built", i)
		}
	}
	if n := maxConcurrent(dir); n < 3 {
		t.Errorf("with -j4 at most %d of 4 partitions ran at once", n)
	}
}

func TestMakeSerialAndJobsForms(t *testing.T) {
	for _, form := range [][]string{{"-j1", "all"}, {"-j", "1", "all"}, {"--jobs=1", "all"}, {"all"}} {
		dir := t.TempDir()
		code, out, _ := runMakeText(t, ltoMakefile(dir, 3, -1), form...)
		if code != 0 {
			t.Fatalf("%v: exit %d: %s", form, code, out)
		}
		if n := maxConcurrent(dir); n != 1 {
			t.Errorf("%v: %d partitions ran at once, want 1", form, n)
		}
	}
}

func TestMakeFailure(t *testing.T) {
	dir := t.TempDir()
	code, out, _ := runMakeText(t, ltoMakefile(dir, 4, 1), "-j2", "all")
	if code != 2 {
		t.Fatalf("exit %d, want 2", code)
	}
	if !strings.Contains(out, "make: *** [") || !strings.Contains(out, "ltrans1.ltrans.o] Error 3") || !strings.Contains(out, "failing on purpose") {
		t.Fatalf("output: %s", out)
	}
}

func TestMakeVersionAndMissing(t *testing.T) {
	var out, errb bytes.Buffer
	if code := runMake([]string{"--version"}, &out, &errb, func(string) string { return "" }); code != 0 || !strings.HasPrefix(out.String(), "GNU Make") {
		t.Fatalf("--version: %d %q", code, out.String())
	}
	out.Reset()
	errb.Reset()
	if code := runMake([]string{"-f", filepath.Join(t.TempDir(), "nope.mk"), "all"}, &out, &errb, func(string) string { return "" }); code != 2 {
		t.Fatalf("missing makefile: %d", code)
	}
	code, o, _ := runMakeText(t, "all: nothing\n", "all")
	if code != 2 || !strings.Contains(o, "No rule to make target 'nothing'") {
		t.Fatalf("%d %s", code, o)
	}
}

func TestMakeProgressFile(t *testing.T) {
	dir := t.TempDir()
	pf := filepath.Join(dir, "progress.txt")
	mk := filepath.Join(dir, "x.mk")
	os.WriteFile(mk, []byte(ltoMakefile(dir, 2, -1)), 0o644)
	var out bytes.Buffer
	code := runMake([]string{"-f", mk, "-j2", "all"}, &out, &out, func(k string) string {
		if k == progressFileEnv {
			return pf
		}
		return ""
	})
	data, _ := os.ReadFile(pf)
	if code != 0 || string(data) != "2 2\n" {
		t.Fatalf("exit %d, progress file %q (%s)", code, data, out.String())
	}
}

func TestParseMakefileShapes(t *testing.T) {
	text := "# comment\nCC = gcc\nOUT := /tmp/o\n\n/tmp/a.o /tmp/b.o: /tmp/a.c\n\t@$(CC) -c 'a b' $$HOME\n\t-false\n.PHONY: all\nall: \\\n\t/tmp/a.o \\\n\t/tmp/b.o\n"
	mf, err := parseMakefile(strings.NewReader(text), nil)
	if err != nil {
		t.Fatal(err)
	}
	if !mf.phony["all"] || mf.first != "/tmp/a.o" {
		t.Fatalf("phony %v first %q", mf.phony, mf.first)
	}
	r := mf.rules["/tmp/a.o"]
	if r == nil || len(r.recipe) != 2 || r.recipe[0] != "@gcc -c 'a b' $$HOME" || len(r.deps) != 1 {
		t.Fatalf("rule %+v", r)
	}
	if all := mf.rules["all"]; all == nil || strings.Join(all.deps, ",") != "/tmp/a.o,/tmp/b.o" {
		t.Fatalf("all: %+v", all)
	}
	if mf.countRecipes([]string{"all"}) != 2 { // one recipe per target of the shared rule
		t.Fatalf("recipes: %d", mf.countRecipes([]string{"all"}))
	}
	if _, err := parseMakefile(strings.NewReader("\techo hi\n"), nil); err == nil {
		t.Fatal("recipe before a target should fail")
	}
	cyc, _ := parseMakefile(strings.NewReader("a: b\nb: a\n"), nil)
	if !cyc.hasCycle([]string{"a"}) {
		t.Fatal("cycle not found")
	}
}

func TestDriveLetterColon(t *testing.T) {
	line := `C:\Users\me\Temp\cc1.ltrans.o: D:/dep/x.o`
	c := findColon(line)
	if c != strings.Index(line, ": ") {
		t.Fatalf("colon at %d in %q", c, line)
	}
	mf, err := parseMakefile(strings.NewReader("all: C:/t/a.o\nC:/t/a.o:\n\t@x\n"), nil)
	if err != nil || mf.rules["C:/t/a.o"] == nil || mf.rules["all"].deps[0] != "C:/t/a.o" {
		t.Fatalf("%v %+v", err, mf)
	}
}

func TestParseRecipeLine(t *testing.T) {
	cmds, ign, err := parseRecipeLine(`@-touch -r in.o in.o.tem > /dev/null 2>&1 && mv in.o.tem in.o`, false)
	if err != nil || !ign || len(cmds) != 2 || strings.Join(cmds[0].words, " ") != "touch -r in.o in.o.tem" || strings.Join(cmds[1].words, " ") != "mv in.o.tem in.o" {
		t.Fatalf("%v %v %+v", err, ign, cmds)
	}
	cmds, _, err = parseRecipeLine(`@/x/gcc  '-xlto' '-DA="b c"' "d e" f\ g '' 'it'\''s'`, false)
	want := []string{"/x/gcc", "-xlto", `-DA="b c"`, "d e", "f g", "", "it's"}
	if err != nil || len(cmds) != 1 || fmt.Sprint(cmds[0].words) != fmt.Sprint(want) {
		t.Fatalf("%v %q", err, cmds)
	}
	cmds, _, err = parseRecipeLine(`C:\kit\bin\gcc.exe '-o' 'C:\tmp\x.o'`, true)
	if err != nil || cmds[0].words[0] != `C:\kit\bin\gcc.exe` || cmds[0].words[2] != `C:\tmp\x.o` {
		t.Fatalf("windows: %v %q", err, cmds)
	}
	for _, line := range []string{`a | b`, `a || b`, "a; b", "a > out.txt", "echo `x`", "a < in"} {
		if _, _, err := parseRecipeLine(line, false); err != errNeedShell {
			t.Errorf("%q: got %v, want a shell", line, err)
		}
	}
	// a quoted && is an argument, not an operator
	cmds, _, _ = parseRecipeLine(`echo '&&' ">"`, false)
	if len(cmds) != 1 || len(cmds[0].words) != 3 {
		t.Fatalf("%q", cmds)
	}
	if _, _, err := parseRecipeLine(`echo 'oops`, false); err == nil {
		t.Fatal("unterminated quote")
	}
}

func TestBuiltins(t *testing.T) {
	dir := t.TempDir()
	in := filepath.Join(dir, "in.o")
	os.WriteFile(in, []byte("data"), 0o644)
	old := time.Unix(1_600_000_000, 0)
	os.Chtimes(in, old, old)
	rr := &recipeRunner{stdout: os.Stdout, stderr: os.Stderr, winBackslash: runtime.GOOS == "windows"}
	line := fmt.Sprintf("@-touch -r %s %s.tem > /dev/null 2>&1 && mv %s.tem %s", in, in, in, in)
	if err := rr.run(line); err != nil {
		t.Fatal(err)
	}
	st, err := os.Stat(in)
	if err != nil || st.Size() != 0 || !st.ModTime().Equal(old) {
		t.Fatalf("after touch/mv: %v %v", st, err)
	}
	if _, err := os.Stat(in + ".tem"); err == nil {
		t.Fatal(".tem left behind")
	}
	if err := rr.run("rm -f " + in); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(in); err == nil {
		t.Fatal("rm did not remove")
	}
}

func TestResolveProgramWithSpaces(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "John Smith", "kit")
	os.MkdirAll(dir, 0o755)
	prog := filepath.Join(dir, "gcc")
	os.WriteFile(prog, []byte("x"), 0o755)
	words := append(strings.Fields(prog), "-c", "x.c")
	p, rest := resolveProgram(words)
	if p != prog || len(rest) != 2 {
		t.Fatalf("%q %q", p, rest)
	}
	if runtime.GOOS != "windows" {
		if p, rest = resolveProgram([]string{"/no/such", "a"}); p != "/no/such" || len(rest) != 1 {
			t.Fatalf("%q %q", p, rest)
		}
	}
}

func TestShellFallback(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("no /bin/sh")
	}
	dir := t.TempDir()
	out := filepath.Join(dir, "o.txt")
	rr := &recipeRunner{stdout: os.Stdout, stderr: os.Stderr}
	if err := rr.run(fmt.Sprintf("echo hi | tr a-z A-Z > %s", out)); err != nil {
		t.Fatal(err)
	}
	if b, _ := os.ReadFile(out); strings.TrimSpace(string(b)) != "HI" {
		t.Fatalf("%q", b)
	}
}

func TestQuoteLeadingProgram(t *testing.T) {
	win := runtime.GOOS == "windows"
	for _, base := range []string{"John Smith", "O'Brien", "John Smith's folder"} {
		dir := filepath.Join(t.TempDir(), base, "Kit B (2)")
		os.MkdirAll(dir, 0o755)
		prog := filepath.Join(dir, "gcc")
		os.WriteFile(prog, []byte("x"), 0o755)
		line := prog + "  '-xlto' '-c' '-o' 'out file.o' '-fmap=" + strings.ReplaceAll(dir, "'", `'\''`) + "=/kit'"
		cmds, _, err := parseRecipeLine("@"+line, win)
		if err != nil || len(cmds) != 1 || len(cmds[0].words) != 6 || cmds[0].words[0] != prog || cmds[0].words[1] != "-xlto" || cmds[0].words[4] != "out file.o" ||
			cmds[0].words[5] != "-fmap="+dir+"=/kit" {
			t.Fatalf("%q: %v %q", base, err, cmds)
		}
	}
	// a plain path is left alone, and so is a line whose program does not exist
	if got := quoteLeadingProgram("/bin/sh -c 'x'", false); got != "/bin/sh -c 'x'" {
		t.Fatalf("%q", got)
	}
	if got := quoteLeadingProgram("/no/such dir/gcc 'x'", false); got != "/no/such dir/gcc 'x'" {
		t.Fatalf("%q", got)
	}
}

// GCC writes an apostrophe inside a quoted argument as quote, backslash, quote, quote on every system; on Windows a backslash outside quotes is otherwise an
// ordinary character.
func TestWindowsApostropheArgument(t *testing.T) {
	words, err := splitWords(`'-fmap=C:\Users\O'\''Brien\kit=/kit' 'C:\x y\z'`, true)
	if err != nil || len(words) != 2 || words[0].text != `-fmap=C:\Users\O'Brien\kit=/kit` || words[1].text != `C:\x y\z` {
		t.Fatalf("%v %+v", err, words)
	}
	words, err = splitWords(`'it'\''s'`, false)
	if err != nil || len(words) != 1 || words[0].text != "it's" {
		t.Fatalf("%v %+v", err, words)
	}
}
