//go:build darwin

package main

import (
	"os/exec"
	"strconv"
	"strings"
)

// freeRAMBytes: free + inactive + speculative pages from vm_stat, or half the installed memory when vm_stat cannot be read.
func freeRAMBytes() int64 {
	if out, err := exec.Command("/usr/bin/vm_stat").Output(); err == nil {
		return parseVMStat(string(out))
	}
	if out, err := exec.Command("/usr/sbin/sysctl", "-n", "hw.memsize").Output(); err == nil {
		if n, err := strconv.ParseInt(strings.TrimSpace(string(out)), 10, 64); err == nil {
			return n / 2
		}
	}
	return 0
}

// macOSVersion is the version of macOS from sw_vers (14 for macOS 14.x, 10.15 for Catalina); zero when unknown.
func macOSVersion() macVersion {
	out, err := exec.Command("/usr/bin/sw_vers", "-productVersion").Output()
	if err != nil {
		return macVersion{}
	}
	return parseMacVersion(string(out))
}
