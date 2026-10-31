package main

// A small stand-in for GNU make, just big enough for the one makefile that GCC's lto-wrapper writes.
//
// Why: the final LTO link has a serial first half (whole-program analysis) and a parallel second half (the "ltrans" partitions,
// each a full compile of a slice of the program). lto-wrapper runs the second half through `make -f FILE -j N all`, taking make
// from $MAKE or else from PATH; without a make it runs the partitions one after the other (about twice as slow on two cores).
// Players' computers have no make, so the builder sets MAKE to itself (runner.go childEnv) and, when it is started that way
// (INFECTED_BUILD_MAKE=1 in its environment), runs as this make.
//
// The makefile has this shape (GCC 15, lto-wrapper.cc run_gcc):
//
//	/path/to/ccXXXX.ltrans0.ltrans.o:
//	<TAB>@/path/to/arm-vita-eabi-gcc '-xlto' '-c' ... '-fltrans' '-o' '/path/to/ccXXXX.ltrans0.ltrans.o' '/path/to/ccXXXX.ltrans0.o' -truncate '...'
//	.PHONY: all
//	all: \
//	<TAB>/path/to/ccXXXX.ltrans0.ltrans.o \
//	<TAB>/path/to/ccXXXX.ltrans1.ltrans.o
//
// (GCC before 15 appended `@-touch -r IN IN.tem >/dev/null 2>&1 && mv IN.tem IN` to each rule; that is understood too.)
// Supported: rules with targets, prerequisites and recipe lines (@ - + prefixes), `.PHONY`, line continuations, comments, simple
// variables, -f, -j, -C. Every rule that has a recipe runs (as with `make -B`): the outputs never exist beforehand.

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
)

type makeRule struct {
	targets []string
	deps    []string
	recipe  []string
}

type makefile struct {
	rules map[string]*makeRule
	phony map[string]bool
	first string
}

// isDriveColon reports whether the colon at s[i] belongs to a Windows drive letter ("C:/dir", "C:\dir"), as GNU make's DOS
// handling does, and so does not separate targets from prerequisites.
func isDriveColon(s string, i int) bool {
	if i < 1 || i+1 >= len(s) || (s[i+1] != '/' && s[i+1] != '\\') {
		return false
	}
	c := s[i-1]
	if !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
		return false
	}
	return i == 1 || s[i-2] == ' ' || s[i-2] == '\t'
}

func findColon(s string) int {
	for i := 0; i < len(s); i++ {
		if s[i] == ':' && !isDriveColon(s, i) {
			return i
		}
	}
	return -1
}

// logicalLines joins lines ending in a backslash with the next one.
func logicalLines(r io.Reader) ([]string, error) {
	sc := bufio.NewScanner(r)
	sc.Buffer(make([]byte, 1<<20), 16<<20)
	var out []string
	var cur string
	cont := false
	for sc.Scan() {
		l := strings.TrimRight(sc.Text(), "\r")
		if cont {
			l = " " + strings.TrimLeft(l, " \t")
		}
		if strings.HasSuffix(l, "\\") && (len(l)-len(strings.TrimRight(l, "\\")))%2 == 1 {
			cur += strings.TrimSuffix(l, "\\")
			cont = true
			continue
		}
		out = append(out, cur+l)
		cur, cont = "", false
	}
	if cont {
		out = append(out, cur)
	}
	return out, sc.Err()
}

// expandVars replaces $(NAME), ${NAME} and $N; `$$` becomes `$` unless keepDollars (recipe lines keep it until they run).
func expandVars(s string, vars map[string]string, keepDollars bool) string {
	if !strings.Contains(s, "$") {
		return s
	}
	var b strings.Builder
	for i := 0; i < len(s); i++ {
		if s[i] != '$' || i+1 >= len(s) {
			b.WriteByte(s[i])
			continue
		}
		switch n := s[i+1]; {
		case n == '$':
			b.WriteByte('$')
			if keepDollars {
				b.WriteByte('$')
			}
			i++
		case n == '(' || n == '{':
			end := strings.IndexByte(s[i+2:], map[byte]byte{'(': ')', '{': '}'}[n])
			if end < 0 {
				b.WriteByte('$')
				continue
			}
			b.WriteString(vars[s[i+2:i+2+end]])
			i += end + 2
		default:
			b.WriteString(vars[string(n)])
			i++
		}
	}
	return b.String()
}

func parseMakefile(r io.Reader, cmdVars map[string]string) (*makefile, error) {
	lines, err := logicalLines(r)
	if err != nil {
		return nil, err
	}
	mf := &makefile{rules: map[string]*makeRule{}, phony: map[string]bool{}}
	vars := map[string]string{}
	for k, v := range cmdVars {
		vars[k] = v
	}
	var cur *makeRule
	for n, raw := range lines {
		if strings.HasPrefix(raw, "\t") {
			if cur == nil {
				return nil, fmt.Errorf("line %d: commands commence before first target", n+1)
			}
			cur.recipe = append(cur.recipe, expandVars(strings.TrimPrefix(raw, "\t"), vars, true))
			continue
		}
		line := raw
		if i := strings.IndexByte(line, '#'); i >= 0 {
			line = line[:i]
		}
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		if i := strings.IndexAny(line, "=:"); i > 0 && (line[i] == '=' || strings.HasPrefix(line[i:], ":=")) {
			name := strings.TrimSpace(line[:i])
			val := strings.TrimSpace(strings.TrimPrefix(strings.TrimPrefix(line[i:], ":"), "="))
			if _, fixed := cmdVars[name]; !fixed {
				vars[name] = expandVars(val, vars, false)
			}
			cur = nil
			continue
		}
		line = expandVars(line, vars, false)
		c := findColon(line)
		if c < 0 {
			return nil, fmt.Errorf("line %d: missing separator", n+1)
		}
		targets := strings.Fields(line[:c])
		rest := strings.TrimLeft(line[c+1:], ":")
		if i := strings.IndexByte(rest, '|'); i >= 0 {
			rest = rest[:i] // order-only prerequisites are ordinary ones here
		}
		deps := strings.Fields(rest)
		if len(targets) == 1 && targets[0] == ".PHONY" {
			for _, d := range deps {
				mf.phony[d] = true
			}
			cur = nil
			continue
		}
		if len(targets) > 0 && strings.HasPrefix(targets[0], ".") && !strings.Contains(targets[0], "/") && !strings.Contains(targets[0], string(filepath.Separator)) && len(targets[0]) > 1 && strings.ToUpper(targets[0]) == targets[0] {
			cur = nil // other special targets (.SUFFIXES, .DEFAULT, ...) mean nothing here
			continue
		}
		rule := &makeRule{targets: targets, deps: deps}
		for _, t := range targets {
			if old := mf.rules[t]; old != nil && (len(old.deps) > 0 || len(old.recipe) > 0) && len(deps) == 0 {
				continue
			}
			if old := mf.rules[t]; old != nil {
				rule.deps = append(append([]string{}, old.deps...), rule.deps...)
				rule.recipe = append(append([]string{}, old.recipe...), rule.recipe...)
			}
			mf.rules[t] = rule
			if mf.first == "" {
				mf.first = t
			}
		}
		cur = rule
	}
	return mf, nil
}

// ---------------------------------------------------------------------------------------------------- recipe lines

type shellWord struct {
	text   string
	quoted bool // any part of it was quoted or escaped: an operator is only an operator when unquoted
}

// splitWords splits a command line like a POSIX shell (single and double quotes, backslash escapes); on Windows a backslash
// outside quotes is an ordinary character, because paths are written with them.
func splitWords(s string, winBackslash bool) ([]shellWord, error) {
	var out []shellWord
	var cur strings.Builder
	have, quoted := false, false
	flush := func() {
		if have {
			out = append(out, shellWord{cur.String(), quoted})
		}
		cur.Reset()
		have, quoted = false, false
	}
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c == ' ' || c == '\t' || c == '\n':
			flush()
		case c == '\'':
			j := strings.IndexByte(s[i+1:], '\'')
			if j < 0 {
				return nil, errors.New("unterminated quote")
			}
			cur.WriteString(s[i+1 : i+1+j])
			i += j + 1
			have, quoted = true, true
		case c == '"':
			have, quoted = true, true
			i++
			for ; i < len(s) && s[i] != '"'; i++ {
				if s[i] == '\\' && i+1 < len(s) && strings.IndexByte("\"\\$`", s[i+1]) >= 0 && !winBackslash {
					i++
				}
				cur.WriteByte(s[i])
			}
			if i >= len(s) {
				return nil, errors.New("unterminated quote")
			}
		case c == '\\' && winBackslash && i > 0 && s[i-1] == '\'' && i+2 < len(s) && s[i+1] == '\'' && s[i+2] == '\'':
			// GCC writes an apostrophe inside a quoted argument as quote, backslash, quote, quote even on Windows: close, escaped apostrophe, open again
			cur.WriteByte('\'')
			i++ // the escaped apostrophe; the next one opens the quote again
			have, quoted = true, true
		case c == '\\' && !winBackslash:
			if i+1 < len(s) {
				i++
				cur.WriteByte(s[i])
			}
			have, quoted = true, true
		default:
			cur.WriteByte(c)
			have = true
		}
	}
	flush()
	return out, nil
}

// simpleCommand is one program (or builtin) with its arguments.
type simpleCommand struct{ words []string }

var errNeedShell = errors.New("needs a shell")

// parseRecipeLine turns a recipe line into commands joined by &&. Redirections to /dev/null are dropped (the output goes to
// the log anyway); anything else a shell would do reports errNeedShell.
func parseRecipeLine(line string, winBackslash bool) (cmds []simpleCommand, ignoreErr bool, err error) {
	line = strings.TrimLeft(line, " \t")
	for len(line) > 0 && strings.IndexByte("@-+", line[0]) >= 0 {
		if line[0] == '-' {
			ignoreErr = true
		}
		line = strings.TrimLeft(line[1:], " \t")
	}
	line = strings.ReplaceAll(line, "$$", "$")
	line = quoteLeadingProgram(line, winBackslash)
	words, err := splitWords(line, winBackslash)
	if err != nil {
		return nil, ignoreErr, err
	}
	var cur []string
	for i := 0; i < len(words); i++ {
		w := words[i]
		if !w.quoted {
			switch w.text {
			case "&&":
				if len(cur) == 0 {
					return nil, ignoreErr, errNeedShell
				}
				cmds = append(cmds, simpleCommand{cur})
				cur = nil
				continue
			case "||", ";", "|", "<", "&", "(", ")":
				return nil, ignoreErr, errNeedShell
			case "2>&1", ">&2", "1>&2":
				continue
			case ">", ">>", "2>", "1>":
				if i+1 < len(words) && (words[i+1].text == "/dev/null" || strings.EqualFold(words[i+1].text, "nul")) {
					i++
					continue
				}
				return nil, ignoreErr, errNeedShell
			case ">/dev/null", "2>/dev/null", "1>/dev/null", "&>/dev/null":
				continue
			}
			metas := ";|<>&`"
			if !winBackslash {
				metas += "()"
			}
			if strings.ContainsAny(w.text, metas) || strings.Contains(w.text, "$(") {
				return nil, ignoreErr, errNeedShell
			}
		}
		cur = append(cur, w.text)
	}
	if len(cur) > 0 {
		cmds = append(cmds, simpleCommand{cur})
	}
	return cmds, ignoreErr, nil
}

// isFile reports whether path names an existing file. Windows ignores trailing spaces and dots in a name ("C:\kit\gcc " finds
// "C:\kit\gcc"), so a name that ends in one is never taken as the name of a program: otherwise the longest candidate ending at a
// space would swallow the space after the program.
func isFile(path string) bool {
	if path == "" || strings.HasSuffix(path, " ") || strings.HasSuffix(path, ".") || strings.HasSuffix(path, "\t") {
		return false
	}
	st, err := os.Stat(path)
	return err == nil && !st.IsDir()
}

// quoteLeadingProgram puts quotes around the program of a command line when its path has spaces, parentheses or an apostrophe and is
// not quoted, as GCC writes it ("/home/John Smith/kit (2)/toolchain/bin/arm-vita-eabi-gcc  '-xlto' ...): the longest start of the line,
// ending at a space, that is an existing file. (A start that begins with a quote is never a file, so a quoted program is left alone;
// the quotes of the arguments after an unquoted program do not matter, as an apostrophe in the folder name shows: "/home/O'Brien/kit/gcc".)
func quoteLeadingProgram(line string, winBackslash bool) string {
	best := -1
	for i := 1; i < len(line); i++ {
		if (line[i] == ' ' || line[i] == '\t') && isFile(line[:i]) {
			best = i
		}
	}
	if best < 0 || !strings.ContainsAny(line[:best], " \t()'\"") {
		return line
	}
	prog := line[:best]
	if winBackslash {
		// no backslash escapes here (they are the separators of paths); a Windows file name has no double quote
		if strings.Contains(prog, "'") {
			return "\"" + prog + "\"" + line[best:]
		}
		return "'" + prog + "'" + line[best:]
	}
	return "'" + strings.ReplaceAll(prog, "'", "'\\''") + "'" + line[best:]
}

// resolveProgram finds the program of a command whose path has spaces in it and is not quoted (as GCC writes it): the longest
// leading run of words that names an existing file.
func resolveProgram(words []string) (string, []string) {
	if _, err := os.Stat(words[0]); err == nil || len(words) == 1 {
		return words[0], words[1:]
	}
	p := words[0]
	for i := 1; i < len(words); i++ {
		p += " " + words[i]
		if isFile(p) {
			return p, words[i+1:]
		}
	}
	return words[0], words[1:]
}

type recipeRunner struct {
	stdout, stderr io.Writer
	winBackslash   bool
}

// run executes a recipe line.
func (rr *recipeRunner) run(line string) error {
	cmds, ignoreErr, err := parseRecipeLine(line, rr.winBackslash)
	if errors.Is(err, errNeedShell) {
		return rr.runShell(line, ignoreErr || strings.HasPrefix(strings.TrimLeft(line, " \t@"), "-"))
	}
	if err != nil {
		return err
	}
	for _, c := range cmds {
		if e := rr.runSimple(c.words); e != nil {
			if ignoreErr {
				return nil
			}
			return e
		}
	}
	return nil
}

func (rr *recipeRunner) runShell(line string, ignoreErr bool) error {
	if runtime.GOOS == "windows" {
		return fmt.Errorf("cannot run %q without a shell", line)
	}
	line = strings.TrimLeft(line, " \t")
	for len(line) > 0 && strings.IndexByte("@-+", line[0]) >= 0 {
		line = strings.TrimLeft(line[1:], " \t")
	}
	cmd := exec.Command("/bin/sh", "-c", strings.ReplaceAll(line, "$$", "$"))
	cmd.Stdout, cmd.Stderr = rr.stdout, rr.stderr
	if err := cmd.Run(); err != nil && !ignoreErr {
		return err
	}
	return nil
}

func (rr *recipeRunner) runSimple(words []string) error {
	if len(words) == 0 {
		return nil
	}
	switch words[0] {
	case "touch":
		return builtinTouch(words[1:])
	case "mv":
		return builtinMv(words[1:])
	case "rm":
		return builtinRm(words[1:])
	}
	prog, args := resolveProgram(words)
	cmd := exec.Command(prog, args...)
	cmd.Stdout, cmd.Stderr = rr.stdout, rr.stderr
	return cmd.Run()
}

// touch [-r REF] FILE...: create the file if needed; with -r it gets the modification time of REF.
func builtinTouch(args []string) error {
	var ref string
	var files []string
	for i := 0; i < len(args); i++ {
		switch {
		case args[i] == "-r" && i+1 < len(args):
			ref = args[i+1]
			i++
		case strings.HasPrefix(args[i], "-"):
		default:
			files = append(files, args[i])
		}
	}
	for _, f := range files {
		if _, err := os.Stat(f); err != nil {
			fh, err := os.OpenFile(f, os.O_CREATE|os.O_WRONLY, 0o644)
			if err != nil {
				return err
			}
			fh.Close()
		}
		if ref != "" {
			st, err := os.Stat(ref)
			if err != nil {
				return err
			}
			if err := os.Chtimes(f, st.ModTime(), st.ModTime()); err != nil {
				return err
			}
		}
	}
	return nil
}

func builtinMv(args []string) error {
	var p []string
	for _, a := range args {
		if !strings.HasPrefix(a, "-") {
			p = append(p, a)
		}
	}
	if len(p) != 2 {
		return errors.New("mv: needs a source and a destination")
	}
	return os.Rename(p[0], p[1])
}

func builtinRm(args []string) error {
	for _, a := range args {
		if !strings.HasPrefix(a, "-") {
			if err := os.Remove(a); err != nil && !os.IsNotExist(err) {
				return err
			}
		}
	}
	return nil
}

// ---------------------------------------------------------------------------------------------------- scheduling

type targetState struct {
	once sync.Once
	err  error
}

type maker struct {
	mf       *makefile
	rr       *recipeRunner
	tokens   chan struct{}
	mu       sync.Mutex
	states   map[string]*targetState
	failed   bool
	failTgt  string
	failErr  error
	onFinish func(done, total int)
	total    int
	done     int
}

var errSkipped = errors.New("skipped")

func (m *maker) state(t string) *targetState {
	m.mu.Lock()
	defer m.mu.Unlock()
	s := m.states[t]
	if s == nil {
		s = &targetState{}
		m.states[t] = s
	}
	return s
}

func (m *maker) build(t string) error {
	s := m.state(t)
	s.once.Do(func() { s.err = m.buildOnce(t) })
	return s.err
}

func (m *maker) buildOnce(t string) error {
	rule := m.mf.rules[t]
	if rule == nil {
		if _, err := os.Stat(t); err == nil {
			return nil
		}
		return fmt.Errorf("No rule to make target '%s'.", t)
	}
	var wg sync.WaitGroup
	errs := make([]error, len(rule.deps))
	for i, d := range rule.deps {
		wg.Add(1)
		go func(i int, d string) {
			defer wg.Done()
			errs[i] = m.build(d)
		}(i, d)
	}
	wg.Wait()
	for _, e := range errs {
		if e != nil {
			return e
		}
	}
	if len(rule.recipe) == 0 {
		return nil
	}
	m.tokens <- struct{}{}
	defer func() { <-m.tokens }()
	m.mu.Lock()
	stop := m.failed
	m.mu.Unlock()
	if stop {
		return errSkipped
	}
	for _, line := range rule.recipe {
		if err := m.rr.run(line); err != nil {
			m.mu.Lock()
			if !m.failed {
				m.failed, m.failTgt, m.failErr = true, t, err
			}
			m.mu.Unlock()
			return err
		}
	}
	m.mu.Lock()
	m.done++
	d, tot := m.done, m.total
	m.mu.Unlock()
	if m.onFinish != nil {
		m.onFinish(d, tot)
	}
	return nil
}

// countRecipes counts the recipes that will run for goals (for the progress file).
func (mf *makefile) countRecipes(goals []string) int {
	seen := map[string]bool{}
	n := 0
	var walk func(t string)
	walk = func(t string) {
		if seen[t] {
			return
		}
		seen[t] = true
		r := mf.rules[t]
		if r == nil {
			return
		}
		for _, d := range r.deps {
			walk(d)
		}
		if len(r.recipe) > 0 {
			n++
		}
	}
	for _, g := range goals {
		walk(g)
	}
	return n
}

// hasCycle reports whether the prerequisites reachable from goals loop back on themselves.
func (mf *makefile) hasCycle(goals []string) bool {
	state := map[string]int{}
	var visit func(t string) bool
	visit = func(t string) bool {
		switch state[t] {
		case 1:
			return true
		case 2:
			return false
		}
		state[t] = 1
		if r := mf.rules[t]; r != nil {
			for _, d := range r.deps {
				if visit(d) {
					return true
				}
			}
		}
		state[t] = 2
		return false
	}
	for _, g := range goals {
		if visit(g) {
			return true
		}
	}
	return false
}

// runMake is the whole make program: args are the command line without the program name. It returns the exit status.
func runMake(args []string, stdout, stderr io.Writer, getenv func(string) string) int {
	file, dir := "", ""
	jobs := 0
	var goals []string
	cmdVars := map[string]string{}
	for i := 0; i < len(args); i++ {
		a := args[i]
		next := func() string {
			if i+1 < len(args) {
				i++
				return args[i]
			}
			return ""
		}
		switch {
		case a == "--version" || a == "-v":
			fmt.Fprintln(stdout, "GNU Make 4.4.1")
			fmt.Fprintln(stdout, "infected-build's built-in make: runs the makefiles of GCC's LTO link, nothing more.")
			return 0
		case a == "-f" || a == "--file" || a == "--makefile":
			file = next()
		case strings.HasPrefix(a, "--file="):
			file = strings.TrimPrefix(a, "--file=")
		case strings.HasPrefix(a, "-f") && len(a) > 2:
			file = a[2:]
		case a == "-C" || a == "--directory":
			dir = next()
		case strings.HasPrefix(a, "--directory="):
			dir = strings.TrimPrefix(a, "--directory=")
		case a == "-j" || a == "--jobs":
			if i+1 < len(args) {
				if n, err := strconv.Atoi(args[i+1]); err == nil {
					jobs = n
					i++
					continue
				}
			}
			jobs = runtime.NumCPU()
		case strings.HasPrefix(a, "--jobs="):
			jobs, _ = strconv.Atoi(strings.TrimPrefix(a, "--jobs="))
		case strings.HasPrefix(a, "-j") && len(a) > 2:
			jobs, _ = strconv.Atoi(a[2:])
		case strings.HasPrefix(a, "-"):
			// -s, -w, -k, -B, --no-print-directory, --jobserver-auth=...: nothing to do
		case strings.Contains(a, "="):
			kv := strings.SplitN(a, "=", 2)
			cmdVars[kv[0]] = kv[1]
		default:
			goals = append(goals, a)
		}
	}
	if jobs < 1 {
		jobs = 1
	}
	if dir != "" {
		if err := os.Chdir(dir); err != nil {
			fmt.Fprintf(stderr, "make: *** %v.  Stop.\n", err)
			return 2
		}
	}
	if file == "" {
		for _, n := range []string{"GNUmakefile", "makefile", "Makefile"} {
			if _, err := os.Stat(n); err == nil {
				file = n
				break
			}
		}
	}
	if file == "" {
		fmt.Fprintln(stderr, "make: *** No targets specified and no makefile found.  Stop.")
		return 2
	}
	fh, err := os.Open(file)
	if err != nil {
		fmt.Fprintf(stderr, "make: %s: No such file or directory\nmake: *** No rule to make target '%s'.  Stop.\n", file, file)
		return 2
	}
	mf, err := parseMakefile(fh, cmdVars)
	fh.Close()
	if err != nil {
		fmt.Fprintf(stderr, "make: %s: %v.  Stop.\n", file, err)
		return 2
	}
	if len(goals) == 0 {
		if mf.first == "" {
			fmt.Fprintln(stderr, "make: *** No targets.  Stop.")
			return 2
		}
		goals = []string{mf.first}
	}
	if mf.hasCycle(goals) {
		fmt.Fprintln(stderr, "make: *** Circular dependency dropped.  Stop.")
		return 2
	}
	m := &maker{
		mf:     mf,
		rr:     &recipeRunner{stdout: &lockedWriter{w: stdout}, stderr: &lockedWriter{w: stderr}, winBackslash: runtime.GOOS == "windows"},
		tokens: make(chan struct{}, jobs),
		states: map[string]*targetState{},
		total:  mf.countRecipes(goals),
	}
	if pf := getenv(progressFileEnv); pf != "" {
		// Recipes finish on several goroutines: write in order, never a smaller count after a larger one.
		var pmu sync.Mutex
		best := 0
		m.onFinish = func(done, total int) {
			pmu.Lock()
			defer pmu.Unlock()
			if done > best {
				best = done
				writeProgressFile(pf, done, total)
			}
		}
		writeProgressFile(pf, 0, m.total)
	}
	var wg sync.WaitGroup
	errs := make([]error, len(goals))
	for i, g := range goals {
		wg.Add(1)
		go func(i int, g string) {
			defer wg.Done()
			errs[i] = m.build(g)
		}(i, g)
	}
	wg.Wait()
	m.mu.Lock()
	failed, tgt, ferr := m.failed, m.failTgt, m.failErr
	m.mu.Unlock()
	if failed {
		code := 1
		var ee *exec.ExitError
		if errors.As(ferr, &ee) {
			code = ee.ExitCode()
		}
		fmt.Fprintf(stderr, "make: *** [%s] Error %d\n", tgt, code)
		return 2
	}
	for _, e := range errs {
		if e != nil {
			fmt.Fprintf(stderr, "make: *** %v  Stop.\n", e)
			return 2
		}
	}
	return 0
}

// writeProgressFile tells the parent builder how many partitions are done: the file holds "done total".
func writeProgressFile(path string, done, total int) {
	tmp := path + ".new"
	if err := os.WriteFile(tmp, []byte(fmt.Sprintf("%d %d\n", done, total)), 0o644); err == nil {
		os.Rename(tmp, path)
	}
}

// lockedWriter lets the recipes that run in parallel share one output.
type lockedWriter struct {
	mu sync.Mutex
	w  io.Writer
}

func (l *lockedWriter) Write(p []byte) (int, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.w.Write(p)
}
