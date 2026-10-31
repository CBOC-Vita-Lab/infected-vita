//go:build windows

package main

import "syscall"

func processAlive(pid int) bool {
	const synchronize, queryLimited, waitTimeout = 0x100000, 0x1000, 0x102
	h, err := syscall.OpenProcess(synchronize|queryLimited, false, uint32(pid))
	if err != nil {
		return false
	}
	defer syscall.CloseHandle(h)
	ev, _ := syscall.WaitForSingleObject(h, 0)
	return ev == waitTimeout
}
