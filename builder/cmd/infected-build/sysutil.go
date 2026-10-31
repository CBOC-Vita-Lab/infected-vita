package main

import (
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

// parseMajor is the major version number of a version string ("14.2.1" gives 14).
func parseMajor(v string) int {
	v = strings.TrimSpace(v)
	if i := strings.IndexByte(v, '.'); i >= 0 {
		v = v[:i]
	}
	n, _ := strconv.Atoi(v)
	return n
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
