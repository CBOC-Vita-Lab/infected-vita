//go:build linux || darwin

package main

import (
	"os"
	"os/exec"
	"syscall"
)

// setProcAttr puts a tool into its own process group, so that killTree can stop it together with everything it started
// (gcc runs cc1plus, lto1, as, ld and, for the LTO link, this program as make).
func setProcAttr(cmd *exec.Cmd) { cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true} }

func killTree(p *os.Process) error {
	if p == nil {
		return nil
	}
	if err := syscall.Kill(-p.Pid, syscall.SIGKILL); err != nil {
		return p.Kill()
	}
	return nil
}

// freeDiskBytes is the space available to this user on the volume that holds dir.
func freeDiskBytes(dir string) (int64, error) {
	var st syscall.Statfs_t
	if err := syscall.Statfs(dir, &st); err != nil {
		return 0, err
	}
	return int64(uint64(st.Bavail) * uint64(st.Bsize)), nil
}

// lowerPriority makes the build a little less greedy, so the computer stays usable (children inherit it).
func lowerPriority() { syscall.Setpriority(syscall.PRIO_PROCESS, 0, 10) }

func isTerminal(f *os.File) bool {
	st, err := f.Stat()
	return err == nil && st.Mode()&os.ModeCharDevice != 0
}

// shortPath: only Windows has short names.
func shortPath(p string) string { return p }
