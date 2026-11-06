package main

import (
	"fmt"
	"regexp"
	"strconv"
	"strings"
)

// Helpers of the system probes that are plain text parsing, kept out of the per-OS files so that they are tested everywhere.

var vmStatRE = regexp.MustCompile(`^(Pages free|Pages inactive|Pages speculative):\s+(\d+)\.`)
var pageSizeRE = regexp.MustCompile(`page size of (\d+) bytes`)

// parseVMStat adds up the free, inactive and speculative pages of `vm_stat` output and returns bytes.
func parseVMStat(out string) int64 {
	page := int64(4096)
	var pages int64
	for _, l := range strings.Split(out, "\n") {
		if m := pageSizeRE.FindStringSubmatch(l); m != nil {
			page, _ = strconv.ParseInt(m[1], 10, 64)
		}
		if m := vmStatRE.FindStringSubmatch(l); m != nil {
			n, _ := strconv.ParseInt(m[2], 10, 64)
			pages += n
		}
	}
	return pages * page
}

// macVersion is a macOS version as far as the builder cares: "14" (14.x) or "10.13". Up to macOS 10.15 the second number is the
// release ("10.13" is High Sierra), from macOS 11 on it is not and is left out.
type macVersion struct{ Major, Minor int }

func (v macVersion) String() string {
	if v.Major == 10 {
		return fmt.Sprintf("10.%d", v.Minor)
	}
	return strconv.Itoa(v.Major)
}

// less: v is an older macOS than o. 14 (no minor) is the same as 14.0.
func (v macVersion) less(o macVersion) bool {
	if v.Major != o.Major {
		return v.Major < o.Major
	}
	return v.Major == 10 && v.Minor < o.Minor
}

// UnmarshalJSON reads kit.json "requires.macos": a number (14) or a string ("10.13").
func (v *macVersion) UnmarshalJSON(b []byte) error {
	s := strings.Trim(strings.TrimSpace(string(b)), `"`)
	m := parseMacVersion(s)
	if m.Major == 0 {
		return fmt.Errorf("requires.macos %q is not a macOS version", s)
	}
	*v = m
	return nil
}

// parseMacVersion reads "14.2.1" as {14, 0} and "10.15.7" as {10, 15}; {0, 0} when it is not a version.
func parseMacVersion(v string) macVersion {
	parts := strings.Split(strings.TrimSpace(v), ".")
	major, err := strconv.Atoi(parts[0])
	if err != nil || major < 0 {
		return macVersion{}
	}
	m := macVersion{Major: major}
	if major == 10 && len(parts) > 1 {
		m.Minor, _ = strconv.Atoi(parts[1])
	}
	return m
}

// defaultJobs is the number of parallel compiles: one per CPU, but no more than the free memory allows at about 1.5 GB each
// (a unit of the game needs that much), and at least one. freeRAM 0 means unknown.
func defaultJobs(cpus int, freeRAM int64) int {
	j := cpus
	if freeRAM > 0 {
		if byRAM := int(freeRAM * 2 / (3 << 30)); byRAM < j { // freeRAM / 1.5 GiB
			j = byRAM
		}
	}
	if j < 1 {
		j = 1
	}
	return j
}
