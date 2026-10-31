package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strings"
)

// Kit is the recipe of the builder kit (kit.json, format 1) and the folder it lives in. The kit maker is builder/kit/make-kit.sh;
// builder/README.md documents every field.
type Kit struct {
	Dir string `json:"-"`

	Format int    `json:"format"`
	Name   string `json:"name"`
	OS     string `json:"os"`
	Arch   string `json:"arch"`
	Game   struct {
		TitleID    string `json:"titleId"`
		Name       string `json:"name"`
		Version    string `json:"version"`
		BootSha256 string `json:"bootSha256"`
	} `json:"game"`
	Requires struct {
		MacOS int `json:"macos"` // lowest macOS major version the kit's compilers run on
	} `json:"requires"`
	Tools   map[string]string `json:"tools"`
	Profile string            `json:"profile"`
	Compile struct {
		Tool    string   `json:"tool"`
		Args    []string `json:"args"`
		Cwd     string   `json:"cwd"`
		Sources []string `json:"sources"`
	} `json:"compile"`
	Archive struct {
		Tool        string   `json:"tool"`
		Args        []string `json:"args"`
		Order       []string `json:"order"`
		DeleteFirst bool     `json:"deleteFirst"`
	} `json:"archive"`
	Link struct {
		Tool      string   `json:"tool"`
		ArgsFile  string   `json:"argsFile"`
		ExtraArgs []string `json:"extraArgs"`
		Jobs      int      `json:"jobs"`
	} `json:"link"`
	PostLinkCheck struct {
		Tool              string   `json:"tool"`
		Args              []string `json:"args"`
		FailIfLineMatches string   `json:"failIfLineMatches"`
		Message           string   `json:"message"`
	} `json:"postLinkCheck"`
	TextPad struct {
		Object       string `json:"object"`
		Hint         int    `json:"hint"`
		MinFreeBytes int    `json:"minFreeBytes"`
		Check        struct {
			Tool string   `json:"tool"`
			Args []string `json:"args"`
		} `json:"check"`
		Fix struct {
			Source string   `json:"source"`
			Tool   string   `json:"tool"`
			Args   []string `json:"args"`
		} `json:"fix"`
	} `json:"textPad"`
	ElfCreate struct {
		Tool string   `json:"tool"`
		Args []string `json:"args"`
	} `json:"elfCreate"`
	Fself struct {
		Tool string   `json:"tool"`
		Args []string `json:"args"`
	} `json:"fself"`
	Vpk struct {
		Zip     string   `json:"zip"`
		Entries []string `json:"entries"`
		FromKit string   `json:"fromKit"`
		Eboot   string   `json:"eboot"`
	} `json:"vpk"`
	MinDiskBytes int64              `json:"minDiskBytes"`
	Files        map[string]KitFile `json:"files"`

	linkArgs []string // lines of game/link/link.args, once loaded
}

// KitFile is one row of the "files" table.
type KitFile struct {
	Sha256 string `json:"sha256"`
	Size   int64  `json:"size"`
}

// userError is a failure with a message for the player: one plain-English line, no stack, no jargon.
type userError struct{ msg string }

func (e *userError) Error() string { return e.msg }

func userErrorf(format string, a ...interface{}) error { return &userError{fmt.Sprintf(format, a...)} }

// LoadKit reads kit.json from dir and checks that it is complete enough to run.
func LoadKit(dir string) (*Kit, error) {
	dir, err := filepath.Abs(dir)
	if err != nil {
		return nil, err
	}
	data, err := os.ReadFile(filepath.Join(dir, "kit.json"))
	if err != nil {
		if os.IsNotExist(err) {
			return nil, userErrorf("The builder kit is missing: there is no kit.json next to infected-build (%s). Unpack the whole builder archive and run it from there.", dir)
		}
		return nil, userErrorf("The builder kit cannot be read: %v", err)
	}
	return ParseKit(dir, data)
}

// ParseKit parses the text of a kit.json for the kit folder dir.
func ParseKit(dir string, data []byte) (*Kit, error) {
	k := &Kit{Dir: dir}
	if err := json.Unmarshal(data, k); err != nil {
		return nil, userErrorf("The builder kit is damaged (kit.json is not valid: %v).", err)
	}
	if k.Format != 1 {
		return nil, userErrorf("This builder kit has format %d, which this infected-build does not know. Download the newest builder.", k.Format)
	}
	need := func(ok bool, what string) error {
		if !ok {
			return userErrorf("The builder kit is incomplete (kit.json has no %s).", what)
		}
		return nil
	}
	checks := []struct {
		ok   bool
		what string
	}{
		{k.Game.BootSha256 != "", "game.bootSha256"},
		{k.Profile != "", "profile"},
		{len(k.Compile.Args) > 0 && len(k.Compile.Sources) > 0, "compile recipe"},
		{len(k.Archive.Args) > 0 && len(k.Archive.Order) > 0, "archive recipe"},
		{k.Link.ArgsFile != "", "link recipe"},
		{k.PostLinkCheck.FailIfLineMatches != "", "postLinkCheck"},
		{k.TextPad.Object != "" && k.TextPad.MinFreeBytes > 0, "textPad recipe"},
		{len(k.ElfCreate.Args) > 0, "elfCreate recipe"},
		{len(k.Fself.Args) > 0, "fself recipe"},
		{k.Vpk.Zip != "" && len(k.Vpk.Entries) > 0 && k.Vpk.Eboot != "", "vpk recipe"},
	}
	for _, c := range checks {
		if err := need(c.ok, c.what); err != nil {
			return nil, err
		}
	}
	for _, name := range []string{"cxx", "ar", "nm", "readelf", "elfCreate", "fself"} {
		if k.Tools[name] == "" {
			return nil, userErrorf("The builder kit is incomplete (kit.json names no tool %q).", name)
		}
	}
	if _, err := regexp.Compile(k.PostLinkCheck.FailIfLineMatches); err != nil {
		return nil, userErrorf("The builder kit is damaged (postLinkCheck pattern: %v).", err)
	}
	if k.Link.Jobs <= 0 {
		k.Link.Jobs = 2
	}
	if k.Vpk.FromKit == "" {
		k.Vpk.FromKit = "game/vpk/"
	}
	return k, nil
}

// Path turns a path relative to the kit (slashes) into a native absolute path.
func (k *Kit) Path(rel string) string { return filepath.Join(k.Dir, filepath.FromSlash(rel)) }

// Tool is the absolute path of a tool named in "tools".
func (k *Kit) Tool(name string) string { return k.Path(k.Tools[name]) }

// toolOf resolves the "tool" field of a step (a key of "tools").
func (k *Kit) toolOf(key string) (string, error) {
	if _, ok := k.Tools[key]; !ok {
		return "", userErrorf("The builder kit is damaged (it names the tool %q but does not define it).", key)
	}
	return k.Tool(key), nil
}

// Verify checks that every file of the "files" table exists with its recorded size and, with deep, its SHA-256.
func (k *Kit) Verify(deep bool) error {
	names := make([]string, 0, len(k.Files))
	for n := range k.Files {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, n := range names {
		f := k.Files[n]
		p := k.Path(n)
		st, err := os.Stat(p)
		if err != nil {
			return userErrorf("The builder kit is incomplete: %s is missing. Unpack the whole builder archive again.", n)
		}
		if st.Size() != f.Size {
			return userErrorf("The builder kit is damaged: %s has the wrong size. Unpack the builder archive again.", n)
		}
		if deep {
			got, err := fileSha256(p)
			if err != nil {
				return userErrorf("The builder kit cannot be read: %v", err)
			}
			if got != f.Sha256 {
				return userErrorf("The builder kit is damaged: %s does not match its checksum. Download the builder again.", n)
			}
		}
	}
	for _, key := range []string{"cxx", "ar", "nm", "readelf", "elfCreate", "fself"} {
		if st, err := os.Stat(k.Tool(key)); err != nil || st.IsDir() {
			return userErrorf("The builder kit is incomplete: %s is missing.", k.Tools[key])
		}
	}
	return nil
}

func fileSha256(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

// LinkArgs returns the lines of the kit's link.args, with kit.json's link.extraArgs appended.
func (k *Kit) LinkArgs() ([]string, error) {
	if k.linkArgs != nil {
		return k.linkArgs, nil
	}
	data, err := os.ReadFile(k.Path(k.Link.ArgsFile))
	if err != nil {
		return nil, userErrorf("The builder kit is incomplete: %s is missing.", k.Link.ArgsFile)
	}
	var out []string
	for _, l := range strings.Split(strings.ReplaceAll(string(data), "\r\n", "\n"), "\n") {
		if l != "" {
			out = append(out, l)
		}
	}
	out = append(out, k.Link.ExtraArgs...)
	k.linkArgs = out
	return out, nil
}

// ---------------------------------------------------------------------------------------------------- placeholders

// Vars holds the values of the {placeholders} of kit.json. Names are written without braces.
type Vars map[string]string

var placeholderRE = regexp.MustCompile(`\{[A-Za-z][A-Za-z0-9]*\}`)

// Expand replaces every {name} of s. A placeholder that is not in v is an error (a kit made by a newer kit maker).
func (v Vars) Expand(s string) (string, error) {
	var bad string
	out := placeholderRE.ReplaceAllStringFunc(s, func(m string) string {
		val, ok := v[m[1:len(m)-1]]
		if !ok {
			bad = m
			return m
		}
		return val
	})
	if bad != "" {
		return "", userErrorf("The builder kit needs a newer infected-build (unknown placeholder %s in kit.json).", bad)
	}
	return out, nil
}

// ExpandArgs expands each argument. An argument that is exactly a list placeholder ({objs}) is replaced by all of its items.
func (v Vars) ExpandArgs(args []string, lists map[string][]string) ([]string, error) {
	out := make([]string, 0, len(args))
	for _, a := range args {
		if len(a) > 2 && a[0] == '{' && a[len(a)-1] == '}' {
			if items, ok := lists[a[1:len(a)-1]]; ok {
				out = append(out, items...)
				continue
			}
		}
		e, err := v.Expand(a)
		if err != nil {
			return nil, err
		}
		out = append(out, e)
	}
	return out, nil
}

// requiredMacOS is the lowest macOS the kit runs on (kit.json "requires.macos", else 14: the compilers of the VitaSDK
// release are built for macOS 14).
func (k *Kit) requiredMacOS() int {
	if k.Requires.MacOS > 0 {
		return k.Requires.MacOS
	}
	return 14
}

// thisHost is "<goos>/<goarch>", compared with the kit's own os/arch only for a warning in the log.
func thisHost() string { return runtime.GOOS + "/" + runtime.GOARCH }
