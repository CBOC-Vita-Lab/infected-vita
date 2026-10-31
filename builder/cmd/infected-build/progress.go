package main

import (
	"fmt"
	"io"
	"strings"
	"sync"
	"time"
)

// Reporter writes what the builder is doing: machine lines for the installer (--progress) or friendly text for a person.
//
// Machine protocol (stdout, one per line):
//
//	@step <text>             a new phase starts
//	@progress <done> <total> progress within the current phase
//	@done <absolute path>    last line on success
type Reporter struct {
	mu       sync.Mutex
	w        io.Writer
	machine  bool
	tty      bool // human output on a terminal: redraw one line with \r
	now      func() time.Time
	start    time.Time
	stepAt   time.Time
	stepNo   int
	stepMax  int
	stepName string
	lastSent int // last progress sent, in the step's own units (-1: none)
	maxDone  int // the highest value seen in this step: progress never goes backwards
	maxTotal int
	lastAt   time.Time
	lastPct  int
	drawn    bool // a progress line is on screen (tty)
}

// NewReporter makes a reporter. steps is the number of phases (for "[2/5]").
func NewReporter(w io.Writer, machine, tty bool, steps int) *Reporter {
	r := &Reporter{w: w, machine: machine, tty: tty && !machine, now: time.Now, stepMax: steps, lastSent: -1, lastPct: -1}
	r.start = r.now()
	return r
}

// Step ends the current phase (human mode prints how long it took) and starts the next.
func (r *Reporter) Step(name string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.endStepLocked()
	r.stepNo++
	r.stepName = name
	r.stepAt = r.now()
	r.lastSent, r.lastPct = -1, -1
	r.maxDone, r.maxTotal = 0, 0
	r.lastAt = time.Time{}
	if r.machine {
		fmt.Fprintf(r.w, "@step %s\n", oneLine(name))
		return
	}
	fmt.Fprintf(r.w, "[%d/%d] %s\n", r.stepNo, r.stepMax, name)
}

func (r *Reporter) endStepLocked() {
	if r.stepName == "" {
		return
	}
	if r.drawn {
		fmt.Fprint(r.w, "\r"+strings.Repeat(" ", 70)+"\r")
		r.drawn = false
	}
	if !r.machine {
		fmt.Fprintf(r.w, "      done in %s\n", FormatDuration(r.now().Sub(r.stepAt)))
	}
	r.stepName = ""
}

// Progress reports done of total within the current phase. Calls are cheap; output is thinned out.
func (r *Reporter) Progress(done, total int) {
	if total <= 0 {
		return
	}
	if done > total {
		done = total
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if total == r.maxTotal && done < r.maxDone {
		return // the translator reports its phases out of order now and then; the bar must not step back
	}
	r.maxDone, r.maxTotal = done, total
	now := r.now()
	if r.machine {
		if done == r.lastSent || (done != total && now.Sub(r.lastAt) < 250*time.Millisecond) {
			return
		}
		r.lastSent, r.lastAt = done, now
		fmt.Fprintf(r.w, "@progress %d %d\n", done, total)
		return
	}
	pct := done * 100 / total
	if r.tty {
		if pct == r.lastPct && done != total {
			return
		}
		r.lastPct = pct
		fmt.Fprintf(r.w, "\r      %s %3d%%  %s ", Bar(done, total, 30), pct, FormatDuration(now.Sub(r.stepAt)))
		r.drawn = true
		return
	}
	// not a terminal (a log, a pipe): a line for every 10 percent
	if pct/10 == r.lastPct/10 && r.lastPct >= 0 {
		return
	}
	r.lastPct = pct
	fmt.Fprintf(r.w, "      %d%%\n", pct)
}

// Note prints a line of text for a person (not in machine mode: readers of --progress ignore stdout noise, but the log has it).
func (r *Reporter) Note(format string, a ...interface{}) {
	if r.machine {
		return
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.drawn {
		fmt.Fprint(r.w, "\r"+strings.Repeat(" ", 70)+"\r")
		r.drawn = false
	}
	fmt.Fprintf(r.w, format+"\n", a...)
}

// Done ends the last phase and reports success.
func (r *Reporter) Done(path string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.endStepLocked()
	if r.machine {
		fmt.Fprintf(r.w, "@done %s\n", path)
		return
	}
	fmt.Fprintf(r.w, "\nDone: %s (copy it to your Vita and install it with VitaShell)\n", path)
	fmt.Fprintf(r.w, "Total time: %s\n", FormatDuration(r.now().Sub(r.start)))
}

// Abort ends a run that failed: the progress line is cleared so that the error that follows reads cleanly.
func (r *Reporter) Abort() {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.drawn {
		fmt.Fprint(r.w, "\r"+strings.Repeat(" ", 70)+"\r")
		r.drawn = false
	}
}

// Bar draws "[#####-----]".
func Bar(done, total, width int) string {
	if total <= 0 {
		total = 1
	}
	if done < 0 {
		done = 0
	}
	if done > total {
		done = total
	}
	n := done * width / total
	return "[" + strings.Repeat("#", n) + strings.Repeat("-", width-n) + "]"
}

// FormatDuration renders 7s, 2m05s, 1h03m.
func FormatDuration(d time.Duration) string {
	s := int(d.Round(time.Second) / time.Second)
	switch {
	case s < 60:
		return fmt.Sprintf("%ds", s)
	case s < 3600:
		return fmt.Sprintf("%dm%02ds", s/60, s%60)
	}
	return fmt.Sprintf("%dh%02dm", s/3600, (s%3600)/60)
}

func oneLine(s string) string { return strings.NewReplacer("\r", " ", "\n", " ").Replace(s) }
