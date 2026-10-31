// Command leangen is the test driver of the leangen package: it reads BOOT.BIN out of a PSP disc image, builds the relocated load
// image and runs the recompiler (recomp/lean_gen.py ported to Go) end to end.
//
//	leangen --iso Infected.iso --profile recomp/infected_profile.json --out DIR
package main

import (
	"crypto/sha256"
	"flag"
	"fmt"
	"os"
	"time"

	"github.com/CBOC-Vita-Lab/infected-vita/builder/leangen"
	"github.com/CBOC-Vita-Lab/infected-vita/builder/psp"
)

func main() {
	iso := flag.String("iso", "", "PSP disc image (ISO9660)")
	bootPath := flag.String("boot", "", "BOOT.BIN to use instead of reading it from --iso")
	profile := flag.String("profile", "", "generator profile (recomp/infected_profile.json)")
	out := flag.String("out", "", "output directory")
	verbose := flag.Bool("v", false, "print progress and SHA-256 sums")
	flag.Parse()
	if (*iso == "" && *bootPath == "") || *out == "" {
		fmt.Fprintln(os.Stderr, "usage: leangen --iso DISC.iso --profile PROFILE.json --out DIR")
		os.Exit(2)
	}
	if err := run(*iso, *bootPath, *profile, *out, *verbose); err != nil {
		fmt.Fprintln(os.Stderr, "leangen:", err)
		os.Exit(1)
	}
}

func run(iso, bootPath, profilePath, out string, verbose bool) error {
	t0 := time.Now()
	var boot []byte
	var err error
	if bootPath != "" {
		boot, err = os.ReadFile(bootPath)
	} else {
		boot, err = psp.ReadFile(iso, "PSP_GAME/SYSDIR/BOOT.BIN")
	}
	if err != nil {
		return err
	}
	image, err := psp.BuildImage(boot)
	if err != nil {
		return err
	}
	var prof []byte
	if profilePath != "" {
		if prof, err = os.ReadFile(profilePath); err != nil {
			return err
		}
	}
	if verbose {
		fmt.Printf("BOOT.BIN sha256 %x\nimage.bin sha256 %x\n", sha256.Sum256(boot), sha256.Sum256(image))
	}
	last := -1
	err = leangen.Generate(image, boot, prof, out, func(done, total int) {
		if verbose {
			if p := done * 100 / total; p != last {
				last = p
				fmt.Printf("\r%3d%% ", p)
			}
		}
	})
	if verbose {
		fmt.Printf("\ndone in %.1fs\n", time.Since(t0).Seconds())
	}
	return err
}
