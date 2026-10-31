//go:build linux || darwin

package main

import (
	"os/exec"
	"strconv"
	"strings"
	"syscall"
)

// processAlive: the process exists and is not a zombie.
func processAlive(pid int) bool {
	if syscall.Kill(pid, 0) != nil {
		return false
	}
	out, err := exec.Command("ps", "-o", "stat=", "-p", strconv.Itoa(pid)).Output()
	if err != nil {
		return false
	}
	s := strings.TrimSpace(string(out))
	return s != "" && s[0] != 'Z'
}
