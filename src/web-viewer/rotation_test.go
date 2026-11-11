// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

// rotatedPath returns the filesystem path for rotation generation n.
// n==0 returns activePath itself; n>=1 gives syslog.N.jsonl.
func rotatedPath(activePath string, n int) string {
	if n == 0 {
		return activePath
	}
	ext := filepath.Ext(activePath)
	stem := strings.TrimSuffix(activePath, ext)
	return fmt.Sprintf("%s.%d%s", stem, n, ext)
}

// rotate renames activePath → .1.jsonl, .1 → .2, etc. (oldest falls off at maxFiles).
// Creates a fresh empty activePath. The caller must have closed the file handle first.
func rotate(t *testing.T, activePath string, maxFiles int) {
	t.Helper()
	for n := maxFiles; n >= 1; n-- {
		older := rotatedPath(activePath, n)
		var newer string
		if n == 1 {
			newer = activePath
		} else {
			newer = rotatedPath(activePath, n-1)
		}
		if _, err := os.Stat(newer); err == nil {
			_ = os.Rename(newer, older)
		}
	}
	if err := os.WriteFile(activePath, nil, 0o600); err != nil {
		t.Fatalf("rotate: create new active: %v", err)
	}
}

// TestNoLineLoss is the primary safety net for log line loss during rotation.
//
// It runs two complementary sub-tests:
//
//  1. Forward-scan completeness: after all writes finish, read the entire chain
//     from offset 0 and verify every line number appears exactly once.  This
//     tests that ReadForward / NewFileChain never skip bytes at file boundaries.
//
//  2. Concurrent-poll simulation: a goroutine writes and rotates files while a
//     second goroutine polls /lines?dir=forward exactly as the browser does.
//     On rotation (tail_offset regression) it mirrors the browser's loadTail()
//     by reading the last batch and continuing from there.  It verifies that
//     every line number in the window that cannot have fallen off the chain is
//     received at least once.
func TestNoLineLoss(t *testing.T) {
	const (
		totalLines   = 2000
		linesPerFile = 50
		maxFiles     = 5
	)

	dir := t.TempDir()
	activePath := filepath.Join(dir, "syslog.jsonl")

	sink := Sink{Name: "main", Path: activePath, MaxFiles: maxFiles}
	mux := http.NewServeMux()
	registerHandlers(mux, []Sink{sink})
	ts := httptest.NewServer(mux)
	defer ts.Close()

	// ── Helper: write all lines then rotate on schedule ───────────────────────

	// lineBytes returns the raw JSONL bytes for line number n.
	lineBytes := func(n int) []byte {
		return []byte(fmt.Sprintf("{\"n\":%d}\n", n))
	}

	// writeAll writes totalLines to the active file, rotating every linesPerFile.
	// Returns the list of line numbers that are guaranteed to still be in the
	// chain at the end (i.e. not fallen off the maxFiles window).
	writeAll := func() []int {
		if err := os.WriteFile(activePath, nil, 0o600); err != nil {
			t.Fatalf("writeAll: create: %v", err)
		}
		fh, err := os.OpenFile(activePath, os.O_APPEND|os.O_WRONLY, 0o600)
		if err != nil {
			t.Fatalf("writeAll: open: %v", err)
		}

		var rotations int
		for i := 1; i <= totalLines; i++ {
			if i > 1 && (i-1)%linesPerFile == 0 {
				fh.Close()
				rotate(t, activePath, maxFiles)
				rotations++
				fh, err = os.OpenFile(activePath, os.O_APPEND|os.O_WRONLY, 0o600)
				if err != nil {
					t.Fatalf("writeAll: reopen after rotate: %v", err)
				}
			}
			if _, err := fh.Write(lineBytes(i)); err != nil {
				t.Fatalf("writeAll: write line %d: %v", i, err)
			}
		}
		fh.Close()

		// Lines that have fallen off: the first (rotations - maxFiles) × linesPerFile.
		fallenOff := 0
		if rotations > maxFiles {
			fallenOff = (rotations - maxFiles) * linesPerFile
		}
		var inChain []int
		for n := fallenOff + 1; n <= totalLines; n++ {
			inChain = append(inChain, n)
		}
		return inChain
	}

	// ── Sub-test 1: forward-scan completeness ─────────────────────────────────
	t.Run("forward_scan", func(t *testing.T) {
		inChain := writeAll()

		// Read the entire chain from offset 0 via the HTTP API.
		type resp struct {
			Lines      []string `json:"lines"`
			NextOffset int64    `json:"next_offset"`
			TailOffset int64    `json:"tail_offset"`
		}

		var allLines []string
		var offset int64
		for {
			url := fmt.Sprintf("%s/lines?sink=main&offset=%d&dir=forward&count=500", ts.URL, offset)
			r, err := http.Get(url)
			if err != nil {
				t.Fatalf("GET: %v", err)
			}
			var result resp
			json.NewDecoder(r.Body).Decode(&result)
			r.Body.Close()

			allLines = append(allLines, result.Lines...)
			if result.NextOffset <= offset || len(result.Lines) == 0 {
				break
			}
			offset = result.NextOffset
		}

		seen := make(map[int]int)
		for _, raw := range allLines {
			var rec struct {
				N int `json:"n"`
			}
			if err := json.Unmarshal([]byte(raw), &rec); err == nil {
				seen[rec.N]++
			}
		}

		var missing, duplicates []int
		for _, n := range inChain {
			switch seen[n] {
			case 0:
				missing = append(missing, n)
			default:
				if seen[n] > 1 {
					duplicates = append(duplicates, n)
				}
			}
		}
		if len(missing) > 0 {
			show := missing
			if len(show) > 20 {
				show = show[:20]
			}
			t.Errorf("forward scan MISSING %d lines (first %d): %v",
				len(missing), len(show), show)
		}
		if len(duplicates) > 0 {
			t.Errorf("forward scan DUPLICATED lines: %v", duplicates)
		}
		if len(missing) == 0 && len(duplicates) == 0 {
			t.Logf("OK: all %d in-chain lines readable via ReadForward", len(inChain))
		}
	})

	// ── Sub-test 2: concurrent poll simulation ────────────────────────────────
	// This test verifies that the browser's polling protocol (ReadForward from
	// tailOffset, reset on regression) does not lose lines that are still in
	// the chain at the time they could have been read.
	//
	// Invariant tested: every line that was in the chain at some point during
	// the polling window is eventually delivered to the poller — UNLESS it fell
	// off the chain (maxFiles exceeded) before the poller had a chance to read it.
	// We consider a line "must be received" if it is in the final chain.
	t.Run("concurrent_poll", func(t *testing.T) {
		// Clean up from previous sub-test.
		for n := maxFiles; n >= 1; n-- {
			_ = os.Remove(rotatedPath(activePath, n))
		}
		if err := os.WriteFile(activePath, nil, 0o600); err != nil {
			t.Fatalf("create active: %v", err)
		}

		const (
			writeDelay   = 2 * time.Millisecond
			pollInterval = 3 * time.Millisecond
		)

		// Writer goroutine.
		var writerDone sync.WaitGroup
		writerDone.Add(1)
		go func() {
			defer writerDone.Done()
			fh, err := os.OpenFile(activePath, os.O_APPEND|os.O_WRONLY, 0o600)
			if err != nil {
				t.Errorf("writer open: %v", err)
				return
			}
			for i := 1; i <= totalLines; i++ {
				if i > 1 && (i-1)%linesPerFile == 0 {
					fh.Close()
					rotate(t, activePath, maxFiles)
					fh, err = os.OpenFile(activePath, os.O_APPEND|os.O_WRONLY, 0o600)
					if err != nil {
						t.Errorf("writer reopen: %v", err)
						return
					}
				}
				if _, err := fh.Write(lineBytes(i)); err != nil {
					t.Errorf("writer write %d: %v", i, err)
					return
				}
				time.Sleep(writeDelay)
			}
			fh.Close()
		}()

		// Poller: mirrors browser pollTail() + loadTail() on regression.
		type linesResp struct {
			Lines      []string `json:"lines"`
			Offsets    []int64  `json:"offsets"`
			NextOffset int64    `json:"next_offset"`
			TailOffset int64    `json:"tail_offset"`
		}
		doGet := func(url string) (*linesResp, error) {
			resp, err := http.Get(ts.URL + url)
			if err != nil {
				return nil, err
			}
			defer resp.Body.Close()
			var r linesResp
			if err := json.NewDecoder(resp.Body).Decode(&r); err != nil {
				return nil, err
			}
			return &r, nil
		}

		// seenN tracks which line numbers we've collected (by value, not offset,
		// since offsets are remapped after rotation).
		seenN := make(map[int]bool)

		// tailOffset is the read cursor; chainEnd is the end of the chain as of
		// the last response. The browser keeps both for the same reason: the
		// cursor lags behind the end while a burst drains, so only chainEnd can
		// tell a rotation from ordinary catching up.
		var tailOffset, chainEnd int64
		deadline := time.Now().Add(120 * time.Second)
		writerFinished := false
		drainPasses := 0
		// Whether the loop below read to the end of the chain, rather than
		// being cut off by the deadline. A line unseen is the viewer's fault
		// only if the poller had the chance to see it; without this the two
		// came out as the same message, and "MISSING 1 lines present in final
		// chain" reads like line loss when it can mean the test ran out of
		// time. 120 s because one run on a contended four-core machine
		// measured 34 s of the 60 s this had.
		drained := false

		for time.Now().Before(deadline) {
			r, err := doGet(fmt.Sprintf("/lines?sink=main&offset=%d&dir=forward&count=200", tailOffset))
			if err != nil {
				time.Sleep(pollInterval)
				continue
			}

			if r.TailOffset < chainEnd {
				// Rotation shrank the chain: mirror loadTail().
				// Read the last batch to pick up lines written just before rotation.
				//
				// The cursor moves only once that read has returned. Setting it
				// to the post-rotation end first — which this did — and then
				// finding the read had failed left it past lines nothing had
				// read, so they were never seen again and came out as line
				// loss. loadTail() in the browser does not have the problem:
				// it sets tailStale on a failure and the next poll reloads.
				rt, err := doGet("/lines?sink=main&tail=true&count=200")
				if err != nil {
					time.Sleep(pollInterval)
					continue
				}
				for _, line := range rt.Lines {
					var rec struct {
						N int `json:"n"`
					}
					if json.Unmarshal([]byte(line), &rec) == nil {
						seenN[rec.N] = true
					}
				}
				tailOffset, chainEnd = rt.TailOffset, rt.TailOffset
				continue
			}

			chainEnd = r.TailOffset
			tailOffset = r.NextOffset
			for _, line := range r.Lines {
				var rec struct {
					N int `json:"n"`
				}
				if json.Unmarshal([]byte(line), &rec) == nil {
					seenN[rec.N] = true
				}
			}

			if !writerFinished {
				done := make(chan struct{})
				go func() { writerDone.Wait(); close(done) }()
				select {
				case <-done:
					writerFinished = true
				default:
				}
			}
			if writerFinished {
				drainPasses++
				if drainPasses >= 20 && len(r.Lines) == 0 {
					drained = true
					break
				}
			}
			time.Sleep(pollInterval)
		}
		writerDone.Wait()

		// Determine which lines are still in the final chain.
		finalChain, err := NewFileChain(sink)
		if err != nil {
			t.Fatalf("NewFileChain: %v", err)
		}
		inFinalChain := make(map[int]bool)
		lines, _, _, _, err := finalChain.ReadForward(0, totalLines*2, noFilter())
		if err != nil {
			t.Fatalf("ReadForward final: %v", err)
		}
		for _, raw := range lines {
			var rec struct {
				N int `json:"n"`
			}
			if json.Unmarshal(raw, &rec) == nil {
				inFinalChain[rec.N] = true
			}
		}

		// Every line in the final chain must have been seen by the poller.
		var missing []int
		for n := range inFinalChain {
			if !seenN[n] {
				missing = append(missing, n)
			}
		}
		if len(missing) > 0 {
			show := missing
			if len(show) > 20 {
				show = show[:20]
			}
			if !drained {
				t.Errorf("the poller ran out of time with %d of %d final-chain lines still "+
					"unread (first %d: %v) — the deadline, not line loss; give it longer or "+
					"give the runner less to do", len(missing), len(inFinalChain), len(show), show)
			} else {
				t.Errorf("concurrent poll MISSING %d lines present in final chain (first %d): %v "+
					"— the poller read to the end of the chain and never saw them, which is "+
					"line loss", len(missing), len(show), show)
			}
		} else {
			t.Logf("OK: all %d final-chain lines received by poller (drained=%v)",
				len(inFinalChain), drained)
		}
	})
}

// A rotation between NewFileChain's directory listing and its stat calls must
// not drop a generation. The listing then has nothing at the name the oldest
// generation was renamed to, and every other name holds the next newer file,
// so a chain taken from it as it stands leaves out the oldest generation's
// lines and moves every offset after them. Seen once in CI as one line
// missing from TestNoLineLoss/concurrent_poll; here the rotation is put
// exactly in that window.
func TestAChainIsNotBuiltFromARotationHalfSeen(t *testing.T) {
	dir := t.TempDir()
	active := filepath.Join(dir, "syslog.jsonl")
	sink := Sink{Name: "main", Path: active, MaxFiles: 5}
	for i, path := range []string{rotatedPath(active, 2), rotatedPath(active, 1), active} {
		if err := os.WriteFile(path, []byte(fmt.Sprintf("{\"n\":%d}\n", i+1)), 0o600); err != nil {
			t.Fatal(err)
		}
	}

	rotated := false
	testHookAfterListing = func() {
		if !rotated {
			rotated = true
			rotate(t, active, sink.MaxFiles)
		}
	}
	t.Cleanup(func() { testHookAfterListing = nil })

	fc, err := NewFileChain(sink)
	if err != nil {
		t.Fatal(err)
	}
	if !rotated {
		t.Fatal("the hook never ran")
	}
	lines, _, _, _, err := fc.ReadForward(0, 10, noFilter())
	if err != nil {
		t.Fatal(err)
	}
	var got []string
	for _, l := range lines {
		got = append(got, string(l))
	}
	want := []string{`{"n":1}`, `{"n":2}`, `{"n":3}`}
	if strings.Join(got, " ") != strings.Join(want, " ") {
		t.Errorf("chain after a rotation during its build holds %v, want %v", got, want)
	}
}

// ── Live tail: bursts larger than one batch ──────────────────────────────────

// tailPoller mirrors the cursor handling of the browser's pollTail(): it reads
// forward from a cursor advanced by next_offset, and treats a tail_offset that
// regressed against the previous response as a rotation.
type tailPoller struct {
	t      *testing.T
	ts     *httptest.Server
	params string // extra query params, e.g. "&inc=keep"
	batch  int    // app.js BATCH

	cursor   int64 // tailOffset in app.js
	chainEnd int64 // chainEnd in app.js
	rotated  bool  // set when a poll would have called loadTail()
}

// poll performs one poll and returns the lines it delivered.
func (p *tailPoller) poll() []string {
	p.t.Helper()
	url := fmt.Sprintf("/lines?sink=main&dir=forward&offset=%d&count=%d%s",
		p.cursor, p.batch, p.params)
	var r linesResponse
	decodeJSON(p.t, get(p.t, p.ts, url), &r)

	if r.TailOffset < p.chainEnd {
		p.rotated = true
		return nil
	}
	p.chainEnd = r.TailOffset
	p.cursor = r.NextOffset
	return r.Lines
}

// drain polls until a poll delivers nothing, returning the "n" field of every
// line delivered, in the order they arrived.
func (p *tailPoller) drain(maxPolls int) []int {
	p.t.Helper()
	var got []int
	for i := 0; i < maxPolls; i++ {
		delivered := p.poll()
		if len(delivered) == 0 {
			break
		}
		if len(delivered) > p.batch {
			p.t.Fatalf("poll returned %d lines, past the %d cap", len(delivered), p.batch)
		}
		for _, l := range delivered {
			var rec struct {
				N int `json:"n"`
			}
			if err := json.Unmarshal([]byte(l), &rec); err != nil {
				p.t.Fatalf("unmarshal %q: %v", l, err)
			}
			got = append(got, rec.N)
		}
	}
	return got
}

// wantSequence fails unless got is exactly want — which catches a gap, a
// duplicate and a reordering in one assertion.
func wantSequence(t *testing.T, got, want []int) {
	t.Helper()
	if len(got) != len(want) {
		t.Fatalf("delivered %d lines, want %d", len(got), len(want))
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("position %d: delivered n=%d, want n=%d", i, got[i], want[i])
		}
	}
}

func TestPollTail_BurstLargerThanBatch_DrainsInOrder(t *testing.T) {
	// More lines arrive between two polls than one response can carry. The
	// cursor used to jump to the end of the chain, so everything past the first
	// batch was never fetched and never displayed.
	const (
		burst = 1000
		batch = 200 // app.js BATCH
	)

	var lines []string
	var want []int
	for i := 1; i <= burst; i++ {
		lines = append(lines, fmt.Sprintf(`{"n":%d}`, i))
		want = append(want, i)
	}

	ts := newTestServer(t, []Sink{makeSink(t, t.TempDir(), "main", lines)})
	defer ts.Close()

	p := &tailPoller{t: t, ts: ts, batch: batch}
	wantSequence(t, p.drain(burst), want)
}

func TestPollTail_BurstLargerThanBatch_CountsMatchingLines(t *testing.T) {
	// The cap counts lines that pass the filter, so a burst in which only every
	// other line matches still needs several polls to drain.
	const batch = 200

	var lines []string
	var want []int
	for i := 1; i <= 1200; i++ {
		if i%2 == 0 {
			lines = append(lines, fmt.Sprintf(`{"n":%d,"message":"keep"}`, i))
			want = append(want, i)
		} else {
			lines = append(lines, fmt.Sprintf(`{"n":%d,"message":"drop"}`, i))
		}
	}

	ts := newTestServer(t, []Sink{makeSink(t, t.TempDir(), "main", lines)})
	defer ts.Close()

	p := &tailPoller{t: t, ts: ts, batch: batch, params: "&inc=keep"}
	wantSequence(t, p.drain(len(want)), want)
}

func TestPollTail_RotationDetected_WhileCursorLagsBehind(t *testing.T) {
	// While a burst drains the cursor sits well behind the end of the chain, so
	// a rotation can leave the chain shorter than it was without leaving it
	// shorter than the cursor. Comparing tail_offset against the previous
	// response's tail_offset catches that; comparing it against the cursor,
	// which is what the cursor being the previous tail_offset used to amount
	// to, would not.
	const batch = 200

	dir := t.TempDir()
	activePath := filepath.Join(dir, "syslog.jsonl")
	sink := Sink{Name: "main", Path: activePath, MaxFiles: 1}

	writeN := func(from, to int) {
		t.Helper()
		fh, err := os.OpenFile(activePath, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o600)
		if err != nil {
			t.Fatalf("open active: %v", err)
		}
		defer fh.Close()
		for i := from; i <= to; i++ {
			if _, err := fmt.Fprintf(fh, "{\"n\":%d}\n", i); err != nil {
				t.Fatalf("write line %d: %v", i, err)
			}
		}
	}

	writeN(1, 1000)

	ts := newTestServer(t, []Sink{sink})
	defer ts.Close()

	p := &tailPoller{t: t, ts: ts, batch: batch}
	if got := len(p.poll()); got != batch {
		t.Fatalf("first poll: want %d lines, got %d", batch, got)
	}
	cursor, chainEndBefore := p.cursor, p.chainEnd

	// Two rotations with fresh traffic in between, so the generation holding
	// the burst falls off the single-file retention window.
	rotate(t, activePath, sink.MaxFiles)
	writeN(1001, 1400)
	rotate(t, activePath, sink.MaxFiles)

	fc, err := NewFileChain(sink)
	if err != nil {
		t.Fatalf("NewFileChain: %v", err)
	}
	if fc.TailOffset() >= chainEndBefore {
		t.Fatalf("setup: chain did not shrink (%d >= %d)", fc.TailOffset(), chainEndBefore)
	}
	if fc.TailOffset() <= cursor {
		t.Fatalf("setup: chain shrank past the cursor (%d <= %d), so the test would "+
			"pass even without the separate chain-end cursor", fc.TailOffset(), cursor)
	}

	p.poll()
	if !p.rotated {
		t.Error("tail_offset regressed but the poll did not reload")
	}
}

// A directory that keeps changing under every attempt is an error, not a chain
// built from a listing known to be stale.
//
// The retry above gives up eventually, and what it used to do then was return
// the chain anyway: a generation short, every offset after it moved, and
// nothing to say so. A client reading at those offsets gets the wrong lines —
// silently, which for a log viewer is the worst way to be wrong. The caller
// turns this error into a 500 and the page retries a second later.
func TestAChainIsNotBuiltFromADirectoryThatNeverSettles(t *testing.T) {
	dir := t.TempDir()
	active := filepath.Join(dir, "syslog.jsonl")
	sink := Sink{Name: "main", Path: active, MaxFiles: 5}
	// Two generations and an active file, so that a rotation adds a name and
	// the listing really does change — the case the check is there to catch.
	for i, path := range []string{rotatedPath(active, 2), rotatedPath(active, 1), active} {
		if err := os.WriteFile(path, []byte(fmt.Sprintf("{\"n\":%d}\n", i+1)), 0o600); err != nil {
			t.Fatal(err)
		}
	}

	// Each attempt sees the directory change: a rotation once the chain is at
	// max_files leaves the name set alone, which validates, so what is
	// simulated here is the listing itself moving — a generation appearing or
	// going, which is what a rotation does to a chain below max_files and what
	// the check exists to catch.
	builds := 0
	testHookAfterListing = func() {
		builds++
		stirred := filepath.Join(dir, fmt.Sprintf("syslog.%d.jsonl", builds+2))
		if err := os.WriteFile(stirred, []byte("{\"n\":0}\n"), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	t.Cleanup(func() { testHookAfterListing = nil })

	fc, err := NewFileChain(sink)
	if err == nil {
		t.Fatalf("a chain was returned from a directory that never settled: %+v", fc.files)
	}
	if fc != nil {
		t.Errorf("a chain was returned alongside the error: %+v", fc.files)
	}
	if builds != chainBuildAttempts {
		t.Errorf("the directory was disturbed %d times, want %d — one per attempt",
			builds, chainBuildAttempts)
	}
	if !strings.Contains(err.Error(), "main") {
		t.Errorf("error %q does not name the sink", err)
	}
}
