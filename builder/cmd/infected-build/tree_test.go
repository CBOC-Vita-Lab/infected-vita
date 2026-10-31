package main

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"
)

// The test binary plays two parts (see TestMain): "parent" starts "child" and waits for it; "child" writes its process id to a file
// and sleeps for a minute.
func treeHelper() {
	switch os.Getenv("IB_TREE") {
	case "parent":
		cmd := exec.Command(os.Args[0])
		cmd.Env = append(os.Environ(), "IB_TREE=child")
		cmd.Run()
		os.Exit(0)
	case "child":
		os.WriteFile(os.Getenv("IB_PIDFILE"), []byte(strconv.Itoa(os.Getpid())), 0o644)
		time.Sleep(time.Minute)
		os.Exit(0)
	}
}

// Cancelling a run must stop the tool and the processes it started (gcc starts cc1plus, lto1, ld and this program as make).
func TestCancelKillsProcessTree(t *testing.T) {
	dir := t.TempDir()
	pidFile := filepath.Join(dir, "child.pid")
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	r := &Runner{Ctx: ctx, Env: append(os.Environ(), "IB_TREE=parent", "IB_PIDFILE="+pidFile)}
	errc := make(chan error, 1)
	go func() { errc <- r.Run("tree", dir, os.Args[0], nil) }()
	var pid int
	for end := time.Now().Add(30 * time.Second); time.Now().Before(end); time.Sleep(50 * time.Millisecond) {
		if b, err := os.ReadFile(pidFile); err == nil {
			if n, err := strconv.Atoi(strings.TrimSpace(string(b))); err == nil {
				pid = n
				break
			}
		}
	}
	if pid == 0 {
		t.Fatal("the grandchild did not start")
	}
	if !processAlive(pid) {
		t.Fatalf("grandchild %d is not running before the cancel", pid)
	}
	cancel()
	select {
	case err := <-errc:
		if err == nil {
			t.Fatal("run returned no error after cancel")
		}
	case <-time.After(30 * time.Second):
		t.Fatal("run did not return after cancel")
	}
	for end := time.Now().Add(15 * time.Second); processAlive(pid); time.Sleep(50 * time.Millisecond) {
		if time.Now().After(end) {
			t.Fatalf("grandchild %d is still running after cancel", pid)
		}
	}
}
