package leangen_test

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"testing"

	"github.com/CBOC-Vita-Lab/infected-vita/builder/leangen"
	"github.com/CBOC-Vita-Lab/infected-vita/builder/psp"
)

// TestInfectedMatchesPythonReference runs the whole translation of the real disc and compares every generated file byte for byte
// with the output of the Python generator (recomp/lean_gen.py). It needs the player's own disc, which is never part of the
// repository:
//
//	INFECTED_ISO=/path/Infected.iso INFECTED_REF=/path/to/python/output go test ./leangen -run Infected -v
//
// INFECTED_PROFILE may name the profile (default: ../../recomp/infected_profile.json).
func TestInfectedMatchesPythonReference(t *testing.T) {
	iso, ref := os.Getenv("INFECTED_ISO"), os.Getenv("INFECTED_REF")
	if iso == "" || ref == "" {
		t.Skip("set INFECTED_ISO and INFECTED_REF to compare with the Python generator's output")
	}
	profilePath := os.Getenv("INFECTED_PROFILE")
	if profilePath == "" {
		profilePath = filepath.Join("..", "..", "recomp", "infected_profile.json")
	}
	profile, err := os.ReadFile(profilePath)
	if err != nil {
		t.Fatal(err)
	}
	boot, err := psp.ReadFile(iso, "PSP_GAME/SYSDIR/BOOT.BIN")
	if err != nil {
		t.Fatal(err)
	}
	sum := func(b []byte) string { h := sha256.Sum256(b); return hex.EncodeToString(h[:]) }
	if got := sum(boot); got != "952d4bb2c08209f4cbc0020d95c4cc601e9944cad5c7ade4c1d060fe8daab1bc" {
		t.Fatalf("BOOT.BIN sha256 %s: this is not the supported disc", got)
	}
	image, err := psp.BuildImage(boot)
	if err != nil {
		t.Fatal(err)
	}
	if got := sum(image); got != "332e061aa4b00cb1ebd82f3a40247a31e28d177dbe452dbe8a2dc4b74ec3c564" {
		t.Fatalf("relocated image sha256 %s", got)
	}
	out := t.TempDir()
	if err := leangen.Generate(image, boot, profile, out, nil); err != nil {
		t.Fatal(err)
	}
	want, err := os.ReadDir(ref)
	if err != nil {
		t.Fatal(err)
	}
	got, err := os.ReadDir(out)
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != len(want) {
		t.Errorf("%d files generated, the reference has %d", len(got), len(want))
	}
	same := 0
	for _, e := range want {
		a, err := os.ReadFile(filepath.Join(ref, e.Name()))
		if err != nil {
			t.Fatal(err)
		}
		b, err := os.ReadFile(filepath.Join(out, e.Name()))
		if err != nil {
			t.Errorf("%s: not generated", e.Name())
			continue
		}
		if bytes.Equal(a, b) {
			same++
		} else {
			t.Errorf("%s differs (%d bytes, reference %d)", e.Name(), len(b), len(a))
		}
	}
	t.Logf("%d of %d files are byte-identical", same, len(want))
}
