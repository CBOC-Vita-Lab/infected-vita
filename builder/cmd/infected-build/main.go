// Command infected-build turns a player's Infected (PSP) disc image into infected.vpk for the PS Vita, using the builder kit
// that sits next to it (see builder/README.md).
//
//	infected-build [--out DIR] [--jobs N] [--work DIR] [--progress] [--keep] Infected.iso
package main

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"
)

const version = "1.1"

const usageText = `Usage: infected-build [options] Infected.iso

Builds infected.vpk, the Infected package for the PS Vita, from your own Infected disc image
(the US version, ULUS-10054). The builder kit must be in the same folder as this program.

Options:
  --out DIR     where to put infected.vpk (default: the folder of the .iso)
  --jobs N      how many compiler processes to run at once (default: as many as the CPUs and free memory allow)
  --work DIR    folder for the temporary files (default: a new folder in the temporary folder; about 700 MB are needed)
  --progress    print machine-readable progress lines (@step, @progress, @done) instead of text for people
  --keep        keep the temporary files (build log, objects, eboot.bin) when finished
  --gen DIR     skip the disc: build from C++ that was already generated (lean_*.cpp, lean_table.cpp, lean_decls.h in DIR)
  --kit DIR     use the builder kit in DIR (default: the folder of this program)
  --verify      check every kit file against its checksum first
  --version     print the version
`

// cliOptions are Options plus the flags that are not about the build.
type cliOptions struct {
	Options
	Progress bool
	Help     bool
	Version  bool
}

// parseArgs reads the command line (without the program name). Options and the disc image may come in any order.
func parseArgs(args []string) (cliOptions, error) {
	var o cliOptions
	var files []string
	value := func(i *int, name, inline string, hasInline bool) (string, error) {
		if hasInline {
			return inline, nil
		}
		if *i+1 >= len(args) {
			return "", userErrorf("The option %s needs a value.", name)
		}
		*i++
		return args[*i], nil
	}
	for i := 0; i < len(args); i++ {
		a := args[i]
		if a == "--" {
			files = append(files, args[i+1:]...)
			break
		}
		if !strings.HasPrefix(a, "-") || a == "-" {
			files = append(files, a)
			continue
		}
		name, inline, hasInline := strings.TrimLeft(a, "-"), "", false
		if k := strings.IndexByte(name, '='); k >= 0 {
			name, inline, hasInline = name[:k], name[k+1:], true
		}
		switch name {
		case "out", "o":
			v, err := value(&i, a, inline, hasInline)
			if err != nil {
				return o, err
			}
			o.OutDir = v
		case "work":
			v, err := value(&i, a, inline, hasInline)
			if err != nil {
				return o, err
			}
			o.WorkDir = v
		case "kit":
			v, err := value(&i, a, inline, hasInline)
			if err != nil {
				return o, err
			}
			o.KitDir = v
		case "gen":
			v, err := value(&i, a, inline, hasInline)
			if err != nil {
				return o, err
			}
			o.GenDir = v
		case "jobs", "j":
			v, err := value(&i, a, inline, hasInline)
			if err != nil {
				return o, err
			}
			n, err := strconv.Atoi(v)
			if err != nil || n < 1 || n > 256 {
				return o, userErrorf("The option --jobs needs a number from 1 up, not %q.", v)
			}
			o.Jobs = n
		case "progress":
			o.Progress = true
		case "keep":
			o.Keep = true
		case "verify":
			o.Verify = true
		case "help", "h", "?":
			o.Help = true
		case "version":
			o.Version = true
		default:
			return o, userErrorf("Unknown option %s.", a)
		}
	}
	if len(files) > 1 {
		return o, userErrorf("Give one disc image, not %d.", len(files))
	}
	if len(files) == 1 {
		o.ISO = files[0]
	}
	return o, nil
}

func main() { os.Exit(realMain(os.Args, os.Stdin, os.Stdout, os.Stderr)) }

func realMain(argv []string, stdin io.Reader, stdout, stderr io.Writer) int {
	if os.Getenv(makeModeEnv) == "1" {
		// started by GCC's lto-wrapper as `make` (runner.go childEnv, make.go)
		return runMake(argv[1:], stdout, stderr, os.Getenv)
	}
	args := argv[1:]
	interactive := false
	if len(args) == 0 {
		// started by a double-click: ask for the disc image
		if f, ok := stdin.(*os.File); !ok || !isTerminal(f) {
			fmt.Fprint(stderr, usageText)
			return 2
		}
		interactive = true
		iso, err := askForDisc(stdin, stdout)
		if err != nil {
			fmt.Fprintln(stderr, err)
			return pause(stdin, stdout, interactive, 1)
		}
		args = []string{iso}
	}
	o, err := parseArgs(args)
	if err != nil {
		fmt.Fprintln(stderr, strings.TrimSuffix(err.Error(), " Try --help.")+" Try --help.")
		return pause(stdin, stdout, interactive, 2)
	}
	switch {
	case o.Help:
		fmt.Fprint(stdout, usageText)
		return 0
	case o.Version:
		fmt.Fprintln(stdout, "infected-build", version)
		return 0
	case o.ISO == "" && o.GenDir == "":
		fmt.Fprint(stderr, usageText)
		return 2
	}
	self, err := os.Executable()
	if err == nil {
		if r, rerr := filepath.EvalSymlinks(self); rerr == nil {
			self = r
		}
	} else {
		self = argv[0]
	}
	code := runBuild(o, self, stdout, stderr)
	return pause(stdin, stdout, interactive, code)
}

// pause keeps a window that was opened by a double-click on screen until Enter is pressed.
func pause(stdin io.Reader, stdout io.Writer, interactive bool, code int) int {
	if interactive {
		fmt.Fprint(stdout, "\nPress Enter to close.")
		bufio.NewReader(stdin).ReadString('\n')
	}
	return code
}

// askForDisc prompts for the disc image and returns the path typed or dropped onto the window.
func askForDisc(stdin io.Reader, stdout io.Writer) (string, error) {
	fmt.Fprint(stdout, "Drag your Infected .iso onto this window and press Enter: ")
	line, err := bufio.NewReader(stdin).ReadString('\n')
	if err != nil && line == "" {
		return "", errors.New("No disc image given.")
	}
	p := cleanDroppedPath(line)
	if p == "" {
		return "", errors.New("No disc image given.")
	}
	return p, nil
}

// cleanDroppedPath removes the spaces and quotes a terminal puts around a dragged file name ("C:\My Games\Infected.iso", 'x y.iso',
// x\ y.iso).
func cleanDroppedPath(s string) string {
	s = strings.TrimSpace(s)
	if len(s) >= 2 && (s[0] == '"' && s[len(s)-1] == '"' || s[0] == '\'' && s[len(s)-1] == '\'') {
		s = s[1 : len(s)-1]
	}
	if filepath.Separator == '/' {
		s = strings.ReplaceAll(s, `\ `, " ")
	}
	return strings.TrimSpace(s)
}

// runBuild runs the build with signal handling and prints the outcome. It returns the exit status.
func runBuild(o cliOptions, self string, stdout, stderr io.Writer) int {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	sig := make(chan os.Signal, 2)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM)
	defer signal.Stop(sig)

	tty := false
	if f, ok := stdout.(*os.File); ok {
		tty = isTerminal(f)
	}
	steps := stepCount
	if o.GenDir != "" {
		steps-- // no disc to read, no translation: one step takes their place
	}
	rep := NewReporter(stdout, o.Progress, tty, steps)
	b := &Builder{Opt: o.Options, Rep: rep, Self: self}
	if !o.Progress {
		fmt.Fprintf(stdout, "Infected builder %s\n", version)
	}
	type result struct {
		vpk string
		err error
	}
	done := make(chan result, 1)
	go func() {
		vpk, err := b.Run(ctx)
		done <- result{vpk, err}
	}()
	var res result
	select {
	case res = <-done:
	case <-sig:
		cancel()
		select {
		case res = <-done:
		case <-time.After(10 * time.Second):
		}
		rep.Abort()
		fmt.Fprintln(stderr, "Cancelled.")
		return 130
	}
	if res.err != nil {
		rep.Abort()
		if errors.Is(res.err, context.Canceled) {
			fmt.Fprintln(stderr, "Cancelled.")
			return 130
		}
		msg := strings.ReplaceAll(res.err.Error(), " Details: {log}", "")
		fmt.Fprintln(stderr, msg)
		return 1
	}
	rep.Done(res.vpk)
	return 0
}
