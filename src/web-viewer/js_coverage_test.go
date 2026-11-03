// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

package main

// Coverage of the page's own JavaScript, as the browser tests exercise it.
// Go's coverage cannot see app.js, which runs only in the browser, so each
// browser test asks Chrome's V8 profiler for precise block coverage of the
// page and adds it up here. With MINILOG_JS_COVERAGE set to a file name, the
// total is written there as lcov when the tests finish, for Codecov beside the
// Go and C++ reports. Unset, nothing is recorded.

import (
	"context"
	"fmt"
	"os"
	"sort"
	"strings"
	"sync"
	"testing"
	"unicode/utf16"

	"github.com/chromedp/cdproto/cdp"
	"github.com/chromedp/cdproto/profiler"
	"github.com/chromedp/chromedp"
)

const (
	// The script whose coverage is kept, as the page loads it and as the
	// repository holds it.
	jsCoverageURL    = "/assets/app.js"
	jsCoverageSource = "assets/app.js"
	jsCoverageReport = "src/web-viewer/assets/app.js"
)

// jsHits is the execution count of each line of app.js, summed over every
// browser test, by 1-based line number. Only lines that hold code are keys.
var (
	jsHitsMu sync.Mutex
	jsHits   map[int]int64
)

func TestMain(m *testing.M) {
	code := m.Run()
	if path := os.Getenv("MINILOG_JS_COVERAGE"); path != "" {
		if err := writeJSCoverage(path); err != nil {
			fmt.Fprintln(os.Stderr, "JavaScript coverage:", err)
			code = 1
		}
	}
	os.Exit(code)
}

// startJSCoverage starts recording, before the page is loaded, and arranges
// for what was recorded to be added to jsHits when the test ends.
func startJSCoverage(t *testing.T, ctx context.Context) {
	t.Helper()
	if os.Getenv("MINILOG_JS_COVERAGE") == "" {
		return
	}
	yes := true
	err := chromedp.Do(ctx, chromedp.Func(func(ctx context.Context, tg *chromedp.Target) error {
		if _, err := cdp.Call(ctx, tg, profiler.Enable, cdp.Empty{}); err != nil {
			return err
		}
		_, err := cdp.Call(ctx, tg, profiler.StartPreciseCoverage,
			profiler.StartPreciseCoverageParams{CallCount: &yes, Detailed: &yes})
		return err
	}))
	if err != nil {
		t.Fatalf("starting JavaScript coverage: %v", err)
	}

	// Registered after the browser's own cleanup, so it runs before the
	// browser is stopped.
	t.Cleanup(func() {
		res, err := chromedp.Run(ctx, func(ctx context.Context, tg *chromedp.Target) (profiler.TakePreciseCoverageResult, error) {
			return cdp.Call(ctx, tg, profiler.TakePreciseCoverage, cdp.Empty{})
		})
		if err != nil {
			// A test that failed by running out of time has no browser left
			// to ask; its failure is the one to report.
			t.Logf("taking JavaScript coverage: %v", err)
			return
		}
		if err := addJSCoverage(res.Result); err != nil {
			t.Errorf("JavaScript coverage: %v", err)
		}
	})
}

// addJSCoverage adds the line counts of app.js in result to jsHits.
func addJSCoverage(result []*profiler.ScriptCoverage) error {
	src, err := os.ReadFile(jsCoverageSource)
	if err != nil {
		return err
	}
	lines := codeLines(string(src))

	jsHitsMu.Lock()
	defer jsHitsMu.Unlock()
	if jsHits == nil {
		jsHits = map[int]int64{}
		for _, l := range lines {
			jsHits[l.number] = 0
		}
	}
	for _, script := range result {
		if !strings.HasSuffix(script.URL, jsCoverageURL) {
			continue
		}
		var ranges []*profiler.CoverageRange
		for _, f := range script.Functions {
			ranges = append(ranges, f.Ranges...)
		}
		for _, l := range lines {
			jsHits[l.number] += lineCount(ranges, l)
		}
	}
	return nil
}

// codeLine is one line of the script that holds code: its number, and where
// its code starts and ends in the offsets V8 reports, which count UTF-16 code
// units rather than bytes.
type codeLine struct {
	number     int
	start, end int64
}

// codeLines returns the lines of src that hold code, leaving out blank lines
// and comments.
func codeLines(src string) []codeLine {
	var out []codeLine
	var offset int64 // in UTF-16 code units
	inBlockComment := false
	for i, line := range strings.SplitAfter(src, "\n") {
		width := utf16Len(line)
		trimmed := strings.TrimSpace(line)
		lead := utf16Len(line[:len(line)-len(strings.TrimLeft(line, " \t"))])
		switch {
		case inBlockComment:
			inBlockComment = !strings.Contains(trimmed, "*/")
		case strings.HasPrefix(trimmed, "/*"):
			inBlockComment = !strings.Contains(trimmed, "*/")
		case trimmed == "" || strings.HasPrefix(trimmed, "//"):
		default:
			start := offset + lead
			end := start + utf16Len(trimmed)
			out = append(out, codeLine{number: i + 1, start: start, end: end})
		}
		offset += width
	}
	return out
}

func utf16Len(s string) int64 { return int64(len(utf16.Encode([]rune(s)))) }

// lineCount is how often a line ran: the count of the innermost range that
// takes in all of its code. V8 reports nested ranges, a function's and then
// the blocks inside it, each with the count that holds inside it unless a
// range nested in it says otherwise. A block that starts or ends partway
// through a line, as in `if (x) return;`, does not decide that line's count;
// this is how Node's own coverage tools map V8's ranges onto lines.
func lineCount(ranges []*profiler.CoverageRange, l codeLine) int64 {
	var best *profiler.CoverageRange
	for _, r := range ranges {
		if r.StartOffset <= l.start && r.EndOffset >= l.end &&
			(best == nil || r.EndOffset-r.StartOffset < best.EndOffset-best.StartOffset) {
			best = r
		}
	}
	if best == nil {
		return 0
	}
	return best.Count
}

// writeJSCoverage writes jsHits as an lcov report.
func writeJSCoverage(path string) error {
	jsHitsMu.Lock()
	defer jsHitsMu.Unlock()
	if jsHits == nil {
		return fmt.Errorf("no browser test recorded any, so no report is written to %s", path)
	}
	numbers := make([]int, 0, len(jsHits))
	for n := range jsHits {
		numbers = append(numbers, n)
	}
	sort.Ints(numbers)

	var b strings.Builder
	fmt.Fprintf(&b, "TN:\nSF:%s\n", jsCoverageReport)
	hit := 0
	for _, n := range numbers {
		fmt.Fprintf(&b, "DA:%d,%d\n", n, jsHits[n])
		if jsHits[n] > 0 {
			hit++
		}
	}
	fmt.Fprintf(&b, "LF:%d\nLH:%d\nend_of_record\n", len(numbers), hit)
	fmt.Fprintf(os.Stderr, "JavaScript coverage of %s: %d of %d lines, %.1f%%\n",
		jsCoverageReport, hit, len(numbers), 100*float64(hit)/float64(len(numbers)))
	return os.WriteFile(path, []byte(b.String()), 0o644)
}
