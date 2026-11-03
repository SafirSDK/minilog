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
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/chromedp/cdproto/input"
	"github.com/chromedp/chromedp"
)

// browserPage is the viewer open in a headless browser.
type browserPage struct {
	t     *testing.T
	ctx   context.Context
	lines atomic.Int64 // /lines requests served

	// How long each /lines response is held back, in nanoseconds: longer than
	// the poll interval makes polls overlap, as on a slow link.
	linesDelay atomic.Int64

	mu      sync.Mutex
	exports []url.Values // the query of each /export request, in order
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
		switch r.URL.Path {
		case "/lines":
			p.lines.Add(1)
			time.Sleep(time.Duration(p.linesDelay.Load()))
		case "/export":
			p.mu.Lock()
			p.exports = append(p.exports, r.URL.Query())
			p.mu.Unlock()
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
	startJSCoverage(t, ctx)
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
	return jsonRecordWith(msg, "INFO")
}

func jsonRecordWith(msg, severity string) string {
	return fmt.Sprintf(`{"rcv":"2026-11-03T10:00:00Z","src":"10.0.0.1","proto":"RFC3164",`+
		`"facility":"daemon","severity":%q,"hostname":"host","app":"app","pid":null,`+
		`"msgid":null,"msg_time":null,"message":%q}`, severity, msg)
}

// appendRecords appends a record per message to path, creating it if need be.
func appendRecords(t *testing.T, path string, msgs ...string) {
	t.Helper()
	lines := make([]string, len(msgs))
	for i, m := range msgs {
		lines[i] = jsonRecord(m)
	}
	appendLines(t, path, lines...)
}

// appendLines appends raw JSONL lines to path, creating it if need be.
func appendLines(t *testing.T, path string, lines ...string) {
	t.Helper()
	f, err := os.OpenFile(path, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o600)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	if _, err := f.WriteString(strings.Join(lines, "\n") + "\n"); err != nil {
		t.Fatal(err)
	}
}

// numbered returns prefix00, prefix01, … for n messages; more digits past 100.
func numbered(prefix string, n int) []string {
	width := len(fmt.Sprint(n - 1))
	width = max(width, 2)
	out := make([]string, n)
	for i := range out {
		out[i] = fmt.Sprintf("%s%0*d", prefix, width, i)
	}
	return out
}

// lastExport returns the query of the latest /export request, or nil.
func (p *browserPage) lastExport() url.Values {
	p.mu.Lock()
	defer p.mu.Unlock()
	if len(p.exports) == 0 {
		return nil
	}
	return p.exports[len(p.exports)-1]
}

// press sends a key to the page, as if typed with nothing focused.
func (p *browserPage) press(key string) {
	p.t.Helper()
	eval[bool](p, `document.activeElement && document.activeElement.blur(), true`)
	p.run(chromedp.KeyEvent(key))
}

// wheel scrolls the log by dy pixels, up when negative, with a real wheel event.
func (p *browserPage) wheel(dy float64) {
	p.t.Helper()
	box := eval[[]float64](p, `(() => { const r = document.getElementById('main').getBoundingClientRect();
		return [r.left + r.width / 2, r.top + r.height / 2]; })()`)
	p.run(chromedp.MouseEvent(input.DispatchMouseEventTypeMouseWheel, box[0], box[1],
		func(e *input.DispatchMouseEventParams) {
			// The protocol wants both deltas, and a zero is left out of the
			// message, so the horizontal one is a fraction of a pixel.
			e.DeltaX = 0.01
			e.DeltaY = dy
		}))
}

// atBottom reports whether the log is scrolled to its end.
func (p *browserPage) atBottom() bool {
	p.t.Helper()
	return eval[bool](p, `(() => { const m = document.getElementById('main');
		return m.scrollHeight - m.scrollTop - m.clientHeight < 2; })()`)
}

// hidden reports whether the element with this id is hidden.
func (p *browserPage) hidden(id string) bool {
	p.t.Helper()
	return eval[bool](p, fmt.Sprintf(`document.getElementById(%q).hidden`, id))
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

// Every field of a record is text from whoever can send a datagram, and must
// be shown as text: in the row, in its expanded detail and in the facility
// chips. One unescaped field would run their script in the viewer.
func TestBrowser_RecordFieldsAreShownAsTextNeverAsHTML(t *testing.T) {
	const payload = `<img src=x onerror="window.injected=true">`
	fields := []string{"rcv", "src", "proto", "facility", "severity", "hostname", "app", "pid",
		"msgid", "msg_time", "message"}
	var lines []string
	for _, f := range fields {
		rec := map[string]string{}
		for _, g := range fields {
			rec[g] = "x"
		}
		rec["rcv"] = "2026-11-03T10:00:00Z"
		rec[f] = payload + " in " + f
		b, err := json.Marshal(rec)
		if err != nil {
			t.Fatal(err)
		}
		lines = append(lines, string(b))
	}
	dir := t.TempDir()
	active := filepath.Join(dir, "syslog.jsonl")
	appendLines(t, active, lines...)
	p := openViewer(t, []Sink{{Name: "main", Path: active, MaxFiles: 5}})
	p.waitUntil("the records are shown", 10*time.Second, p.showsLast(payload+" in message"))

	// Expand every row, and open the facility list, which is built from them.
	eval[bool](p, `[...document.querySelectorAll('#log-body tr')].forEach(tr => tr.click()), true`)
	p.click("fac-btn")
	time.Sleep(500 * time.Millisecond) // a broken image fails, and fires onerror, almost at once

	if eval[bool](p, `window.injected === true`) {
		t.Errorf("a record's field ran as script")
	}
	if n := eval[int](p, `document.querySelectorAll('[onerror]').length`); n != 0 {
		t.Errorf("a record's field became %d element(s): %s", n,
			eval[string](p, `[...document.querySelectorAll('[onerror]')].map(e => e.parentElement.outerHTML).join('\n')`))
	}
	// textContent: some columns are hidden by default, and innerText skips them.
	text := eval[string](p, `document.getElementById('log-body').textContent`)
	for _, f := range []string{"message", "hostname", "app"} {
		if !strings.Contains(text, payload+" in "+f) {
			t.Errorf("the %s field is not shown as written", f)
		}
	}
}

// A burst larger than a batch drains over several polls, and every record
// shows exactly once, in order. Polls that overlapped used to render the same
// batch twice.
func TestBrowser_ABurstShowsEveryRecordOnceInOrder(t *testing.T) {
	dir := t.TempDir()
	active := filepath.Join(dir, "syslog.jsonl")
	appendRecords(t, active, "start")
	p := openViewer(t, []Sink{{Name: "main", Path: active, MaxFiles: 5}})
	p.waitUntil("the tail is shown", 10*time.Second, p.showsLast("start"))

	// Each response takes longer than the 500 ms poll interval, so a new poll
	// is due before the last one has answered.
	p.linesDelay.Store(int64(700 * time.Millisecond))
	burst := numbered("B", 1500)
	for i := 0; i < len(burst); i += 300 {
		appendRecords(t, active, burst[i:i+300]...)
		time.Sleep(100 * time.Millisecond)
	}
	p.waitUntil("the burst is shown", 30*time.Second, p.showsLast(burst[len(burst)-1]))
	time.Sleep(1500 * time.Millisecond) // any poll still in flight lands

	want := append([]string{"start"}, burst...)
	if got := p.messages(); strings.Join(got, ",") != strings.Join(want, ",") {
		t.Errorf("rows differ from the records: %d rows for %d records", len(got), len(want))
	}
}

// #49: under steady traffic, one small wheel step up leaves the tail and stays
// left. The step is smaller than the 80 px within which each update used to
// pull the view back to the bottom, which is what made leaving impossible:
// now no update pulls the view back down or adds rows below what is read.
// Space follows again, and the newest records are shown.
func TestBrowser_AWheelStepUpUnderTrafficPausesAndSpaceFollowsAgain(t *testing.T) {
	dir := t.TempDir()
	active := filepath.Join(dir, "syslog.jsonl")
	appendRecords(t, active, numbered("L", 300)...)
	p := openViewer(t, []Sink{{Name: "main", Path: active, MaxFiles: 5}})
	p.waitUntil("the tail is shown", 10*time.Second, p.showsLast("L299"))

	stop := make(chan struct{})
	done := make(chan struct{})
	stopTraffic := sync.OnceFunc(func() { close(stop); <-done })
	defer stopTraffic()
	go func() {
		defer close(done)
		for i := 0; ; i++ {
			select {
			case <-stop:
				return
			case <-time.After(50 * time.Millisecond):
				appendRecords(t, active, fmt.Sprintf("T%04d", i))
			}
		}
	}()
	time.Sleep(time.Second) // following: rows keep arriving

	p.wheel(-40)
	p.waitUntil("paused", 5*time.Second, func() bool { return !p.following() })
	rows := p.messages()
	time.Sleep(1500 * time.Millisecond)
	if got := p.messages(); strings.Join(got, ",") != strings.Join(rows, ",") {
		t.Errorf("rows changed while paused: %d before, %d after", len(rows), len(got))
	}
	if p.atBottom() {
		t.Errorf("the view was pulled back to the bottom")
	}
	if p.hidden("jump-tail-btn") {
		t.Errorf("Jump to tail is not offered")
	}

	p.press(" ")
	p.waitUntil("following again", 5*time.Second, p.following)
	stopTraffic()
	appendRecords(t, active, "after")
	p.waitUntil("the newest record is shown", 10*time.Second, p.showsLast("after"))
}

// A search match outside the loaded rows is shown with the rows after it. The
// end of those rows is not the end of the log, so reaching it must not start
// following from there, which would skip everything in between; Follow loads
// the newest rows instead.
func TestBrowser_TheBottomOfASearchResultIsNotTheTail(t *testing.T) {
	dir := t.TempDir()
	active := filepath.Join(dir, "syslog.jsonl")
	lines := numbered("L", 600)
	lines[5] = "needle"
	appendRecords(t, active, lines...)
	p := openViewer(t, []Sink{{Name: "main", Path: active, MaxFiles: 5}})
	p.waitUntil("the tail is shown", 10*time.Second, p.showsLast("L599"))

	p.run(chromedp.SendKeys(chromedp.ID("search"), "needle\r"))
	// The rows from the match on, and possibly a few older ones loaded above
	// it: the match is near the top of the screen, which loads older rows.
	p.waitUntil("the match is shown", 10*time.Second, func() bool {
		m := p.messages()
		return containsAll(m, []string{"needle"}) && !containsAll(m, []string{"L599"})
	})
	time.Sleep(time.Second) // the smooth scroll to the match has finished
	p.wheel(100000)         // down, to the bottom, as a person would
	p.waitUntil("at the bottom of the result", 5*time.Second, p.atBottom)
	appendRecords(t, active, "new")
	time.Sleep(1500 * time.Millisecond)
	if p.following() {
		t.Errorf("following started from the end of the search result")
	}
	if m := p.messages(); m[len(m)-1] == "new" {
		t.Errorf("a new record was added below the search result")
	}

	p.click("follow-btn")
	p.waitUntil("the newest record is shown", 10*time.Second, p.showsLast("new"))
	if m := p.messages(); !containsAll(m, []string{"L598", "L599"}) {
		t.Errorf("the rows before the newest are missing: %v", m[len(m)-5:])
	}
}

// The severity chips send syslog's short names (err, warn, crit, emerg) and
// minilog writes the long ones. Hiding DEBUG once hid the four whose names
// differ as well.
func TestBrowser_HidingOneSeverityHidesOnlyThatOne(t *testing.T) {
	sevs := []string{"EMERGENCY", "ALERT", "CRITICAL", "ERROR", "WARNING", "NOTICE", "INFO", "DEBUG"}
	var lines []string
	for _, s := range sevs {
		lines = append(lines, jsonRecordWith("a "+s, s))
	}
	dir := t.TempDir()
	active := filepath.Join(dir, "syslog.jsonl")
	appendLines(t, active, lines...)
	p := openViewer(t, []Sink{{Name: "main", Path: active, MaxFiles: 5}})
	p.waitUntil("the records are shown", 10*time.Second, p.showsLast("a DEBUG"))

	p.click("sev-btn")
	p.run(chromedp.Click(chromedp.CSS(`#sev-chips [data-sev="DEBUG"]`)))
	p.waitUntil("the tail is reloaded without DEBUG", 10*time.Second, p.showsLast("a INFO"))

	want := []string{"a EMERGENCY", "a ALERT", "a CRITICAL", "a ERROR", "a WARNING", "a NOTICE", "a INFO"}
	if got := p.messages(); strings.Join(got, ",") != strings.Join(want, ",") {
		t.Errorf("rows %v, want %v", got, want)
	}
}

// What the Export dropdown asks the server for: the format, the time range,
// and the filters only when ticked — never the Clear Screen bound.
func TestBrowser_ExportRequestsWhatTheDropdownSays(t *testing.T) {
	dir := t.TempDir()
	active := filepath.Join(dir, "syslog.jsonl")
	appendRecords(t, active, "abc", "def")
	p := openViewer(t, []Sink{{Name: "main", Path: active, MaxFiles: 5}})
	p.waitUntil("the tail is shown", 10*time.Second, p.showsLast("def"))

	p.click("clear-btn")
	p.click("filter-btn")
	p.run(chromedp.SendKeys(chromedp.ID("inc-input"), "abc"))
	p.click("inc-add")

	export := func(format, preset string, applyFilters bool) url.Values {
		t.Helper()
		p.mu.Lock()
		before := len(p.exports)
		p.mu.Unlock()
		p.click("export-btn")
		p.run(chromedp.SetValue(chromedp.ID("export-format"), format))
		p.run(chromedp.Click(chromedp.CSS(fmt.Sprintf(`[data-preset=%q]`, preset))))
		if eval[bool](p, `document.getElementById('export-apply-filters').checked`) != applyFilters {
			p.click("export-apply-filters")
		}
		p.click("export-save")
		p.waitUntil("the export is requested", 10*time.Second, func() bool {
			p.mu.Lock()
			defer p.mu.Unlock()
			return len(p.exports) > before
		})
		return p.lastExport()
	}

	q := export("csv", "hour", true)
	if q.Get("sink") != "main" || q.Get("format") != "csv" || q.Get("inc") != "abc" || q.Has("to") {
		t.Errorf("with filters: %v", q)
	}
	if q.Has("since") {
		t.Errorf("the Clear Screen bound was sent: %v", q)
	}
	from, err := time.Parse(time.RFC3339, q.Get("from"))
	if err != nil || time.Since(from) < 59*time.Minute || time.Since(from) > 61*time.Minute {
		t.Errorf("from = %q, want about an hour ago", q.Get("from"))
	}

	q = export("text", "all", false)
	if q.Get("format") != "text" || q.Has("inc") || q.Has("from") || q.Has("to") {
		t.Errorf("without filters: %v", q)
	}
}

// With compress, history older than .1 is compressed and only Export reads
// it. The top of the log says so, and its link opens Export.
func TestBrowser_TheTopOfTheLogPointsToCompressedHistory(t *testing.T) {
	dir := t.TempDir()
	active := filepath.Join(dir, "syslog.jsonl")
	appendRecords(t, filepath.Join(dir, "syslog.1.jsonl"), "one")
	if err := os.WriteFile(filepath.Join(dir, "syslog.2.jsonl.gz"), nil, 0o600); err != nil {
		t.Fatal(err)
	}
	appendRecords(t, active, "now")
	p := openViewer(t, []Sink{{Name: "main", Path: active, MaxFiles: 5, Compress: true}})
	p.waitUntil("the tail is shown", 10*time.Second, p.showsLast("now"))

	p.waitUntil("the note is shown", 5*time.Second, func() bool { return !p.hidden("older-note") })
	p.click("older-note-export")
	if p.hidden("export-panel") {
		t.Errorf("the note's link did not open Export")
	}
}

// Switching sink and Reset both start from a clean view: the Clear Screen
// bound and the filters go, and every record shows again.
func TestBrowser_SwitchingSinkAndResetStartClean(t *testing.T) {
	dir := t.TempDir()
	mainPath := filepath.Join(dir, "main.jsonl")
	otherPath := filepath.Join(dir, "other.jsonl")
	appendRecords(t, mainPath, numbered("M", 5)...)
	appendRecords(t, otherPath, numbered("O", 5)...)
	p := openViewer(t, []Sink{
		{Name: "main", Path: mainPath, MaxFiles: 5},
		{Name: "other", Path: otherPath, MaxFiles: 5},
	})
	p.waitUntil("the tail is shown", 10*time.Second, p.showsLast("M04"))

	selectSink := func(name string) {
		eval[bool](p, fmt.Sprintf(`(() => { const s = document.getElementById('sink-select');
			s.value = %q; s.dispatchEvent(new Event('change')); return true; })()`, name))
	}

	p.click("clear-btn")
	selectSink("other")
	p.waitUntil("the other sink is shown", 10*time.Second, p.showsLast("O04"))
	if got := p.messages(); !containsAll(got, numbered("O", 5)) {
		t.Errorf("after switching sink, rows %v", got)
	}

	selectSink("main")
	p.waitUntil("main is shown", 10*time.Second, p.showsLast("M04"))
	p.click("clear-btn")
	p.click("filter-btn")
	p.run(chromedp.SendKeys(chromedp.ID("exc-input"), "m0"))
	p.click("exc-add")
	p.click("reset-btn")
	p.waitUntil("every record shows again", 10*time.Second, func() bool {
		return containsAll(p.messages(), numbered("M", 5))
	})
	// Reset restores the default exclusions, so the filters are not empty:
	// what has to go is the pattern added here.
	if chips := eval[string](p, `document.getElementById('exc-chips').textContent`); strings.Contains(chips, "m0") {
		t.Errorf("the exclusion is still there after Reset: %q", chips)
	}
}
