package main

import (
	"bufio"
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
)

// buildLog is the file with the full output of every tool (<work>/build.log).
type buildLog struct {
	mu sync.Mutex
	f  *os.File
}

func openBuildLog(path string) (*buildLog, error) {
	f, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o644)
	if err != nil {
		return nil, err
	}
	return &buildLog{f: f}, nil
}

func (l *buildLog) Printf(format string, a ...interface{}) {
	if l == nil {
		return
	}
	l.mu.Lock()
	defer l.mu.Unlock()
	fmt.Fprintf(l.f, format+"\n", a...)
}

func (l *buildLog) Block(header string, out []byte) {
	if l == nil {
		return
	}
	l.mu.Lock()
	defer l.mu.Unlock()
	fmt.Fprintf(l.f, "$ %s\n", header)
	l.f.Write(out)
	if len(out) > 0 && out[len(out)-1] != '\n' {
		l.f.WriteString("\n")
	}
}

func (l *buildLog) Close() {
	if l != nil && l.f != nil {
		l.f.Close()
	}
}

// Runner starts the kit's tools. It never looks at PATH: tools are absolute paths, and the environment the tools see is
// built here from nothing (childEnv).
type Runner struct {
	Ctx     context.Context
	Log     *buildLog
	Env     []string
	LogPath string
}

// ToolError is a tool that ran and failed.
type ToolError struct {
	Label string
	Err   error
}

func (e *ToolError) Error() string { return e.Label + ": " + e.Err.Error() }
func (e *ToolError) Unwrap() error { return e.Err }

func (r *Runner) command(dir, tool string, args []string) *exec.Cmd {
	cmd := exec.CommandContext(r.Ctx, tool, args...)
	cmd.Dir = dir
	cmd.Env = r.Env
	setProcAttr(cmd)
	cmd.Cancel = func() error { return killTree(cmd.Process) }
	return cmd
}

func describe(tool string, args []string) string {
	parts := make([]string, 0, len(args)+1)
	parts = append(parts, tool)
	for _, a := range args {
		if len(a) > 200 {
			a = a[:200] + "..."
		}
		parts = append(parts, a)
	}
	return strings.Join(parts, " ")
}

// Run runs a tool, collecting its output into the log. label names the step in error messages.
func (r *Runner) Run(label, dir, tool string, args []string) error {
	cmd := r.command(dir, tool, args)
	var out bytes.Buffer
	cmd.Stdout, cmd.Stderr = &out, &out
	err := cmd.Run()
	r.Log.Block("cd "+dir+" && "+describe(tool, args), out.Bytes())
	if err != nil {
		if r.Ctx.Err() != nil {
			return r.Ctx.Err()
		}
		r.Log.Printf("-> %v", err)
		return &ToolError{Label: label, Err: err}
	}
	return nil
}

// RunLines runs a tool and calls fn with every line of its standard output (standard error goes to the log). The output itself is
// not logged: it can be long (nm).
func (r *Runner) RunLines(label, dir, tool string, args []string, fn func(line string)) error {
	cmd := r.command(dir, tool, args)
	var errOut bytes.Buffer
	cmd.Stderr = &errOut
	pipe, err := cmd.StdoutPipe()
	if err != nil {
		return err
	}
	if err := cmd.Start(); err != nil {
		r.Log.Printf("$ %s\n-> %v", describe(tool, args), err)
		return &ToolError{Label: label, Err: err}
	}
	sc := bufio.NewScanner(pipe)
	sc.Buffer(make([]byte, 1<<20), 1<<20)
	for sc.Scan() {
		fn(sc.Text())
	}
	io.Copy(io.Discard, pipe)
	werr := cmd.Wait()
	r.Log.Block("cd "+dir+" && "+describe(tool, args)+"   (output not logged)", errOut.Bytes())
	if werr != nil {
		if r.Ctx.Err() != nil {
			return r.Ctx.Err()
		}
		return &ToolError{Label: label, Err: werr}
	}
	return nil
}

// childEnv is the whole environment of the kit's programs: nothing is inherited, so PATH, VITASDK and friends cannot matter.
//   - PATH is the kit's own bin folder (some Windows programs look for their DLLs there; on other systems it is only a tidy value).
//   - TMPDIR/TEMP/TMP point into the work folder: the LTO link writes about 50 MB of temporary files.
//   - MAKE is this program (makeProgram): GCC runs the parallel part of the LTO link through $MAKE (make.go), and INFECTED_BUILD_MAKE
//     tells the copy that gets started that way to behave as make.
//   - PWD=/proc/self/cwd on Linux: see below.
func childEnv(kit *Kit, tmp, make, progressFile string, host func(string) string) []string {
	env := []string{
		"PATH=" + filepath.Join(kit.Dir, "toolchain", "bin"),
		"TMPDIR=" + tmp, "TEMP=" + tmp, "TMP=" + tmp,
		"LC_ALL=C", "LANG=C",
		makeModeEnv + "=1",
		progressFileEnv + "=" + progressFile,
	}
	if make != "" {
		env = append(env, "MAKE="+make)
	}
	if runtime.GOOS == "linux" {
		// GCC streams the absolute working directory into every LTO object and takes it from $PWD when that names the working
		// directory; this always does, and reads the same on every machine, so the objects do not depend on the work folder.
		env = append(env, "PWD=/proc/self/cwd")
	}
	for _, k := range []string{"SystemRoot", "SYSTEMROOT", "windir", "SystemDrive"} { // Windows programs need these
		if v := host(k); v != "" {
			env = append(env, k+"="+v)
		}
	}
	return env
}

const (
	makeModeEnv     = "INFECTED_BUILD_MAKE"
	progressFileEnv = "INFECTED_BUILD_PROGRESS_FILE"
)

var errCancelled = errors.New("cancelled")

func hasSpace(s string) bool { return strings.ContainsAny(s, " \t") }

// plainName: a name the kit's programs can use. Only Windows is picky (see plainPath).
func plainName(s string) bool { return runtime.GOOS != "windows" || isASCII(s) }

func isASCII(s string) bool {
	for i := 0; i < len(s); i++ {
		if s[i] >= 0x80 {
			return false
		}
	}
	return true
}

// plainPath returns a spelling of the folder p that the kit's programs can open. On Windows they are MinGW programs: arguments, environment
// and the makefile of the LTO link are in the ANSI code page and files are opened by ANSI names, so a folder with a letter outside plain
// ASCII ("C:\Users\José\kit") cannot be used as it is, and its 8.3 short name ("C:\Users\JOS~1\kit") can, where the volume has short
// names and where every letter is in the ANSI code page (the compiler turns a short name back into the long one: a letter outside the
// code page, like the Cyrillic of "C:\Users\Иван", is lost there). Elsewhere, or when p is plain already, it is p itself. ok is false
// when there is no usable spelling.
func plainPath(p string, windows bool, short func(string) string, ansi func(string) bool) (plain string, ok bool) {
	if !windows || isASCII(p) {
		return p, true
	}
	if s := short(p); isASCII(s) && ansi(p) {
		return s, true
	}
	return p, false
}

// makeProgram chooses the value of $MAKE: GCC's lto-wrapper silently skips make (and links serially, twice as slowly) when the
// path of make has a space in it, and players' folders do ("C:\Users\John Smith\..."). In order of preference:
//   - the path of this program, when it has no space;
//   - its path relative to the kit folder ("./infected-build"), because the link runs with the kit folder as working directory;
//   - a copy in tmpDir (a folder without a space);
//   - the 8.3 short name of the program (Windows);
//   - nothing: the link is then serial.
//
// copy makes the copy (nil: no copy possible), short returns the short name or its argument.
func makeProgram(self, kitDir, tmpDir string, copyTo func(dst string) error, short func(string) string) string {
	if !hasSpace(self) && plainName(self) {
		return self
	}
	if rel, err := filepath.Rel(kitDir, self); err == nil && !strings.HasPrefix(rel, "..") && !filepath.IsAbs(rel) && !hasSpace(rel) && plainName(rel) {
		return "./" + filepath.ToSlash(rel)
	}
	if tmpDir != "" && !hasSpace(tmpDir) && plainName(tmpDir) && copyTo != nil {
		name := "make"
		if runtime.GOOS == "windows" {
			name = "make.exe"
		}
		dst := filepath.Join(tmpDir, name)
		if copyTo(dst) == nil {
			return dst
		}
	}
	if s := short(self); s != self && !hasSpace(s) && plainName(s) {
		return s
	}
	return ""
}

// spacelessDir returns a folder to make temporary folders in whose path has no space (GCC's makefile for the parallel link cannot
// cope with spaces in the names of its temporary files), or "" when there is none. Windows temporary folders usually have the
// user's name in them: the short form of the name is tried first.
func spacelessDir(cands []string, short func(string) string, usable func(string) bool) string {
	for _, c := range cands {
		if c == "" {
			continue
		}
		for _, p := range []string{c, short(c)} {
			if !hasSpace(p) && plainName(p) && usable(p) {
				return p
			}
		}
	}
	return ""
}

func dirUsable(p string) bool {
	st, err := os.Stat(p)
	if err != nil || !st.IsDir() {
		return false
	}
	f, err := os.CreateTemp(p, ".ib-probe-*")
	if err != nil {
		return false
	}
	f.Close()
	os.Remove(f.Name())
	return true
}

// tempCandidates are the places to look for a folder without a space, in order.
func tempCandidates(getenv func(string) string) []string {
	return []string{os.TempDir(), "/tmp", "/var/tmp", getenv("ProgramData"), filepath.Join(getenv("SystemDrive")+string(filepath.Separator), "Users", "Public"),
		getenv("SystemDrive") + string(filepath.Separator)}
}
