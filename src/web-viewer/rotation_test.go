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

		var tailOffset int64
		deadline := time.Now().Add(60 * time.Second)
		writerFinished := false
		drainPasses := 0

		for time.Now().Before(deadline) {
			r, err := doGet(fmt.Sprintf("/lines?sink=main&offset=%d&dir=forward&count=200", tailOffset))
			if err != nil {
				time.Sleep(pollInterval)
				continue
			}

			if r.TailOffset < tailOffset {
				// Rotation shrank the chain: mirror loadTail().
				// Read the last batch to pick up lines written just before rotation.
				tailOffset = r.TailOffset
				rt, err := doGet("/lines?sink=main&tail=true&count=200")
				if err == nil {
					for _, line := range rt.Lines {
						var rec struct {
							N int `json:"n"`
						}
						if json.Unmarshal([]byte(line), &rec) == nil {
							seenN[rec.N] = true
						}
					}
					tailOffset = rt.TailOffset
				}
				continue
			}

			tailOffset = r.TailOffset
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
			t.Errorf("concurrent poll MISSING %d lines present in final chain (first %d): %v",
				len(missing), len(show), show)
		} else {
			t.Logf("OK: all %d final-chain lines received by poller", len(inFinalChain))
		}
	})
}
