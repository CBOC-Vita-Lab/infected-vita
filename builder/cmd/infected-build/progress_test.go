package main

import (
	"bytes"
	"strings"
	"testing"
	"time"
)

type fakeClock struct{ t time.Time }

func (c *fakeClock) now() time.Time { return c.t }

func newTestReporter(machine, tty bool) (*Reporter, *bytes.Buffer, *fakeClock) {
	var buf bytes.Buffer
	clk := &fakeClock{t: time.Unix(1000, 0)}
	r := NewReporter(&buf, machine, tty, 5)
	r.now = clk.now
	r.start, r.stepAt = clk.t, clk.t
	return r, &buf, clk
}

func TestMachineProtocol(t *testing.T) {
	r, buf, clk := newTestReporter(true, true)
	r.Step("Reading your disc")
	r.Step("Compiling (this is the long part)")
	r.Progress(0, 74)
	r.Progress(0, 74) // same value: not repeated
	clk.t = clk.t.Add(time.Second)
	r.Progress(5, 74)
	r.Progress(6, 74) // too soon after the last line: dropped
	clk.t = clk.t.Add(time.Second)
	r.Progress(74, 74)
	r.Progress(80, 74) // clamped, and it is a repeat
	r.Done("/out/infected.vpk")
	want := "@step Reading your disc\n@step Compiling (this is the long part)\n@progress 0 74\n@progress 5 74\n@progress 74 74\n@done /out/infected.vpk\n"
	if buf.String() != want {
		t.Fatalf("got:\n%s\nwant:\n%s", buf.String(), want)
	}
	for _, l := range strings.Split(strings.TrimSpace(buf.String()), "\n") {
		if !strings.HasPrefix(l, "@") {
			t.Fatalf("machine mode printed a plain line: %q", l)
		}
	}
}

func TestMachineStepIsOneLine(t *testing.T) {
	r, buf, _ := newTestReporter(true, false)
	r.Step("a\nb")
	if buf.String() != "@step a b\n" {
		t.Fatalf("%q", buf.String())
	}
}

func TestHumanOutputPlain(t *testing.T) {
	r, buf, clk := newTestReporter(false, false)
	r.Step("Reading your disc")
	clk.t = clk.t.Add(3 * time.Second)
	r.Step("Translating the game")
	for i := 0; i <= 100; i++ {
		r.Progress(i, 100)
	}
	clk.t = clk.t.Add(125 * time.Second)
	r.Done("/out/infected.vpk")
	out := buf.String()
	for _, want := range []string{"[1/5] Reading your disc", "      done in 3s", "[2/5] Translating the game", "      50%", "      done in 2m05s",
		"Done: /out/infected.vpk (copy it to your Vita and install it with VitaShell)", "Total time: 2m08s"} {
		if !strings.Contains(out, want) {
			t.Errorf("missing %q in:\n%s", want, out)
		}
	}
	if strings.Contains(out, "\r") {
		t.Error("a non-terminal output must not use carriage returns")
	}
	if n := strings.Count(out, "%\n"); n < 9 || n > 12 {
		t.Errorf("expected about one line per 10 percent, got %d", n)
	}
}

func TestHumanOutputTerminal(t *testing.T) {
	r, buf, _ := newTestReporter(false, true)
	r.Step("Linking")
	r.Progress(1, 4)
	r.Progress(2, 4)
	if !strings.Contains(buf.String(), "\r      [###############---------------]  50%") {
		t.Fatalf("%q", buf.String())
	}
	r.Done("/x.vpk")
	if !strings.Contains(buf.String(), "Done: /x.vpk") {
		t.Fatal("no done line")
	}
}

func TestBarAndDuration(t *testing.T) {
	if Bar(0, 10, 10) != "[----------]" || Bar(10, 10, 10) != "[##########]" || Bar(5, 10, 10) != "[#####-----]" || Bar(99, 10, 4) != "[####]" {
		t.Fatal("bar")
	}
	for d, want := range map[time.Duration]string{0: "0s", 7 * time.Second: "7s", 125 * time.Second: "2m05s", 3780 * time.Second: "1h03m"} {
		if got := FormatDuration(d); got != want {
			t.Errorf("%v: %s want %s", d, got, want)
		}
	}
}

func TestProgressNeverGoesBack(t *testing.T) {
	r, buf, clk := newTestReporter(true, false)
	r.Step("Translating the game")
	r.Progress(50, 100)
	clk.t = clk.t.Add(time.Second)
	r.Progress(45, 100)
	clk.t = clk.t.Add(time.Second)
	r.Progress(60, 100)
	if buf.String() != "@step Translating the game\n@progress 50 100\n@progress 60 100\n" {
		t.Fatalf("%q", buf.String())
	}
}
