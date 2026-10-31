package main

import (
	"archive/zip"
	"bytes"
	"compress/flate"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"runtime/debug"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/CBOC-Vita-Lab/infected-vita/builder/leangen"
	"github.com/CBOC-Vita-Lab/infected-vita/builder/psp"
)

const (
	discID      = "ULUS-10054"
	bootPath    = "PSP_GAME/SYSDIR/BOOT.BIN"
	paramPath   = "PSP_GAME/PARAM.SFO"
	stepCount   = 5
	wrongDisc   = "This is not the US version of Infected (ULUS-10054, 1.02)."
	logFileName = "build.log"
)

// Options is what the command line asks for.
type Options struct {
	ISO     string
	OutDir  string // default: the folder of the ISO
	WorkDir string // default: a new folder in the temporary folder
	KitDir  string // default: the folder of this program
	GenDir  string // build from this folder of already generated C++ (lean_*.cpp, lean_table.cpp, lean_decls.h) instead of from a disc
	Jobs    int    // 0: automatic
	Keep    bool
	Verify  bool // check the SHA-256 of every kit file
}

// Builder runs the whole pipeline.
type Builder struct {
	Opt  Options
	Rep  *Reporter
	Self string // path of this executable (becomes $MAKE for the tools)

	kit      *Kit
	work     string
	ownWork  bool // the work folder was made by us (not given with --work)
	log      *buildLog
	logPath  string
	runner   *Runner
	jobs     int
	progFile string
	tmp      string   // TMPDIR of the tools: <work>/tmp, or a folder elsewhere when the work folder's name has a space
	extraTmp string   // a folder made for tmp outside the work folder (removed at the end)
	makeProg string   // value of $MAKE (makeProgram)
	created  []string // names inside the work folder to remove at the end (with a given --work)
}

// Run builds the package and returns the path of the .vpk. On failure the work folder keeps only build.log (and everything with --keep).
func (b *Builder) Run(ctx context.Context) (vpk string, err error) {
	defer func() {
		b.cleanup(err == nil || errors.Is(err, context.Canceled)) // a cancelled build leaves nothing behind, not even the log
		if err != nil && !errors.Is(err, context.Canceled) && b.logPath != "" {
			if _, serr := os.Stat(b.logPath); serr == nil {
				err = withLogHint(err, b.logPath)
			}
		}
	}()
	if err = b.prepare(); err != nil {
		return "", err
	}
	t0 := time.Now()
	if b.Opt.GenDir != "" {
		b.Rep.Step("Preparing the generated code")
		if err = b.stageGenerated(ctx); err != nil {
			return "", err
		}
	} else {
		b.Rep.Step("Reading your disc")
		boot, image, err := b.readDisc()
		if err != nil {
			return "", err
		}
		if err = ctx.Err(); err != nil {
			return "", err
		}

		b.Rep.Step("Translating the game")
		if err = b.translate(ctx, boot, image); err != nil {
			return "", err
		}
		boot, image = nil, nil
		debug.FreeOSMemory()
	}

	b.Rep.Step("Compiling (this is the long part)")
	if err = b.compile(ctx); err != nil {
		return "", err
	}
	if err = b.archive(ctx); err != nil {
		return "", err
	}

	b.Rep.Step("Linking")
	elf, err := b.link(ctx)
	if err != nil {
		return "", err
	}

	b.Rep.Step("Packaging")
	vpk, err = b.packageVPK(ctx, elf)
	if err != nil {
		return "", err
	}
	b.log.Printf("== finished in %s", time.Since(t0).Round(time.Second))
	return vpk, nil
}

// withLogHint adds the log location to a tool failure message.
func withLogHint(err error, logPath string) error {
	var ue *userError
	if errors.As(err, &ue) && strings.Contains(ue.msg, "{log}") {
		return &userError{strings.ReplaceAll(ue.msg, "{log}", logPath)}
	}
	return err
}

// ---------------------------------------------------------------------------------------------------- preparation

func (b *Builder) prepare() error {
	if runtime.GOOS == "darwin" {
		if err := checkMacOS(macOSMajor(), 14); err != nil { // before anything else; the kit may ask for more below
			return err
		}
	}
	kitDir := b.Opt.KitDir
	if kitDir == "" {
		kitDir = filepath.Dir(b.Self)
	}
	kit, err := LoadKit(kitDir)
	if err != nil {
		return err
	}
	b.kit = kit
	if runtime.GOOS == "darwin" {
		if err := checkMacOS(macOSMajor(), kit.requiredMacOS()); err != nil {
			return err
		}
	}
	if err := kit.Verify(b.Opt.Verify); err != nil {
		return err
	}
	if b.Opt.GenDir != "" {
		gen, err := filepath.Abs(b.Opt.GenDir)
		if err != nil {
			return err
		}
		b.Opt.GenDir = gen
		for _, n := range []string{"lean_table.cpp", "lean_decls.h"} {
			if _, err := os.Stat(filepath.Join(gen, n)); err != nil {
				return userErrorf("The folder %s has no generated code (%s is missing).", gen, n)
			}
		}
		if b.Opt.OutDir == "" {
			b.Opt.OutDir = "."
		}
	} else {
		if b.Opt.ISO == "" {
			return userErrorf("No disc image given. Usage: infected-build [--out DIR] Infected.iso")
		}
		if st, err := os.Stat(b.Opt.ISO); err != nil || st.IsDir() {
			return userErrorf("I cannot find the disc image %s.", b.Opt.ISO)
		}
		iso, err := filepath.Abs(b.Opt.ISO)
		if err != nil {
			return err
		}
		b.Opt.ISO = iso
		if b.Opt.OutDir == "" {
			b.Opt.OutDir = filepath.Dir(iso)
		}
	}
	if b.Opt.OutDir, err = filepath.Abs(b.Opt.OutDir); err != nil {
		return err
	}
	if err := os.MkdirAll(b.Opt.OutDir, 0o755); err != nil {
		return userErrorf("I cannot create the output folder %s: %v.", b.Opt.OutDir, err)
	}
	if probe, err := os.CreateTemp(b.Opt.OutDir, ".infected-build-*"); err != nil {
		return userErrorf("I cannot write to the output folder %s. Choose another with --out.", b.Opt.OutDir)
	} else {
		probe.Close()
		os.Remove(probe.Name())
	}

	// work folder: short, native, and in a place with room
	if b.Opt.WorkDir != "" {
		if b.work, err = filepath.Abs(b.Opt.WorkDir); err != nil {
			return err
		}
		if _, serr := os.Stat(b.work); os.IsNotExist(serr) {
			b.ownWork = true
		}
		if err := os.MkdirAll(b.work, 0o755); err != nil {
			return userErrorf("I cannot create the work folder %s: %v.", b.work, err)
		}
	} else {
		base := spacelessDir(tempCandidates(os.Getenv), shortPath, dirUsable)
		w, err := os.MkdirTemp(base, "ib") // base "" is the temporary folder of the system
		if err != nil {
			return userErrorf("I cannot create a work folder in the temporary folder: %v.", err)
		}
		b.work, b.ownWork = w, true
	}
	if free, err := freeDiskBytes(b.work); err == nil && kit.MinDiskBytes > 0 && free < kit.MinDiskBytes {
		b.cleanup(false)
		return userErrorf("Not enough free disk space: %s needed, %s free on the drive that holds %s.", gb(kit.MinDiskBytes), gb(free), b.work)
	}
	for _, d := range []string{"inc", "gen", "obj", "tmp"} {
		if err := os.MkdirAll(filepath.Join(b.work, d), 0o755); err != nil {
			return userErrorf("I cannot create files in the work folder %s: %v.", b.work, err)
		}
		b.created = append(b.created, d)
	}
	b.logPath = filepath.Join(b.work, logFileName)
	b.created = append(b.created, logFileName)
	if b.log, err = openBuildLog(b.logPath); err != nil {
		return userErrorf("I cannot create files in the work folder %s: %v.", b.work, err)
	}
	b.jobs = b.Opt.Jobs
	if b.jobs <= 0 {
		b.jobs = defaultJobs(runtime.NumCPU(), freeRAMBytes())
	}
	b.tmp = filepath.Join(b.work, "tmp")
	if hasSpace(b.tmp) {
		if base := spacelessDir(tempCandidates(os.Getenv), shortPath, dirUsable); base != "" {
			if d, err := os.MkdirTemp(base, "ibt"); err == nil {
				b.tmp, b.extraTmp = d, d
			}
		}
		if hasSpace(b.tmp) {
			b.log.Printf("warning: the temporary folder %s has a space in its name; the link will not run in parallel", b.tmp)
		}
	}
	b.progFile = filepath.Join(b.tmp, "partitions.txt")
	b.makeProg = makeProgram(b.Self, kit.Dir, b.tmp, func(dst string) error {
		data, err := os.ReadFile(b.Self)
		if err != nil {
			return err
		}
		return os.WriteFile(dst, data, 0o755)
	}, shortPath)
	if b.makeProg == "" {
		b.log.Printf("warning: no way found to let the linker use this program as make (its path has a space); the link will not run in parallel")
	}
	lowerPriority()
	b.log.Printf("infected-build for %s; kit %s (%s/%s, %s); work %s; %d jobs; host %s", b.Opt.ISO, kit.Dir, kit.OS, kit.Arch, kit.Game.Version, b.work, b.jobs, thisHost())
	return nil
}

// checkMacOS fails on a macOS older than the kit's compilers can run on. major 0 means unknown: no objection.
func checkMacOS(major, need int) error {
	if major > 0 && major < need {
		return userErrorf("Building Infected needs macOS %d or newer on this Mac (this one has macOS %d). Update macOS, or run the builder on a newer Mac or a Windows or Linux computer and copy the .vpk to your Vita.", need, major)
	}
	return nil
}

func gb(n int64) string { return fmt.Sprintf("%.1f GB", float64(n)/1e9) }

// cleanup removes the work files (unless --keep). After a failure build.log stays, so that the error message can point at it.
func (b *Builder) cleanup(ok bool) {
	if b.work == "" {
		return
	}
	if b.log != nil {
		b.log.Close()
		b.log = nil
	}
	if b.Opt.Keep {
		return
	}
	keepLog := !ok
	for _, name := range b.createdNames() {
		if keepLog && name == logFileName {
			continue
		}
		os.RemoveAll(filepath.Join(b.work, name))
	}
	if b.extraTmp != "" {
		os.RemoveAll(b.extraTmp)
	}
	if b.ownWork {
		os.Remove(b.work) // only succeeds when empty: a kept build.log keeps the folder
	}
}

func (b *Builder) createdNames() []string {
	names := append([]string{}, b.created...)
	names = append(names, "liblean_vita_lto.a", "infected.elf", "infected.velf", "eboot.bin", "text_pad.o", "infected.elf.tmp")
	return names
}

// ---------------------------------------------------------------------------------------------------- 1. the disc

func (b *Builder) readDisc() (boot, image []byte, err error) {
	f, err := os.Open(b.Opt.ISO)
	if err != nil {
		return nil, nil, userErrorf("I cannot open %s: %v.", b.Opt.ISO, err)
	}
	defer f.Close()
	var magic [4]byte
	f.ReadAt(magic[:], 0)
	if string(magic[:]) == "CISO" || string(magic[:]) == "ZISO" {
		return nil, nil, userErrorf("This disc image is compressed (.cso). Please make an uncompressed .iso of your disc and use that.")
	}
	boot, err = psp.ReadFileFrom(f, bootPath)
	if err != nil {
		switch {
		case errors.Is(err, psp.ErrNotISO):
			return nil, nil, userErrorf("%s does not look like a PSP disc image. Use the .iso made from your Infected UMD.", filepath.Base(b.Opt.ISO))
		case errors.Is(err, psp.ErrNotOnDisc):
			return nil, nil, userErrorf("%s is a disc image but not of a PSP game (it has no %s). Use the .iso made from your Infected UMD.", filepath.Base(b.Opt.ISO), bootPath)
		}
		return nil, nil, userErrorf("I cannot read the disc image %s: %v.", filepath.Base(b.Opt.ISO), err)
	}
	sum := sha256.Sum256(boot)
	if hex.EncodeToString(sum[:]) != b.kit.Game.BootSha256 {
		msg := wrongDisc
		if sfo, err := psp.ReadFileFrom(f, paramPath); err == nil {
			if kv := parseSFO(sfo); kv["DISC_ID"] != "" {
				msg = fmt.Sprintf("%s Your disc is %s.", wrongDisc, kv["DISC_ID"])
				if kv["DISC_ID"] == discID {
					msg = "This is the US version of Infected (ULUS-10054) but not the 1.02 release this builder needs. Use the original UMD dump (BOOT.BIN is different)."
				}
			}
		}
		return nil, nil, &userError{msg}
	}
	image, err = psp.BuildImage(boot)
	if err != nil {
		return nil, nil, userErrorf("I cannot read the program on your disc: %v.", err)
	}
	b.log.Printf("== disc ok: BOOT.BIN %s", b.kit.Game.BootSha256)
	return boot, image, nil
}

// ---------------------------------------------------------------------------------------------------- 2. translate

// stageGenerated is the --gen start: the generated C++ comes from a folder instead of from the disc.
func (b *Builder) stageGenerated(ctx context.Context) error {
	if err := copyTree(b.kit.Path("game/include"), filepath.Join(b.work, "inc")); err != nil {
		return userErrorf("I cannot copy the kit's headers: %v.", err)
	}
	ents, err := os.ReadDir(b.Opt.GenDir)
	if err != nil {
		return userErrorf("I cannot read %s: %v.", b.Opt.GenDir, err)
	}
	n := 0
	for _, e := range ents {
		name := e.Name()
		if e.IsDir() || !strings.HasPrefix(name, "lean_") || !(strings.HasSuffix(name, ".cpp") || strings.HasSuffix(name, ".h")) {
			continue
		}
		data, err := os.ReadFile(filepath.Join(b.Opt.GenDir, name))
		if err != nil {
			return userErrorf("I cannot read %s: %v.", name, err)
		}
		if err := os.WriteFile(filepath.Join(b.work, "gen", name), data, 0o644); err != nil {
			return userErrorf("I cannot write into the work folder: %v.", err)
		}
		n++
	}
	b.log.Printf("== generated code from %s: %d files", b.Opt.GenDir, n)
	return ctx.Err()
}

func (b *Builder) translate(ctx context.Context, boot, image []byte) error {
	if err := copyTree(b.kit.Path("game/include"), filepath.Join(b.work, "inc")); err != nil {
		return userErrorf("I cannot copy the kit's headers: %v.", err)
	}
	profile, err := os.ReadFile(b.kit.Path(b.kit.Profile))
	if err != nil {
		return userErrorf("The builder kit is incomplete: %s is missing.", b.kit.Profile)
	}
	err = leangen.Generate(image, boot, profile, filepath.Join(b.work, "gen"), b.Rep.Progress)
	if err != nil {
		b.log.Printf("translator: %v", err)
		return userErrorf("The game could not be translated (%v). Details: {log}", err)
	}
	b.Rep.Progress(1, 1)
	return ctx.Err()
}

func copyTree(src, dst string) error {
	return filepath.Walk(src, func(p string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		rel, _ := filepath.Rel(src, p)
		target := filepath.Join(dst, rel)
		if info.IsDir() {
			return os.MkdirAll(target, 0o755)
		}
		data, err := os.ReadFile(p)
		if err != nil {
			return err
		}
		return os.WriteFile(target, data, 0o644)
	})
}

// ---------------------------------------------------------------------------------------------------- 3. compile, archive

// vars are the placeholder values every step can use.
func (b *Builder) vars() Vars {
	return Vars{
		"kit":  filepath.ToSlash(b.kit.Dir),
		"work": filepath.ToSlash(b.work),
		"jobs": strconv.Itoa(b.jobs),
	}
}

func (b *Builder) env() []string {
	e := childEnv(b.kit, b.tmp, b.makeProg, b.progFile, os.Getenv)
	if d := os.Getenv("IB_DIAG_ENV"); d != "" { // TEMPORARY experiment hook
		e = append(e, strings.Split(d, ";;")...)
	}
	return e
}

func (b *Builder) newRunner(ctx context.Context) *Runner {
	return &Runner{Ctx: ctx, Log: b.log, Env: b.env(), LogPath: b.logPath}
}

// sortedMatches lists the files of dir that match any of the patterns, pattern by pattern, each pattern's matches in name order.
func sortedMatches(dir string, patterns []string) []string {
	var out []string
	seen := map[string]bool{}
	for _, pat := range patterns {
		m, _ := filepath.Glob(filepath.Join(dir, pat))
		sort.Strings(m)
		for _, f := range m {
			if !seen[f] {
				seen[f] = true
				out = append(out, f)
			}
		}
	}
	return out
}

func (b *Builder) compile(ctx context.Context) error {
	r := b.newRunner(ctx)
	b.runner = r
	cxx, err := b.kit.toolOf(b.kit.Compile.Tool)
	if err != nil {
		return err
	}
	srcs := sortedMatches(filepath.Join(b.work, "gen"), b.kit.Compile.Sources)
	if len(srcs) == 0 {
		return userErrorf("The translator produced no code (internal error). Details: {log}")
	}
	cwd := b.work
	ctx2, cancel := context.WithCancel(ctx)
	defer cancel()
	r.Ctx = ctx2
	var done int32
	var firstErr error
	var errOnce sync.Once
	work := make(chan string)
	var wg sync.WaitGroup
	b.Rep.Progress(0, len(srcs))
	for w := 0; w < b.jobs; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for src := range work {
				if ctx2.Err() != nil {
					continue
				}
				stem := strings.TrimSuffix(filepath.Base(src), ".cpp")
				v := b.vars()
				v["stem"] = stem
				args, err := v.ExpandArgs(b.kit.Compile.Args, nil)
				if err == nil {
					err = r.Run("compile "+stem, cwd, cxx, args)
				}
				if err != nil {
					errOnce.Do(func() {
						firstErr = compileFailure(stem, err)
						cancel()
					})
					continue
				}
				b.Rep.Progress(int(atomic.AddInt32(&done, 1)), len(srcs))
			}
		}()
	}
	for _, s := range srcs {
		work <- s
	}
	close(work)
	wg.Wait()
	if err := ctx.Err(); err != nil {
		return err
	}
	return firstErr
}

func compileFailure(stem string, err error) error {
	var ue *userError
	if errors.As(err, &ue) {
		return err
	}
	if strings.Contains(err.Error(), "killed") {
		return userErrorf("The compiler was stopped while building %s, probably because the computer ran out of memory. Close other programs or run again with --jobs 1. Details: {log}", stem)
	}
	return userErrorf("The compiler stopped with an error while building %s. Details: {log}", stem)
}

func (b *Builder) archive(ctx context.Context) error {
	ar, err := b.kit.toolOf(b.kit.Archive.Tool)
	if err != nil {
		return err
	}
	lib := filepath.Join(b.work, "liblean_vita_lto.a")
	if b.kit.Archive.DeleteFirst {
		os.Remove(lib)
	}
	objs := sortedMatches(filepath.Join(b.work, "obj"), b.kit.Archive.Order)
	if len(objs) == 0 {
		return userErrorf("The compiler produced no code (internal error). Details: {log}")
	}
	v := b.vars()
	v["lib"] = lib
	args, err := v.ExpandArgs(b.kit.Archive.Args, map[string][]string{"objs": objs})
	if err != nil {
		return err
	}
	if err := b.newRunner(ctx).Run("archive", b.kit.Dir, ar, args); err != nil {
		if ctx.Err() != nil {
			return ctx.Err()
		}
		return userErrorf("Collecting the compiled code failed. Details: {log}")
	}
	return nil
}

// ---------------------------------------------------------------------------------------------------- 4. link

// runLink links with the given text pad object (empty: the kit's own).
func (b *Builder) runLink(ctx context.Context, padObject, elf string) error {
	cxx, err := b.kit.toolOf(b.kit.Link.Tool)
	if err != nil {
		return err
	}
	raw, err := b.kit.LinkArgs()
	if err != nil {
		return err
	}
	v := b.vars()
	v["lean"] = filepath.Join(b.work, "liblean_vita_lto.a")
	v["lib"] = v["lean"]
	v["out"] = elf
	args := make([]string, 0, len(raw))
	for _, a := range raw {
		e, err := v.Expand(a)
		if err != nil {
			return err
		}
		if padObject != "" && e == b.kit.TextPad.Object {
			e = padObject
		}
		args = append(args, e)
	}
	os.Remove(b.progFile)
	stop := b.watchPartitions(ctx)
	defer stop()
	r := b.newRunner(ctx)
	if err := r.Run("link", b.kit.Dir, cxx, args); err != nil {
		if ctx.Err() != nil {
			return ctx.Err()
		}
		if strings.Contains(err.Error(), "killed") {
			return userErrorf("The linker was stopped, probably because the computer ran out of memory. Close other programs and run again with --jobs 1. Details: {log}")
		}
		return userErrorf("Linking the game failed. Details: {log}")
	}
	return nil
}

// watchPartitions turns the progress file of the make stand-in (partitions of the LTO link done) into progress lines.
func (b *Builder) watchPartitions(ctx context.Context) (stop func()) {
	quit := make(chan struct{})
	finished := make(chan struct{})
	go func() {
		defer close(finished)
		t := time.NewTicker(500 * time.Millisecond)
		defer t.Stop()
		last := ""
		for {
			select {
			case <-quit:
				return
			case <-ctx.Done():
				return
			case <-t.C:
				data, err := os.ReadFile(b.progFile)
				if err != nil || string(data) == last {
					continue
				}
				last = string(data)
				var d, tot int
				if n, _ := fmt.Sscanf(last, "%d %d", &d, &tot); n == 2 {
					b.Rep.Progress(d, tot)
				}
			}
		}
	}()
	return func() { close(quit); <-finished }
}

func (b *Builder) link(ctx context.Context) (string, error) {
	elf := filepath.Join(b.work, "infected.elf")
	if err := b.runLink(ctx, "", elf); err != nil {
		return "", err
	}
	// weak pthread functions would crash the game at its first thread
	re := regexp.MustCompile(b.kit.PostLinkCheck.FailIfLineMatches)
	nm, err := b.kit.toolOf(b.kit.PostLinkCheck.Tool)
	if err != nil {
		return "", err
	}
	v := b.vars()
	v["out"], v["elf"] = elf, elf
	args, err := v.ExpandArgs(b.kit.PostLinkCheck.Args, nil)
	if err != nil {
		return "", err
	}
	bad := false
	if err := b.newRunner(ctx).RunLines("check", b.kit.Dir, nm, args, func(l string) {
		if re.MatchString(l) {
			bad = true
		}
	}); err != nil {
		if ctx.Err() != nil {
			return "", ctx.Err()
		}
		return "", userErrorf("Checking the linked game failed. Details: {log}")
	}
	if bad {
		b.log.Printf("post-link check failed: %s", b.kit.PostLinkCheck.Message)
		return "", userErrorf("The linked game is not usable (internal error: %s). Details: {log}", b.kit.PostLinkCheck.Message)
	}

	// room after the code segment for vita-elf-create
	free, err := b.roomAfterCode(ctx, elf)
	if err != nil {
		return "", err
	}
	need := b.kit.TextPad.MinFreeBytes
	b.log.Printf("== room after the code segment: %d bytes (need %d)", free, need)
	if free < need {
		pad := b.kit.TextPad.Hint + need + 4096 - free
		b.log.Printf("== padding .text by %d bytes and linking again", pad)
		padObj := filepath.Join(b.work, "text_pad.o")
		cxx, err := b.kit.toolOf(b.kit.TextPad.Fix.Tool)
		if err != nil {
			return "", err
		}
		v := b.vars()
		v["pad"] = strconv.Itoa(pad)
		v["padObject"] = padObj
		fargs, err := v.ExpandArgs(b.kit.TextPad.Fix.Args, nil)
		if err != nil {
			return "", err
		}
		if err := b.newRunner(ctx).Run("text pad", b.kit.Dir, cxx, fargs); err != nil {
			if ctx.Err() != nil {
				return "", ctx.Err()
			}
			return "", userErrorf("Preparing the game for the Vita failed. Details: {log}")
		}
		if err := b.runLink(ctx, padObj, elf); err != nil {
			return "", err
		}
		free, err = b.roomAfterCode(ctx, elf)
		if err != nil {
			return "", err
		}
		if free < need {
			return "", userErrorf("The linked game does not fit the Vita's layout (internal error: %d bytes free after the code, %d needed). Details: {log}", free, need)
		}
	}
	return elf, nil
}

var loadLineRE = regexp.MustCompile(`^\s*LOAD\s+(.*)$`)

// roomAfterCode is VirtAddr(LOAD 1) - (VirtAddr(LOAD 0) + FileSiz(LOAD 0)) of readelf -lW output, or 1 MiB when there is one segment.
func (b *Builder) roomAfterCode(ctx context.Context, elf string) (int, error) {
	readelf, err := b.kit.toolOf(b.kit.TextPad.Check.Tool)
	if err != nil {
		return 0, err
	}
	v := b.vars()
	v["elf"], v["out"] = elf, elf
	args, err := v.ExpandArgs(b.kit.TextPad.Check.Args, nil)
	if err != nil {
		return 0, err
	}
	var loads [][]string
	err = b.newRunner(ctx).RunLines("readelf", b.kit.Dir, readelf, args, func(l string) {
		if m := loadLineRE.FindStringSubmatch(l); m != nil {
			loads = append(loads, strings.Fields(m[1]))
		}
	})
	if err != nil {
		if ctx.Err() != nil {
			return 0, ctx.Err()
		}
		return 0, userErrorf("Checking the linked game failed. Details: {log}")
	}
	return roomFromLoads(loads)
}

// roomFromLoads takes the fields after "LOAD" of each program header line: Offset VirtAddr PhysAddr FileSiz MemSiz Flg Align.
func roomFromLoads(loads [][]string) (int, error) {
	if len(loads) < 2 {
		return 1 << 20, nil
	}
	parse := func(f []string, i int) (int64, error) {
		if len(f) <= i {
			return 0, errors.New("short line")
		}
		return strconv.ParseInt(strings.TrimPrefix(f[i], "0x"), 16, 64)
	}
	a0, e1 := parse(loads[0], 1)
	s0, e2 := parse(loads[0], 3)
	a1, e3 := parse(loads[1], 1)
	if e1 != nil || e2 != nil || e3 != nil {
		return 0, userErrorf("I cannot read the layout of the linked game (readelf output). Details: {log}")
	}
	return int(a1 - (a0 + s0)), nil
}

// ---------------------------------------------------------------------------------------------------- 5. package

func (b *Builder) packageVPK(ctx context.Context, elf string) (string, error) {
	velf := filepath.Join(b.work, "infected.velf")
	eboot := filepath.Join(b.work, "eboot.bin")
	v := b.vars()
	// forward slashes: vita-elf-create takes the module name from the path it is given, and a Windows backslash path made it "\infected.elf"
	v["elf"], v["velf"], v["eboot"] = filepath.ToSlash(elf), filepath.ToSlash(velf), filepath.ToSlash(eboot)
	for _, st := range []struct {
		name string
		tool string
		args []string
		fail string
	}{
		{"vita-elf-create", "elfCreate", b.kit.ElfCreate.Args, "Converting the game for the Vita failed."},
		{"vita-make-fself", "fself", b.kit.Fself.Args, "Signing the game for the Vita failed."},
	} {
		tool, err := b.kit.toolOf(st.tool)
		if err != nil {
			return "", err
		}
		args, err := v.ExpandArgs(st.args, nil)
		if err != nil {
			return "", err
		}
		if err := b.newRunner(ctx).Run(st.name, b.kit.Dir, tool, args); err != nil {
			if ctx.Err() != nil {
				return "", ctx.Err()
			}
			return "", userErrorf("%s Details: {log}", st.fail)
		}
	}
	out := filepath.Join(b.Opt.OutDir, b.kit.Vpk.Zip)
	if err := writeVPK(b.kit, eboot, out); err != nil {
		return "", userErrorf("I cannot write %s: %v.", out, err)
	}
	return out, ctx.Err()
}

// writeVPK writes the package: the kit's game/vpk files plus the eboot, in the order of kit.json, deflated, with fixed dates and
// permissions, so that the same inputs give the same file on every computer.
func writeVPK(k *Kit, ebootPath, out string) error {
	tmp := out + ".tmp"
	f, err := os.Create(tmp)
	if err != nil {
		return err
	}
	ok := false
	defer func() {
		f.Close()
		if !ok {
			os.Remove(tmp)
		}
	}()
	zw := zip.NewWriter(f)
	zw.RegisterCompressor(zip.Deflate, func(w io.Writer) (io.WriteCloser, error) { return flate.NewWriter(w, flate.BestCompression) })
	for _, name := range k.Vpk.Entries {
		src := k.Path(strings.TrimSuffix(k.Vpk.FromKit, "/") + "/" + name)
		if name == k.Vpk.Eboot {
			src = ebootPath
		}
		data, err := os.ReadFile(src)
		if err != nil {
			return err
		}
		h := &zip.FileHeader{Name: name, Method: zip.Deflate}
		h.SetMode(0o644)
		h.ModifiedDate = (2026-1980)<<9 | 1<<5 | 1 // 2026-01-01 00:00:00, like kit_tool.py
		h.ModifiedTime = 0
		w, err := zw.CreateHeader(h)
		if err != nil {
			return err
		}
		if _, err := io.Copy(w, bytes.NewReader(data)); err != nil {
			return err
		}
	}
	if err := zw.Close(); err != nil {
		return err
	}
	if err := f.Close(); err != nil {
		return err
	}
	if err := os.Rename(tmp, out); err != nil {
		return err
	}
	ok = true
	return nil
}
