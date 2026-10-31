//go:build windows

package main

import (
	"os"
	"os/exec"
	"strconv"
	"syscall"
	"unsafe"
)

var (
	kernel32               = syscall.NewLazyDLL("kernel32.dll")
	procGetDiskFreeSpaceEx = kernel32.NewProc("GetDiskFreeSpaceExW")
	procGlobalMemoryStatus = kernel32.NewProc("GlobalMemoryStatusEx")
	procSetPriorityClass   = kernel32.NewProc("SetPriorityClass")
	procGetShortPathName   = kernel32.NewProc("GetShortPathNameW")
	procGetCurrentProcess  = kernel32.NewProc("GetCurrentProcess")
	procCreateJobObject    = kernel32.NewProc("CreateJobObjectW")
	procSetInfoJobObject   = kernel32.NewProc("SetInformationJobObject")
	procAssignProcessToJob = kernel32.NewProc("AssignProcessToJobObject")
)

const (
	createNoWindow           = 0x08000000
	belowNormalPriorityClass = 0x00004000
)

// setProcAttr: no console window flashes up for every tool when the builder is started by a program with a window.
func setProcAttr(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: createNoWindow}
}

// killTree stops a tool and everything it started (taskkill is part of every Windows).
func killTree(p *os.Process) error {
	if p == nil {
		return nil
	}
	kill := exec.Command("taskkill", "/T", "/F", "/PID", strconv.Itoa(p.Pid))
	setProcAttr(kill)
	if err := kill.Run(); err != nil {
		return p.Kill()
	}
	return nil
}

// The parts of JOBOBJECT_EXTENDED_LIMIT_INFORMATION that matter here (64-bit layout; on a 32-bit build the size is wrong, the call
// fails and the job is simply not used).
type jobBasicLimit struct {
	PerProcessUserTimeLimit int64
	PerJobUserTimeLimit     int64
	LimitFlags              uint32
	MinimumWorkingSetSize   uintptr
	MaximumWorkingSetSize   uintptr
	ActiveProcessLimit      uint32
	Affinity                uintptr
	PriorityClass           uint32
	SchedulingClass         uint32
}

type jobExtendedLimit struct {
	Basic                 jobBasicLimit
	IoInfo                [6]uint64
	ProcessMemoryLimit    uintptr
	JobMemoryLimit        uintptr
	PeakProcessMemoryUsed uintptr
	PeakJobMemoryUsed     uintptr
}

const (
	jobObjectLimitKillOnJobClose = 0x2000
	jobObjectExtendedLimitInfo   = 9
)

// killChildrenWithMe puts this program into a job object that kills every process in it when the last handle to the job closes,
// which is when this program ends in any way (closed window, the installer's TerminateProcess, a crash): without it the compiler
// processes it started would run on, and a half-finished link would hold files in the work folder. Processes started later (gcc,
// cc1plus, lto1, ld, this program as make) inherit the job. Best effort: a computer where this program already runs inside a job that
// does not allow nesting (Windows 7) just does without.
func killChildrenWithMe() {
	h, _, _ := procCreateJobObject.Call(0, 0)
	if h == 0 {
		return
	}
	var info jobExtendedLimit
	info.Basic.LimitFlags = jobObjectLimitKillOnJobClose
	r, _, _ := procSetInfoJobObject.Call(h, jobObjectExtendedLimitInfo, uintptr(unsafe.Pointer(&info)), unsafe.Sizeof(info))
	if r == 0 {
		syscall.CloseHandle(syscall.Handle(h))
		return
	}
	self, _, _ := procGetCurrentProcess.Call()
	if r, _, _ := procAssignProcessToJob.Call(h, self); r == 0 {
		syscall.CloseHandle(syscall.Handle(h))
	}
	// the handle stays open until the program ends
}

func freeDiskBytes(dir string) (int64, error) {
	p, err := syscall.UTF16PtrFromString(dir)
	if err != nil {
		return 0, err
	}
	var avail, total, free uint64
	r, _, e := procGetDiskFreeSpaceEx.Call(uintptr(unsafe.Pointer(p)), uintptr(unsafe.Pointer(&avail)), uintptr(unsafe.Pointer(&total)), uintptr(unsafe.Pointer(&free)))
	if r == 0 {
		return 0, e
	}
	return int64(avail), nil
}

type memoryStatusEx struct {
	Length               uint32
	MemoryLoad           uint32
	TotalPhys            uint64
	AvailPhys            uint64
	TotalPageFile        uint64
	AvailPageFile        uint64
	TotalVirtual         uint64
	AvailVirtual         uint64
	AvailExtendedVirtual uint64
}

func freeRAMBytes() int64 {
	var m memoryStatusEx
	m.Length = uint32(unsafe.Sizeof(m))
	if r, _, _ := procGlobalMemoryStatus.Call(uintptr(unsafe.Pointer(&m))); r == 0 {
		return 0
	}
	return int64(m.AvailPhys)
}

func lowerPriority() {
	h, _, _ := procGetCurrentProcess.Call()
	procSetPriorityClass.Call(h, belowNormalPriorityClass)
}

func isTerminal(f *os.File) bool {
	var mode uint32
	r, _, _ := kernel32.NewProc("GetConsoleMode").Call(f.Fd(), uintptr(unsafe.Pointer(&mode)))
	return r != 0
}

func macOSMajor() int { return 0 }

// shortPath is the 8.3 name of a path ("C:\\Users\\JOHNSM~1"), which has no space; the path itself when there is none.
func shortPath(p string) string {
	in, err := syscall.UTF16PtrFromString(p)
	if err != nil {
		return p
	}
	buf := make([]uint16, 1024)
	n, _, _ := procGetShortPathName.Call(uintptr(unsafe.Pointer(in)), uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf)))
	if n == 0 || int(n) > len(buf) {
		return p
	}
	return syscall.UTF16ToString(buf[:n])
}
