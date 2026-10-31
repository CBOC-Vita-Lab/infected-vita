//go:build linux

package main

import (
	"bufio"
	"os"
	"strconv"
	"strings"
)

// freeRAMBytes is the memory that can be used without swapping (MemAvailable), or 0 when unknown.
func freeRAMBytes() int64 {
	f, err := os.Open("/proc/meminfo")
	if err != nil {
		return 0
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		if strings.HasPrefix(sc.Text(), "MemAvailable:") {
			fs := strings.Fields(sc.Text())
			if len(fs) >= 2 {
				kb, _ := strconv.ParseInt(fs[1], 10, 64)
				return kb * 1024
			}
		}
	}
	return 0
}

// macOSMajor is 0 off macOS.
func macOSMajor() int { return 0 }
