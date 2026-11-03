// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

package main

// Tests of the page itself, in a headless Chrome driven over the DevTools
// protocol, against the real handlers and log files on disk. They cover what
// only shows up with the page's own polling, scrolling and state: the handler
// tests above them cannot see the page lose its place.
//
// They need Chrome or Chromium. Without one they are skipped, as they are
// with -short.

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/chromedp/chromedp"
)

// browserPage is the viewer open in a headless browser.
type browserPage struct {
	t     *testing.T
	ctx   context.Context
	lines atomic.Int64 // /lines requests served
}

// openViewer serves sinks the way the viewer does and opens the page.
func openViewer(t *testing.T, sinks []Sink) *browserPage {
	t.Helper()
	if testing.Short() {
		t.Skip("browser test; skipped with -short")
	}

	p := &browserPage{t: t}
	mux := http.NewServeMux()
	registerHandlers(mux, sinks)
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/lines" {
			p.lines.Add(1)
		}
		mux.ServeHTTP(w, r)
	}))
	t.Cleanup(ts.Close)

	opts := append(chromedp.DefaultExecAllocatorOptions[:],
		chromedp.NoSandbox, // CI runs as a user Chrome's sandbox may not support
		chromedp.WindowSize(1200, 800),
	)
	if path := os.Getenv("MINILOG_TEST_CHROME"); path != "" {
		opts = append(opts, chromedp.ExecPath(path))
	}
	// The timeout bounds the whole test, browser included.
	timeoutCtx, cancelTimeout := context.WithTimeout(context.Background(), 60*time.Second)
	allocCtx, cancelAlloc := chromedp.NewExecAllocator(timeoutCtx, opts...)
	ctx, cancelCtx := chromedp.NewContext(allocCtx)
	t.Cleanup(func() { cancelCtx(); cancelAlloc(); cancelTimeout() })

	if err := chromedp.Do(ctx); err != nil {
		var notFound *exec.Error
		if errors.As(err, &notFound) || strings.Contains(err.Error(), "executable file not found") {
			t.Skipf("no Chrome or Chromium to run the page in: %v", err)
		}
		t.Fatalf("starting the browser: %v", err)
	}
	p.ctx = ctx
	p.run(chromedp.Navigate(ts.URL))
	return p
}

func (p *browserPage) run(actions ...chromedp.Action[chromedp.Void]) {
	p.t.Helper()
	if err := chromedp.Do(p.ctx, actions...); err != nil {
		p.t.Fatalf("browser: %v", err)
	}
}

// click clicks the element with this id.
func (p *browserPage) click(id string) {
	p.t.Helper()
	p.run(chromedp.Click(chromedp.ID(id)))
}

// eval evaluates a JavaScript expression in the page.
func eval[T any](p *browserPage, expr string) T {
	p.t.Helper()
	v, err := chromedp.Run(p.ctx, chromedp.Evaluate[T](expr))
	if err != nil {
		p.t.Fatalf("browser: evaluating %s: %v", expr, err)
	}
	return v
}

// messages returns the Message column of every row on screen, top to bottom.
func (p *browserPage) messages() []string {
	p.t.Helper()
	return eval[[]string](p,
		`[...document.querySelectorAll('#log-body td.col-msg')].map(td => td.textContent)`)
}

// following reports whether the Follow button says the view is following.
func (p *browserPage) following() bool {
	p.t.Helper()
	return eval[string](p, `document.getElementById('follow-btn').getAttribute('aria-pressed')`) == "true"
}

// waitUntil polls cond until it holds, failing the test after timeout.
func (p *browserPage) waitUntil(what string, timeout time.Duration, cond func() bool) {
	p.t.Helper()
	deadline := time.Now().Add(timeout)
	for !cond() {
		if time.Now().After(deadline) {
			p.t.Fatalf("timed out waiting until %s; rows on screen: %v", what, p.messages())
		}
		time.Sleep(50 * time.Millisecond)
	}
}

func (p *browserPage) showsLast(msg string) func() bool {
	return func() bool {
		m := p.messages()
		return len(m) > 0 && m[len(m)-1] == msg
	}
}

// jsonRecord is one JSONL record as minilog writes it, with message msg.
func jsonRecord(msg string) string {
	return fmt.Sprintf(`{"rcv":"2026-11-03T10:00:00Z","src":"10.0.0.1","proto":"RFC3164",`+
		`"facility":"daemon","severity":"INFO","hostname":"host","app":"app","pid":null,`+
		`"msgid":null,"msg_time":null,"message":%q}`, msg)
}

// appendRecords appends a record per message to path, creating it if need be.
func appendRecords(t *testing.T, path string, msgs ...string) {
	t.Helper()
	f, err := os.OpenFile(path, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o600)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	for _, m := range msgs {
		if _, err := f.WriteString(jsonRecord(m) + "\n"); err != nil {
			t.Fatal(err)
		}
	}
}

// numbered returns prefix00, prefix01, … for n messages.
func numbered(prefix string, n int) []string {
	out := make([]string, n)
	for i := range out {
		out[i] = fmt.Sprintf("%s%02d", prefix, i)
	}
	return out
}

// /lines requests the page makes in d, once it has settled.
func (p *browserPage) linesRequestsIn(d time.Duration) int64 {
	before := p.lines.Load()
	time.Sleep(d)
	return p.lines.Load() - before
}

// contains reports whether every one of want is among got.
func containsAll(got, want []string) bool {
	seen := map[string]bool{}
	for _, g := range got {
		seen[g] = true
	}
	for _, w := range want {
		if !seen[w] {
			return false
		}
	}
	return true
}

func TestBrowser_ResumingAtTheBottomCarriesOnFromWhereItPaused(t *testing.T) {
	dir := t.TempDir()
	active := filepath.Join(dir, "syslog.jsonl")
	appendRecords(t, active, numbered("L", 10)...)
	p := openViewer(t, []Sink{{Name: "main", Path: active, MaxFiles: 5}})
	p.waitUntil("the tail is shown", 10*time.Second, p.showsLast("L09"))

	p.click("follow-btn")
	p.waitUntil("paused", 5*time.Second, func() bool { return !p.following() })
	appendRecords(t, active, numbered("N", 5)...)
	time.Sleep(time.Second) // several polls: nothing is added while paused
	if got := p.messages(); got[len(got)-1] != "L09" {
		t.Fatalf("rows were added while paused: %v", got)
	}

	p.click("follow-btn")
	p.waitUntil("the new lines are shown", 10*time.Second, p.showsLast("N04"))
	if got := p.messages(); !containsAll(got, append(numbered("L", 10), numbered("N", 5)...)) {
		t.Errorf("lines missing after resuming: %v", got)
	}
}

// A rotation while paused moves every offset in the chain, the cursor's too.
// Resuming from that cursor once the chain has grown back past it skipped
// lines without a sign: here N00 to N09, read past from the old cursor.
func TestBrowser_ResumingAfterARotationWhilePausedSkipsNothing(t *testing.T) {
	dir := t.TempDir()
	active := filepath.Join(dir, "syslog.jsonl")
	rotated := func(n int) string { return filepath.Join(dir, fmt.Sprintf("syslog.%d.jsonl", n)) }
	appendRecords(t, rotated(1), numbered("A", 10)...)
	appendRecords(t, active, numbered("L", 10)...)
	// compress: the chain is .1 and the active file, as minilog's own
	p := openViewer(t, []Sink{{Name: "main", Path: active, MaxFiles: 5, Compress: true}})
	p.waitUntil("the tail is shown", 10*time.Second, p.showsLast("L09"))

	p.click("follow-btn")
	p.waitUntil("paused", 5*time.Second, func() bool { return !p.following() })

	// Rotate as minilog does. The chain is shorter for a while (L + 3 lines
	// against A + L), then grows past where it was (L + 15 lines).
	for _, step := range [][2]string{{rotated(1), rotated(2)}, {active, rotated(1)}} {
		if err := os.Rename(step[0], step[1]); err != nil {
			t.Fatal(err)
		}
	}
	appendRecords(t, active, numbered("N", 3)...)
	time.Sleep(1500 * time.Millisecond)
	appendRecords(t, active, numbered("N", 15)[3:]...)
	time.Sleep(time.Second)

	p.click("follow-btn")
	p.waitUntil("the newest line is shown", 10*time.Second, p.showsLast("N14"))
	if got := p.messages(); !containsAll(got, append(numbered("L", 10), numbered("N", 15)...)) {
		t.Errorf("lines missing after resuming: %v", got)
	}
}

// After Clear Screen, a filter reloads the tail with a lower bound, so a few
// rows can sit at the top of an otherwise empty screen with nothing older to
// load. Loading older rows used to pause the view, which no scrolling could
// undo, and the empty reads behind it repeated in a tight loop.
func TestBrowser_AFewFilteredRowsAfterClearScreenKeepFollowing(t *testing.T) {
	dir := t.TempDir()
	active := filepath.Join(dir, "syslog.jsonl")
	appendRecords(t, active, numbered("L", 10)...)
	p := openViewer(t, []Sink{{Name: "main", Path: active, MaxFiles: 5}})
	p.waitUntil("the tail is shown", 10*time.Second, p.showsLast("L09"))

	p.click("clear-btn")
	appendRecords(t, active, "M00", "M01")
	p.waitUntil("new lines appear", 10*time.Second, p.showsLast("M01"))
	p.click("filter-btn")
	p.run(chromedp.SendKeys(chromedp.ID("inc-input"), "m0"))
	p.click("inc-add")
	time.Sleep(time.Second)

	if !p.following() {
		t.Errorf("the view paused itself")
	}
	// About four polls; the loop made over a hundred.
	if n := p.linesRequestsIn(2 * time.Second); n > 10 {
		t.Errorf("%d /lines requests in 2 s", n)
	}
	appendRecords(t, active, "M02")
	p.waitUntil("a new matching line appears", 10*time.Second, p.showsLast("M02"))
}
