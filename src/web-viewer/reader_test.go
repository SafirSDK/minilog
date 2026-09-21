// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// ── helpers ───────────────────────────────────────────────────────────────────

// makeLine returns a JSONL line containing the given message and severity/facility values.
func makeLine(msg, severity, facility string) string {
	return fmt.Sprintf(`{"severity":"%s","facility":"%s","message":"%s"}`, severity, facility, msg)
}

// writeLines writes lines (each followed by \n) to path and returns the path.
func writeLines(t *testing.T, path string, lines []string) {
	t.Helper()
	var sb strings.Builder
	for _, l := range lines {
		sb.WriteString(l)
		sb.WriteByte('\n')
	}
	if err := os.WriteFile(path, []byte(sb.String()), 0o600); err != nil {
		t.Fatalf("writeLines: %v", err)
	}
}

// chainFromFiles builds a FileChain from a slice of existing file paths (oldest first).
// Bypasses the naming convention of NewFileChain, building the chain directly.
func chainFromFiles(t *testing.T, paths []string) *FileChain {
	t.Helper()
	if len(paths) == 0 {
		t.Fatal("chainFromFiles: no paths")
	}
	fc := &FileChain{}
	var offset int64
	for _, p := range paths {
		info, err := os.Stat(p)
		if err != nil || info.Size() == 0 {
			continue
		}
		fc.files = append(fc.files, chainFile{path: p, start: offset, size: info.Size()})
		offset += info.Size()
	}
	fc.total = offset
	return fc
}

// noFilter returns a filter that accepts everything.
func noFilter() *Filter { return &Filter{} }

// lineTexts extracts the string content from a slice of raw line bytes.
func lineTexts(lines [][]byte) []string {
	out := make([]string, len(lines))
	for i, l := range lines {
		out[i] = string(l)
	}
	return out
}

// ── Filter.Match ──────────────────────────────────────────────────────────────

func TestFilter_NoConditions_AcceptsAll(t *testing.T) {
	f := &Filter{}
	line := []byte(`{"severity":"info","facility":"daemon","message":"hello"}`)
	if !f.Match(line) {
		t.Error("empty filter should accept all lines")
	}
}

func TestFilter_Exclude_RejectsMatchingLine(t *testing.T) {
	f := &Filter{Exclude: []string{"debug"}}
	if f.Match([]byte(`{"message":"debug output"}`)) {
		t.Error("line containing exclude pattern should be rejected")
	}
}

func TestFilter_Exclude_CaseInsensitive(t *testing.T) {
	f := &Filter{Exclude: []string{"debug"}}
	if f.Match([]byte(`{"message":"DEBUG output"}`)) {
		t.Error("exclude match should be case-insensitive")
	}
}

func TestFilter_Exclude_NonMatchingAccepted(t *testing.T) {
	f := &Filter{Exclude: []string{"debug"}}
	if !f.Match([]byte(`{"message":"info message"}`)) {
		t.Error("line not containing exclude pattern should be accepted")
	}
}

func TestFilter_Include_RequiresMatch(t *testing.T) {
	f := &Filter{Include: []string{"error"}}
	if f.Match([]byte(`{"message":"info message"}`)) {
		t.Error("line not containing include pattern should be rejected")
	}
	if !f.Match([]byte(`{"message":"an error occurred"}`)) {
		t.Error("line containing include pattern should be accepted")
	}
}

func TestFilter_Include_AnyPatternSuffices(t *testing.T) {
	f := &Filter{Include: []string{"error", "warn"}}
	if !f.Match([]byte(`{"message":"warning issued"}`)) {
		t.Error("line matching any include pattern should be accepted")
	}
}

func TestFilter_ExcludeTakesPrecedenceOverInclude(t *testing.T) {
	f := &Filter{Include: []string{"error"}, Exclude: []string{"test"}}
	// Matches include, but also matches exclude → should be rejected.
	if f.Match([]byte(`{"message":"test error"}`)) {
		t.Error("exclude should take precedence over include")
	}
}

func TestFilter_Severity_Allowlist(t *testing.T) {
	f := &Filter{Severities: []string{"error", "warning"}}
	if !f.Match([]byte(`{"severity":"error","message":"err"}`)) {
		t.Error("severity 3 should be accepted")
	}
	if !f.Match([]byte(`{"severity":"warning","message":"warn"}`)) {
		t.Error("severity 4 should be accepted")
	}
	if f.Match([]byte(`{"severity":"info","message":"info"}`)) {
		t.Error("severity 6 should be rejected")
	}
}

func TestFilter_Severity_SpacedJSON(t *testing.T) {
	f := &Filter{Severities: []string{"info"}}
	if !f.Match([]byte(`{"severity": "info", "message":"hello"}`)) {
		t.Error("spaced JSON severity field should be matched")
	}
}

func TestFilter_Severity_NullRejected(t *testing.T) {
	f := &Filter{Severities: []string{"info"}}
	if f.Match([]byte(`{"severity":null,"message":"info"}`)) {
		t.Error("null severity should be rejected by allowlist")
	}
}

func TestFilter_Facility_Allowlist(t *testing.T) {
	f := &Filter{Facilities: []string{"auth"}} // auth
	if !f.Match([]byte(`{"facility":"auth","severity":"info","message":"login"}`)) {
		t.Error("facility 4 should be accepted")
	}
	if f.Match([]byte(`{"facility":"daemon","severity":"info","message":"daemon"}`)) {
		t.Error("facility 3 should be rejected")
	}
}

func TestFilter_MultipleConditions_AllMustPass(t *testing.T) {
	f := &Filter{
		Severities: []string{"info"},
		Include:    []string{"nginx"},
		Exclude:    []string{"debug"},
	}
	// All pass.
	if !f.Match([]byte(`{"severity":"info","message":"nginx: request"}`)) {
		t.Error("should accept line matching all conditions")
	}
	// Severity fails.
	if f.Match([]byte(`{"severity":"debug","message":"nginx: request"}`)) {
		t.Error("should reject line with wrong severity")
	}
	// Include fails.
	if f.Match([]byte(`{"severity":"info","message":"postgres: query"}`)) {
		t.Error("should reject line not matching include")
	}
	// Exclude fires.
	if f.Match([]byte(`{"severity":"info","message":"nginx: debug request"}`)) {
		t.Error("should reject line matching exclude")
	}
}

// ── FileChain / ReadForward ───────────────────────────────────────────────────

func TestReadForward_SingleFile_AllLines(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	lines := []string{
		makeLine("one", "info", "daemon"),
		makeLine("two", "info", "daemon"),
		makeLine("three", "info", "daemon"),
	}
	writeLines(t, p, lines)

	fc := chainFromFiles(t, []string{p})
	got, _, _, _, err := fc.ReadForward(0, 100, noFilter())
	if err != nil {
		t.Fatalf("ReadForward error: %v", err)
	}
	if len(got) != 3 {
		t.Fatalf("want 3 lines, got %d", len(got))
	}
	for i, want := range lines {
		if string(got[i]) != want {
			t.Errorf("line %d: want %q, got %q", i, want, string(got[i]))
		}
	}
}

func TestReadForward_CountLimit(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	writeLines(t, p, []string{
		makeLine("one", "info", "daemon"),
		makeLine("two", "info", "daemon"),
		makeLine("three", "info", "daemon"),
	})

	fc := chainFromFiles(t, []string{p})
	got, _, _, _, err := fc.ReadForward(0, 2, noFilter())
	if err != nil {
		t.Fatalf("ReadForward error: %v", err)
	}
	if len(got) != 2 {
		t.Errorf("want 2 lines, got %d", len(got))
	}
}

func TestReadForward_Filter_SkipsNonMatching(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	writeLines(t, p, []string{
		makeLine("one", "info", "daemon"),
		makeLine("two", "error", "daemon"), // severity 3 — filtered out
		makeLine("three", "info", "daemon"),
	})

	fc := chainFromFiles(t, []string{p})
	f := &Filter{Severities: []string{"info"}}
	got, _, _, _, err := fc.ReadForward(0, 100, f)
	if err != nil {
		t.Fatalf("ReadForward error: %v", err)
	}
	if len(got) != 2 {
		t.Fatalf("want 2 lines, got %d", len(got))
	}
	if !strings.Contains(string(got[0]), "one") || !strings.Contains(string(got[1]), "three") {
		t.Errorf("unexpected lines: %v", lineTexts(got))
	}
}

func TestReadForward_Offsets_Correct(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	l0 := makeLine("first", "info", "daemon")
	l1 := makeLine("second", "info", "daemon")
	writeLines(t, p, []string{l0, l1})

	fc := chainFromFiles(t, []string{p})
	_, offsets, firstOff, nextOff, err := fc.ReadForward(0, 100, noFilter())
	if err != nil {
		t.Fatalf("ReadForward error: %v", err)
	}
	if firstOff != 0 {
		t.Errorf("firstOffset: want 0, got %d", firstOff)
	}
	if offsets[0] != 0 {
		t.Errorf("offset[0]: want 0, got %d", offsets[0])
	}
	expectedOff1 := int64(len(l0) + 1) // +1 for newline
	if offsets[1] != expectedOff1 {
		t.Errorf("offset[1]: want %d, got %d", expectedOff1, offsets[1])
	}
	expectedNext := int64(len(l0)+1) + int64(len(l1)+1)
	if nextOff != expectedNext {
		t.Errorf("nextOffset: want %d, got %d", expectedNext, nextOff)
	}
}

func TestReadForward_NextOffset_AdvancesPastNonMatchingLines(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	l0 := makeLine("keep", "info", "daemon")
	l1 := makeLine("skip", "error", "daemon") // filtered
	writeLines(t, p, []string{l0, l1})

	fc := chainFromFiles(t, []string{p})
	f := &Filter{Severities: []string{"info"}}
	_, _, _, nextOff, err := fc.ReadForward(0, 100, f)
	if err != nil {
		t.Fatalf("ReadForward error: %v", err)
	}
	// nextOffset must be past the skipped line too, not just the last returned line.
	expected := int64(len(l0)+1) + int64(len(l1)+1)
	if nextOff != expected {
		t.Errorf("nextOffset: want %d (past skipped line), got %d", expected, nextOff)
	}
}

func TestReadForward_MultiFile_CrossesBoundary(t *testing.T) {
	dir := t.TempDir()
	p0 := filepath.Join(dir, "old.jsonl")
	p1 := filepath.Join(dir, "new.jsonl")
	writeLines(t, p0, []string{makeLine("old1", "info", "daemon"), makeLine("old2", "info", "daemon")})
	writeLines(t, p1, []string{makeLine("new1", "info", "daemon"), makeLine("new2", "info", "daemon")})

	fc := chainFromFiles(t, []string{p0, p1})
	got, _, _, _, err := fc.ReadForward(0, 100, noFilter())
	if err != nil {
		t.Fatalf("ReadForward error: %v", err)
	}
	if len(got) != 4 {
		t.Fatalf("want 4 lines, got %d: %v", len(got), lineTexts(got))
	}
	if !strings.Contains(string(got[0]), "old1") || !strings.Contains(string(got[3]), "new2") {
		t.Errorf("wrong order: %v", lineTexts(got))
	}
}

func TestReadForward_StartsFromOffset(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	l0 := makeLine("first", "info", "daemon")
	l1 := makeLine("second", "info", "daemon")
	writeLines(t, p, []string{l0, l1})

	fc := chainFromFiles(t, []string{p})
	// Start reading from the second line.
	off := int64(len(l0) + 1)
	got, _, _, _, err := fc.ReadForward(off, 100, noFilter())
	if err != nil {
		t.Fatalf("ReadForward error: %v", err)
	}
	if len(got) != 1 || !strings.Contains(string(got[0]), "second") {
		t.Errorf("want [second], got %v", lineTexts(got))
	}
}

func TestReadForward_EmptyChain(t *testing.T) {
	fc := &FileChain{}
	got, _, _, _, err := fc.ReadForward(0, 100, noFilter())
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(got) != 0 {
		t.Errorf("want no lines from empty chain, got %d", len(got))
	}
}

func TestReadForward_NoMatchingLines_OffsetUnchanged(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	writeLines(t, p, []string{makeLine("msg", "debug", "daemon")})

	fc := chainFromFiles(t, []string{p})
	f := &Filter{Severities: []string{"info"}} // nothing matches
	got, _, firstOff, nextOff, err := fc.ReadForward(0, 100, f)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(got) != 0 {
		t.Errorf("want 0 lines, got %d", len(got))
	}
	// When nothing matches, offsets should reflect we still scanned the file.
	_ = firstOff
	_ = nextOff
}

// ── ReadForward — additional coverage ────────────────────────────────────────

func TestReadForward_OffsetAtFileBoundary(t *testing.T) {
	// Starting exactly at the first byte of the second file should read only
	// lines from that file.
	dir := t.TempDir()
	p0 := filepath.Join(dir, "old.jsonl")
	p1 := filepath.Join(dir, "new.jsonl")
	writeLines(t, p0, []string{makeLine("old", "info", "daemon")})
	writeLines(t, p1, []string{makeLine("new", "info", "daemon")})

	fc := chainFromFiles(t, []string{p0, p1})
	// Offset == size of first file == start of second file.
	boundary := fc.files[0].size
	got, _, _, _, err := fc.ReadForward(boundary, 100, noFilter())
	if err != nil {
		t.Fatalf("ReadForward error: %v", err)
	}
	if len(got) != 1 || !strings.Contains(string(got[0]), "new") {
		t.Errorf("want [new], got %v", lineTexts(got))
	}
}

func TestReadForward_FirstOffset_SkipsFilteredLeadingLines(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	l0 := makeLine("skip", "error", "daemon") // filtered out
	l1 := makeLine("keep", "info", "daemon")
	writeLines(t, p, []string{l0, l1})

	fc := chainFromFiles(t, []string{p})
	f := &Filter{Severities: []string{"info"}}
	_, _, firstOff, _, err := fc.ReadForward(0, 100, f)
	if err != nil {
		t.Fatalf("ReadForward error: %v", err)
	}
	// firstOffset must point to l1, not to the filtered l0.
	expectedFirstOff := int64(len(l0) + 1)
	if firstOff != expectedFirstOff {
		t.Errorf("firstOffset: want %d (start of first matching line), got %d", expectedFirstOff, firstOff)
	}
}

func TestReadForward_NextOffset_EqualsEndOfChain(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	lines := []string{makeLine("a", "info", "daemon"), makeLine("b", "info", "daemon")}
	writeLines(t, p, lines)

	fc := chainFromFiles(t, []string{p})
	_, _, _, nextOff, err := fc.ReadForward(0, 1000, noFilter())
	if err != nil {
		t.Fatalf("ReadForward error: %v", err)
	}
	if nextOff != fc.TailOffset() {
		t.Errorf("nextOffset: want TailOffset=%d, got %d", fc.TailOffset(), nextOff)
	}
}

func TestReadForward_CountZero_ReturnsNothing(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	writeLines(t, p, []string{makeLine("msg", "info", "daemon")})

	fc := chainFromFiles(t, []string{p})
	got, _, _, _, err := fc.ReadForward(0, 0, noFilter())
	if err != nil {
		t.Fatalf("ReadForward error: %v", err)
	}
	if len(got) != 0 {
		t.Errorf("want 0 lines for count=0, got %d", len(got))
	}
}

func TestReadForward_VeryLongLine(t *testing.T) {
	// A line longer than the default 64 KB scanner buffer must still be read
	// intact when the buffer override (1 MB) is in effect.
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")

	// Build a message that is ~100 KB — well over the 64 KB default buffer.
	bigMsg := strings.Repeat("x", 100*1024)
	line := `{"severity":"info","facility":"daemon","message":"` + bigMsg + `"}`
	writeLines(t, p, []string{line})

	fc := chainFromFiles(t, []string{p})
	got, _, _, _, err := fc.ReadForward(0, 10, noFilter())
	if err != nil {
		t.Fatalf("ReadForward error: %v", err)
	}
	if len(got) != 1 {
		t.Fatalf("want 1 line, got %d", len(got))
	}
	if len(got[0]) != len(line) {
		t.Errorf("line length: want %d, got %d", len(line), len(got[0]))
	}
}

func TestReadForward_BlankLinesSkipped(t *testing.T) {
	// Blank lines interspersed in the file must not appear in results and must
	// not corrupt the offset accounting.
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	// Write raw content with blank lines.
	content := makeLine("first", "info", "daemon") + "\n" +
		"\n" + // blank
		makeLine("second", "info", "daemon") + "\n"
	if err := os.WriteFile(p, []byte(content), 0o600); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}

	fc := chainFromFiles(t, []string{p})
	got, offsets, _, _, err := fc.ReadForward(0, 100, noFilter())
	if err != nil {
		t.Fatalf("ReadForward error: %v", err)
	}
	if len(got) != 2 {
		t.Fatalf("want 2 lines (blank skipped), got %d: %v", len(got), lineTexts(got))
	}
	// The second line's offset must skip over the blank line byte.
	l0Len := int64(len(makeLine("first", "info", "daemon")) + 1) // +1 for \n
	blankLen := int64(1)                                         // the blank \n
	expectedOff1 := l0Len + blankLen
	if offsets[1] != expectedOff1 {
		t.Errorf("offset[1]: want %d (past blank), got %d", expectedOff1, offsets[1])
	}
}

// ── ReadBackward ──────────────────────────────────────────────────────────────

func TestReadBackward_SingleFile_LastN(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	lines := []string{
		makeLine("one", "info", "daemon"),
		makeLine("two", "info", "daemon"),
		makeLine("three", "info", "daemon"),
		makeLine("four", "info", "daemon"),
		makeLine("five", "info", "daemon"),
	}
	writeLines(t, p, lines)
	fc := chainFromFiles(t, []string{p})

	got, _, _, _, err := fc.ReadBackward(fc.TailOffset(), 3, noFilter(), -1)
	if err != nil {
		t.Fatalf("ReadBackward error: %v", err)
	}
	if len(got) != 3 {
		t.Fatalf("want 3 lines, got %d: %v", len(got), lineTexts(got))
	}
	// Results should be in forward (oldest-first) order.
	if !strings.Contains(string(got[0]), "three") {
		t.Errorf("want oldest=three, got %q", string(got[0]))
	}
	if !strings.Contains(string(got[2]), "five") {
		t.Errorf("want newest=five, got %q", string(got[2]))
	}
}

func TestReadBackward_ReturnsOldestFirst(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	writeLines(t, p, []string{
		makeLine("a", "info", "daemon"),
		makeLine("b", "info", "daemon"),
		makeLine("c", "info", "daemon"),
	})
	fc := chainFromFiles(t, []string{p})

	got, _, _, _, err := fc.ReadBackward(fc.TailOffset(), 10, noFilter(), -1)
	if err != nil {
		t.Fatalf("ReadBackward error: %v", err)
	}
	texts := lineTexts(got)
	for i := 1; i < len(texts); i++ {
		// Each line should come after the previous one in the file.
		if texts[i] <= texts[i-1] {
			t.Errorf("lines not in forward order at position %d: %v", i, texts)
		}
	}
}

func TestReadBackward_NextOffsetEqualsInput(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	writeLines(t, p, []string{makeLine("x", "info", "daemon")})
	fc := chainFromFiles(t, []string{p})

	tail := fc.TailOffset()
	_, _, _, nextOff, err := fc.ReadBackward(tail, 10, noFilter(), -1)
	if err != nil {
		t.Fatalf("ReadBackward error: %v", err)
	}
	if nextOff != tail {
		t.Errorf("nextOffset: want %d (input), got %d", tail, nextOff)
	}
}

func TestReadBackward_CountLargerThanAvailable_ReturnsAll(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	writeLines(t, p, []string{
		makeLine("a", "info", "daemon"),
		makeLine("b", "info", "daemon"),
	})
	fc := chainFromFiles(t, []string{p})

	got, _, _, _, err := fc.ReadBackward(fc.TailOffset(), 100, noFilter(), -1)
	if err != nil {
		t.Fatalf("ReadBackward error: %v", err)
	}
	if len(got) != 2 {
		t.Errorf("want 2 lines, got %d", len(got))
	}
}

func TestReadBackward_MultiFile_CrossesBoundary(t *testing.T) {
	dir := t.TempDir()
	p0 := filepath.Join(dir, "old.jsonl")
	p1 := filepath.Join(dir, "new.jsonl")
	writeLines(t, p0, []string{makeLine("old1", "info", "daemon"), makeLine("old2", "info", "daemon")})
	writeLines(t, p1, []string{makeLine("new1", "info", "daemon"), makeLine("new2", "info", "daemon")})

	fc := chainFromFiles(t, []string{p0, p1})
	got, _, _, _, err := fc.ReadBackward(fc.TailOffset(), 3, noFilter(), -1)
	if err != nil {
		t.Fatalf("ReadBackward error: %v", err)
	}
	if len(got) != 3 {
		t.Fatalf("want 3 lines, got %d: %v", len(got), lineTexts(got))
	}
	// Should span the file boundary: old2, new1, new2.
	if !strings.Contains(string(got[0]), "old2") {
		t.Errorf("want first=old2, got %q", string(got[0]))
	}
	if !strings.Contains(string(got[2]), "new2") {
		t.Errorf("want last=new2, got %q", string(got[2]))
	}
}

func TestReadBackward_Filter_Applied(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	writeLines(t, p, []string{
		makeLine("a", "info", "daemon"),
		makeLine("b", "error", "daemon"), // filtered
		makeLine("c", "info", "daemon"),
	})
	fc := chainFromFiles(t, []string{p})
	f := &Filter{Severities: []string{"info"}}

	got, _, _, _, err := fc.ReadBackward(fc.TailOffset(), 10, f, -1)
	if err != nil {
		t.Fatalf("ReadBackward error: %v", err)
	}
	if len(got) != 2 {
		t.Fatalf("want 2 lines, got %d: %v", len(got), lineTexts(got))
	}
	if !strings.Contains(string(got[0]), "\"a\"") || !strings.Contains(string(got[1]), "\"c\"") {
		t.Errorf("unexpected lines: %v", lineTexts(got))
	}
}

func TestReadBackward_EmptyChain(t *testing.T) {
	fc := &FileChain{}
	got, _, _, _, err := fc.ReadBackward(0, 10, noFilter(), -1)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(got) != 0 {
		t.Errorf("want no lines from empty chain, got %d", len(got))
	}
}

// ── ReadBackward — additional coverage ───────────────────────────────────────

func TestReadBackward_OffsetZero_ReturnsNothing(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	writeLines(t, p, []string{makeLine("msg", "info", "daemon")})
	fc := chainFromFiles(t, []string{p})

	got, _, _, _, err := fc.ReadBackward(0, 10, noFilter(), -1)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(got) != 0 {
		t.Errorf("want 0 lines for offset=0, got %d", len(got))
	}
}

func TestReadBackward_CountZero_ReturnsNothing(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	writeLines(t, p, []string{makeLine("msg", "info", "daemon")})
	fc := chainFromFiles(t, []string{p})

	got, _, _, _, err := fc.ReadBackward(fc.TailOffset(), 0, noFilter(), -1)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(got) != 0 {
		t.Errorf("want 0 lines for count=0, got %d", len(got))
	}
}

func TestReadBackward_FirstOffset_IsOldestLine(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	l0 := makeLine("alpha", "info", "daemon")
	l1 := makeLine("beta", "info", "daemon")
	l2 := makeLine("gamma", "info", "daemon")
	writeLines(t, p, []string{l0, l1, l2})
	fc := chainFromFiles(t, []string{p})

	// Read the last 2 lines; firstOffset should point to l1, not l0.
	_, _, firstOff, _, err := fc.ReadBackward(fc.TailOffset(), 2, noFilter(), -1)
	if err != nil {
		t.Fatalf("ReadBackward error: %v", err)
	}
	expectedFirstOff := int64(len(l0) + 1) // l1 starts after l0+newline
	if firstOff != expectedFirstOff {
		t.Errorf("firstOffset: want %d (start of l1), got %d", expectedFirstOff, firstOff)
	}
}

func TestReadBackward_AllFiltered_ReturnsEmpty(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	writeLines(t, p, []string{
		makeLine("a", "debug", "daemon"),
		makeLine("b", "debug", "daemon"),
	})
	fc := chainFromFiles(t, []string{p})
	f := &Filter{Severities: []string{"info"}} // nothing matches sev 7

	got, _, firstOff, nextOff, err := fc.ReadBackward(fc.TailOffset(), 10, f, -1)
	if err != nil {
		t.Fatalf("ReadBackward error: %v", err)
	}
	if len(got) != 0 {
		t.Errorf("want 0 lines, got %d", len(got))
	}
	tail := fc.TailOffset()
	if firstOff != tail || nextOff != tail {
		t.Errorf("offsets: want firstOffset=%d nextOffset=%d, got %d %d",
			tail, tail, firstOff, nextOff)
	}
}

func TestReadBackward_OffsetAtFileBoundary(t *testing.T) {
	// When logicalOffset == start of file[1] (physOffset==0 for that file),
	// the implementation must step back into file[0] to find lines.
	dir := t.TempDir()
	p0 := filepath.Join(dir, "old.jsonl")
	p1 := filepath.Join(dir, "new.jsonl")
	writeLines(t, p0, []string{makeLine("old1", "info", "daemon"), makeLine("old2", "info", "daemon")})
	writeLines(t, p1, []string{makeLine("new1", "info", "daemon")})

	fc := chainFromFiles(t, []string{p0, p1})

	// Read backward from exactly the boundary between the two files.
	boundary := fc.files[0].size // == fc.files[1].start
	got, _, _, _, err := fc.ReadBackward(boundary, 10, noFilter(), -1)
	if err != nil {
		t.Fatalf("ReadBackward error: %v", err)
	}
	if len(got) != 2 {
		t.Fatalf("want 2 lines (both from old file), got %d: %v", len(got), lineTexts(got))
	}
	if !strings.Contains(string(got[0]), "old1") || !strings.Contains(string(got[1]), "old2") {
		t.Errorf("unexpected lines: %v", lineTexts(got))
	}
}

func TestReadBackward_CountClampsCorrectly(t *testing.T) {
	// Single-file test that forces the trim-to-count branch: 5 lines, request 3.
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	writeLines(t, p, []string{
		makeLine("one", "info", "daemon"),
		makeLine("two", "info", "daemon"),
		makeLine("three", "info", "daemon"),
		makeLine("four", "info", "daemon"),
		makeLine("five", "info", "daemon"),
	})
	fc := chainFromFiles(t, []string{p})

	got, _, _, _, err := fc.ReadBackward(fc.TailOffset(), 3, noFilter(), -1)
	if err != nil {
		t.Fatalf("ReadBackward error: %v", err)
	}
	if len(got) != 3 {
		t.Fatalf("want 3 lines, got %d", len(got))
	}
	// Results must be in forward (oldest-first) order.
	if !strings.Contains(string(got[0]), "three") {
		t.Errorf("want first=three, got %q", string(got[0]))
	}
	if !strings.Contains(string(got[2]), "five") {
		t.Errorf("want last=five, got %q", string(got[2]))
	}
}

// ── Search ────────────────────────────────────────────────────────────────────

func TestSearch_FindsMatches(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	writeLines(t, p, []string{
		makeLine("nginx: GET /api", "info", "daemon"),
		makeLine("postgres: query", "info", "daemon"),
		makeLine("nginx: POST /login", "info", "daemon"),
	})
	fc := chainFromFiles(t, []string{p})

	results, total, err := fc.Search("nginx", 100, noFilter(), -1)
	if err != nil {
		t.Fatalf("Search error: %v", err)
	}
	if total != 2 {
		t.Errorf("totalMatches: want 2, got %d", total)
	}
	if len(results) != 2 {
		t.Fatalf("results: want 2, got %d", len(results))
	}
}

func TestSearch_CaseInsensitive(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	writeLines(t, p, []string{
		makeLine("NGINX error", "info", "daemon"),
		makeLine("nginx warning", "info", "daemon"),
	})
	fc := chainFromFiles(t, []string{p})

	_, total, err := fc.Search("nginx", 100, noFilter(), -1)
	if err != nil {
		t.Fatalf("Search error: %v", err)
	}
	if total != 2 {
		t.Errorf("totalMatches: want 2, got %d", total)
	}
}

func TestSearch_TotalMatchesExceedsLimit(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	var lines []string
	for i := 0; i < 10; i++ {
		lines = append(lines, makeLine(fmt.Sprintf("target line %d", i), "info", "daemon"))
	}
	writeLines(t, p, lines)
	fc := chainFromFiles(t, []string{p})

	results, total, err := fc.Search("target", 3, noFilter(), -1)
	if err != nil {
		t.Fatalf("Search error: %v", err)
	}
	if total != 10 {
		t.Errorf("totalMatches: want 10, got %d", total)
	}
	if len(results) != 3 {
		t.Errorf("results capped: want 3, got %d", len(results))
	}
}

func TestSearch_FilterCombinedWithQuery(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	writeLines(t, p, []string{
		makeLine("target sev6", "info", "daemon"),
		makeLine("target sev3", "error", "daemon"), // query matches but filter rejects
	})
	fc := chainFromFiles(t, []string{p})
	f := &Filter{Severities: []string{"info"}}

	_, total, err := fc.Search("target", 100, f, -1)
	if err != nil {
		t.Fatalf("Search error: %v", err)
	}
	if total != 1 {
		t.Errorf("totalMatches: want 1, got %d", total)
	}
}

func TestSearch_NoMatches(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	writeLines(t, p, []string{makeLine("hello world", "info", "daemon")})
	fc := chainFromFiles(t, []string{p})

	results, total, err := fc.Search("notpresent", 100, noFilter(), -1)
	if err != nil {
		t.Fatalf("Search error: %v", err)
	}
	if total != 0 || len(results) != 0 {
		t.Errorf("want no matches, got total=%d results=%d", total, len(results))
	}
}

func TestSearch_MultiFile_CrossesBoundary(t *testing.T) {
	dir := t.TempDir()
	p0 := filepath.Join(dir, "old.jsonl")
	p1 := filepath.Join(dir, "new.jsonl")
	writeLines(t, p0, []string{makeLine("target in old", "info", "daemon")})
	writeLines(t, p1, []string{makeLine("target in new", "info", "daemon")})

	fc := chainFromFiles(t, []string{p0, p1})
	_, total, err := fc.Search("target", 100, noFilter(), -1)
	if err != nil {
		t.Fatalf("Search error: %v", err)
	}
	if total != 2 {
		t.Errorf("totalMatches: want 2, got %d", total)
	}
}

// ── Search — additional coverage ──────────────────────────────────────────────

func TestSearch_EmptyQuery_MatchesAllLines(t *testing.T) {
	// bytes.Contains(lower, []byte("")) is always true, so an empty query
	// acts as "match everything" (subject to the filter).
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	writeLines(t, p, []string{
		makeLine("alpha", "info", "daemon"),
		makeLine("beta", "info", "daemon"),
		makeLine("gamma", "info", "daemon"),
	})
	fc := chainFromFiles(t, []string{p})

	_, total, err := fc.Search("", 100, noFilter(), -1)
	if err != nil {
		t.Fatalf("Search error: %v", err)
	}
	if total != 3 {
		t.Errorf("empty query: want totalMatches=3, got %d", total)
	}
}

func TestSearch_FirstResult_AbsoluteOffsetIsZero(t *testing.T) {
	// For a single-line file the first (and only) result must have offset 0.
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	writeLines(t, p, []string{makeLine("target", "info", "daemon")})
	fc := chainFromFiles(t, []string{p})

	results, _, err := fc.Search("target", 100, noFilter(), -1)
	if err != nil {
		t.Fatalf("Search error: %v", err)
	}
	if len(results) != 1 {
		t.Fatalf("want 1 result, got %d", len(results))
	}
	if results[0].Offset != 0 {
		t.Errorf("first result offset: want 0, got %d", results[0].Offset)
	}
}

func TestSearch_LimitZero_NoResultsButCountIsCorrect(t *testing.T) {
	// limit=0 means len(results) < 0 is always false, so results stays empty
	// while totalMatches is still fully counted.
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	writeLines(t, p, []string{
		makeLine("target 1", "info", "daemon"),
		makeLine("target 2", "info", "daemon"),
	})
	fc := chainFromFiles(t, []string{p})

	results, total, err := fc.Search("target", 0, noFilter(), -1)
	if err != nil {
		t.Fatalf("Search error: %v", err)
	}
	if len(results) != 0 {
		t.Errorf("want 0 results for limit=0, got %d", len(results))
	}
	if total != 2 {
		t.Errorf("totalMatches: want 2, got %d", total)
	}
}

func TestSearch_OffsetOrdering(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	writeLines(t, p, []string{
		makeLine("target first", "info", "daemon"),
		makeLine("other", "info", "daemon"),
		makeLine("target second", "info", "daemon"),
	})
	fc := chainFromFiles(t, []string{p})

	results, _, err := fc.Search("target", 100, noFilter(), -1)
	if err != nil {
		t.Fatalf("Search error: %v", err)
	}
	if len(results) != 2 {
		t.Fatalf("want 2 results, got %d", len(results))
	}
	if results[0].Offset >= results[1].Offset {
		t.Errorf("results not in offset order: %d >= %d", results[0].Offset, results[1].Offset)
	}
}

// ── Filter — additional coverage ─────────────────────────────────────────────

func TestFilter_Include_CaseInsensitive(t *testing.T) {
	f := &Filter{Include: []string{"error"}}
	if !f.Match([]byte(`{"message":"ERROR occurred"}`)) {
		t.Error("include match should be case-insensitive")
	}
}

func TestFilter_Exclude_SecondPatternFires(t *testing.T) {
	f := &Filter{Exclude: []string{"alpha", "beta"}}
	// Only the second pattern matches — should still be rejected.
	if f.Match([]byte(`{"message":"beta trouble"}`)) {
		t.Error("second exclude pattern should also reject the line")
	}
}

func TestFilter_Facility_MultipleAllowed(t *testing.T) {
	f := &Filter{Facilities: []string{"auth", "local0"}} // auth, local0
	if !f.Match([]byte(`{"facility":"auth","severity":"info","message":"ok"}`)) {
		t.Error("facility 4 should be accepted")
	}
	if !f.Match([]byte(`{"facility":"local0","severity":"info","message":"ok"}`)) {
		t.Error("facility 16 should be accepted")
	}
	if f.Match([]byte(`{"facility":"daemon","severity":"info","message":"ok"}`)) {
		t.Error("facility 3 should be rejected")
	}
}

func TestFilter_Severity_FieldAbsent_Rejected(t *testing.T) {
	f := &Filter{Severities: []string{"info"}}
	// Line has no "severity" key at all.
	if f.Match([]byte(`{"facility":"daemon","message":"no severity field"}`)) {
		t.Error("line with absent severity field should be rejected by severity filter")
	}
}

func TestFilter_Facility_LongName(t *testing.T) {
	// Facility local7 — exercises string matching for longer facility names.
	f := &Filter{Facilities: []string{"local7"}}
	if !f.Match([]byte(`{"facility":"local7","severity":"info","message":"local7"}`)) {
		t.Error("facility 23 should be accepted")
	}
	if f.Match([]byte(`{"facility":"daemon","severity":"info","message":"nope"}`)) {
		t.Error("facility 3 should be rejected when allowlist is [23]")
	}
}

func TestFilter_SeverityAndFacility_BothMustPass(t *testing.T) {
	f := &Filter{Severities: []string{"info"}, Facilities: []string{"auth"}}
	// Both match.
	if !f.Match([]byte(`{"facility":"auth","severity":"info","message":"ok"}`)) {
		t.Error("line matching both severity and facility should be accepted")
	}
	// Severity passes but facility fails.
	if f.Match([]byte(`{"facility":"daemon","severity":"info","message":"nope"}`)) {
		t.Error("should reject when facility does not match")
	}
	// Facility passes but severity fails.
	if f.Match([]byte(`{"facility":"auth","severity":"debug","message":"nope"}`)) {
		t.Error("should reject when severity does not match")
	}
}

func TestFilter_EmptyLine_Rejected(t *testing.T) {
	// An empty byte slice contains no severity/facility fields, so any
	// non-empty allowlist must reject it; an empty filter accepts it.
	fAll := &Filter{}
	if !fAll.Match([]byte{}) {
		t.Error("empty filter should accept an empty line")
	}
	fSev := &Filter{Severities: []string{"info"}}
	if fSev.Match([]byte{}) {
		t.Error("severity filter should reject an empty line (no field present)")
	}
}

// ── NewFileChain ──────────────────────────────────────────────────────────────

func TestNewFileChain_SkipsEmptyFiles(t *testing.T) {
	dir := t.TempDir()
	active := filepath.Join(dir, "syslog.jsonl")
	empty := filepath.Join(dir, "syslog.1.jsonl")

	// Write content only to the active file; leave the rotated one empty.
	writeLines(t, active, []string{makeLine("msg", "info", "daemon")})
	if err := os.WriteFile(empty, []byte{}, 0o600); err != nil {
		t.Fatalf("create empty file: %v", err)
	}

	sink := Sink{Name: "main", Path: active, MaxFiles: 5}
	fc, err := NewFileChain(sink)
	if err != nil {
		t.Fatalf("NewFileChain: %v", err)
	}
	if len(fc.files) != 1 {
		t.Errorf("want 1 file (empty skipped), got %d", len(fc.files))
	}
}

func TestNewFileChain_DiscoversRotatedFiles(t *testing.T) {
	dir := t.TempDir()
	active := filepath.Join(dir, "syslog.jsonl")
	rot1 := filepath.Join(dir, "syslog.1.jsonl")
	rot2 := filepath.Join(dir, "syslog.2.jsonl")

	writeLines(t, active, []string{makeLine("active", "info", "daemon")})
	writeLines(t, rot1, []string{makeLine("rot1", "info", "daemon")})
	writeLines(t, rot2, []string{makeLine("rot2", "info", "daemon")})

	sink := Sink{Name: "main", Path: active, MaxFiles: 5}
	fc, err := NewFileChain(sink)
	if err != nil {
		t.Fatalf("NewFileChain: %v", err)
	}
	if len(fc.files) != 3 {
		t.Errorf("want 3 files, got %d", len(fc.files))
	}
	// Oldest first.
	if !strings.HasSuffix(fc.files[0].path, "syslog.2.jsonl") {
		t.Errorf("first file should be syslog.2.jsonl, got %s", fc.files[0].path)
	}
	if !strings.HasSuffix(fc.files[2].path, "syslog.jsonl") {
		t.Errorf("last file should be syslog.jsonl, got %s", fc.files[2].path)
	}
}

func TestNewFileChain_OffsetsContinuous(t *testing.T) {
	dir := t.TempDir()
	active := filepath.Join(dir, "syslog.jsonl")
	rot1 := filepath.Join(dir, "syslog.1.jsonl")

	writeLines(t, rot1, []string{makeLine("old", "info", "daemon")})
	writeLines(t, active, []string{makeLine("new", "info", "daemon")})

	rot1Size := fileSize(t, rot1)

	sink := Sink{Name: "main", Path: active, MaxFiles: 5}
	fc, err := NewFileChain(sink)
	if err != nil {
		t.Fatalf("NewFileChain: %v", err)
	}
	if fc.files[0].start != 0 {
		t.Errorf("first file start: want 0, got %d", fc.files[0].start)
	}
	if fc.files[1].start != rot1Size {
		t.Errorf("second file start: want %d, got %d", rot1Size, fc.files[1].start)
	}
}

func fileSize(t *testing.T, path string) int64 {
	t.Helper()
	info, err := os.Stat(path)
	if err != nil {
		t.Fatalf("fileSize: %v", err)
	}
	return info.Size()
}

// ── fileAt ────────────────────────────────────────────────────────────────────

func TestFileAt_OffsetPastEnd_ClampsToLastFile(t *testing.T) {
	// logicalOffset >= fc.total must return the last file index and its size.
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	writeLines(t, p, []string{makeLine("msg", "info", "daemon")})
	fc := chainFromFiles(t, []string{p})

	idx, phys := fc.fileAt(fc.total + 999)
	if idx != 0 {
		t.Errorf("fileIdx: want 0 (last), got %d", idx)
	}
	if phys != fc.files[0].size {
		t.Errorf("physOffset: want %d (file size), got %d", fc.files[0].size, phys)
	}
}

// ── matchStringField ──────────────────────────────────────────────────────────

func TestMatchStringField_NoClosingQuote_ReturnsFalse(t *testing.T) {
	// A JSON value whose opening quote is present but closing quote is absent
	// must not match — exercises the `end < 0` branch in matchStringField.
	f := &Filter{Severities: []string{"info"}}
	// Truncated JSON: "severity":"inf  (no closing quote)
	line := []byte(`{"severity":"inf`)
	if f.Match(line) {
		t.Error("truncated value with no closing quote should not match")
	}
}

// ── ReadForward — file disappears between chain build and read ────────────────

func TestReadForward_FileDeletedAfterChainBuild_SkipsGracefully(t *testing.T) {
	// Build the chain, then delete the file before reading.
	// ReadForward must skip the missing file and return no lines (not panic/error).
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	writeLines(t, p, []string{makeLine("msg", "info", "daemon")})
	fc := chainFromFiles(t, []string{p})

	if err := os.Remove(p); err != nil {
		t.Fatalf("remove: %v", err)
	}

	got, _, _, _, err := fc.ReadForward(0, 100, noFilter())
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(got) != 0 {
		t.Errorf("want 0 lines from deleted file, got %d", len(got))
	}
}

// ── ReadBackward — file disappears between chain build and read ───────────────

func TestReadBackward_FileDeletedAfterChainBuild_SkipsGracefully(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	writeLines(t, p, []string{makeLine("msg", "info", "daemon")})
	fc := chainFromFiles(t, []string{p})
	tail := fc.TailOffset()

	if err := os.Remove(p); err != nil {
		t.Fatalf("remove: %v", err)
	}

	got, _, _, _, err := fc.ReadBackward(tail, 10, noFilter(), -1)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(got) != 0 {
		t.Errorf("want 0 lines from deleted file, got %d", len(got))
	}
}

// ── ReadBackward — trim-to-count across file boundary ────────────────────────

func TestReadBackward_MultiFile_TrimToCount(t *testing.T) {
	// Two files each with 4 lines; request only 3.
	// The backward scan will collect lines from both files before trimming.
	dir := t.TempDir()
	p0 := filepath.Join(dir, "old.jsonl")
	p1 := filepath.Join(dir, "new.jsonl")
	writeLines(t, p0, []string{
		makeLine("old1", "info", "daemon"),
		makeLine("old2", "info", "daemon"),
		makeLine("old3", "info", "daemon"),
		makeLine("old4", "info", "daemon"),
	})
	writeLines(t, p1, []string{
		makeLine("new1", "info", "daemon"),
		makeLine("new2", "info", "daemon"),
		makeLine("new3", "info", "daemon"),
		makeLine("new4", "info", "daemon"),
	})

	fc := chainFromFiles(t, []string{p0, p1})
	got, _, _, _, err := fc.ReadBackward(fc.TailOffset(), 3, noFilter(), -1)
	if err != nil {
		t.Fatalf("ReadBackward error: %v", err)
	}
	if len(got) != 3 {
		t.Fatalf("want 3 lines, got %d: %v", len(got), lineTexts(got))
	}
	// Must be the last 3 lines in forward order: new2, new3, new4.
	if !strings.Contains(string(got[0]), "new2") {
		t.Errorf("want first=new2, got %q", string(got[0]))
	}
	if !strings.Contains(string(got[2]), "new4") {
		t.Errorf("want last=new4, got %q", string(got[2]))
	}
}

// ── Search — empty lines in file ─────────────────────────────────────────────

func TestSearch_EmptyLinesInFile_Skipped(t *testing.T) {
	// Blank lines (len(raw)==0) must be skipped by Search without counting as matches.
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	content := makeLine("target", "info", "daemon") + "\n" +
		"\n" + // blank line
		makeLine("target2", "info", "daemon") + "\n"
	if err := os.WriteFile(p, []byte(content), 0o600); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
	fc := chainFromFiles(t, []string{p})

	results, total, err := fc.Search("target", 100, noFilter(), -1)
	if err != nil {
		t.Fatalf("Search error: %v", err)
	}
	if total != 2 {
		t.Errorf("totalMatches: want 2 (blank skipped), got %d", total)
	}
	if len(results) != 2 {
		t.Errorf("results: want 2, got %d", len(results))
	}
}

// ── fileAt — additional coverage ─────────────────────────────────────────────

func TestFileAt_ExactlyAtTail_ClampsToEnd(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	writeLines(t, p, []string{makeLine("msg", "info", "daemon")})

	fc := chainFromFiles(t, []string{p})
	idx, phys := fc.fileAt(fc.TailOffset())
	if idx != 0 {
		t.Errorf("fileIdx: want 0, got %d", idx)
	}
	if phys != fc.files[0].size {
		t.Errorf("physOffset: want %d, got %d", fc.files[0].size, phys)
	}
}

func TestFileAt_MultiFile_CorrectFileSelected(t *testing.T) {
	dir := t.TempDir()
	p0 := filepath.Join(dir, "old.jsonl")
	p1 := filepath.Join(dir, "new.jsonl")
	writeLines(t, p0, []string{makeLine("old", "info", "daemon")})
	writeLines(t, p1, []string{makeLine("new", "info", "daemon")})

	fc := chainFromFiles(t, []string{p0, p1})
	// Offset 0 should be in file 0.
	idx0, _ := fc.fileAt(0)
	if idx0 != 0 {
		t.Errorf("offset 0: want file 0, got %d", idx0)
	}
	// Offset at boundary should be in file 1.
	idx1, phys1 := fc.fileAt(fc.files[0].size)
	if idx1 != 1 {
		t.Errorf("offset at boundary: want file 1, got %d", idx1)
	}
	if phys1 != 0 {
		t.Errorf("physOffset at boundary: want 0, got %d", phys1)
	}
}

// ── ReadForward — missing file in chain ──────────────────────────────────────

func TestReadForward_MissingFileInChain_Skipped(t *testing.T) {
	// If a rotated file is deleted between NewFileChain and ReadForward,
	// the reader should skip it gracefully and continue to the next file.
	dir := t.TempDir()
	p0 := filepath.Join(dir, "old.jsonl")
	p1 := filepath.Join(dir, "new.jsonl")
	writeLines(t, p0, []string{makeLine("old", "info", "daemon")})
	writeLines(t, p1, []string{makeLine("new", "info", "daemon")})

	fc := chainFromFiles(t, []string{p0, p1})

	// Delete the first file after building the chain.
	os.Remove(p0)

	got, _, _, _, err := fc.ReadForward(0, 100, noFilter())
	if err != nil {
		t.Fatalf("ReadForward error: %v", err)
	}
	// Should still get the line from the second file.
	if len(got) != 1 || !strings.Contains(string(got[0]), "new") {
		t.Errorf("want [new], got %v", lineTexts(got))
	}
}

// ── ReadBackward — large file requiring multiple chunks ──────────────────────

func TestReadBackward_LargeFile_MultipleChunks(t *testing.T) {
	// Create a file larger than backwardChunkSize (64KB) to exercise the
	// multi-chunk backward reading loop.
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")

	var lines []string
	// Each line is ~80 bytes; 1000 lines ≈ 80KB > 64KB chunk size.
	for i := 0; i < 1000; i++ {
		lines = append(lines, makeLine(fmt.Sprintf("line-%04d", i), "info", "daemon"))
	}
	writeLines(t, p, lines)

	fc := chainFromFiles(t, []string{p})
	got, _, _, _, err := fc.ReadBackward(fc.TailOffset(), 5, noFilter(), -1)
	if err != nil {
		t.Fatalf("ReadBackward error: %v", err)
	}
	if len(got) != 5 {
		t.Fatalf("want 5 lines, got %d", len(got))
	}
	// Last line should be line-0999.
	if !strings.Contains(string(got[4]), "line-0999") {
		t.Errorf("want last=line-0999, got %q", string(got[4]))
	}
	// First returned should be line-0995.
	if !strings.Contains(string(got[0]), "line-0995") {
		t.Errorf("want first=line-0995, got %q", string(got[0]))
	}
}

// ── ReadBackward — lines longer than one chunk ───────────────────────────────

// padLine returns a JSONL line of exactly n bytes (excluding the newline),
// carrying id so the line can be identified in failure messages.
func padLine(t *testing.T, n int, id string) string {
	t.Helper()
	base := makeLine(id, "info", "daemon")
	if len(base) > n {
		t.Fatalf("padLine: %q already needs %d bytes, want %d", id, len(base), n)
	}
	return makeLine(id+strings.Repeat("-", n-len(base)), "info", "daemon")
}

// checkBackwardRoundTrip reads the whole chain backwards and asserts that it
// yields want byte for byte, at the offsets implied by concatenating want, and
// that ReadForward from each returned offset re-reads the same line.
func checkBackwardRoundTrip(t *testing.T, fc *FileChain, want []string) {
	t.Helper()

	got, offsets, firstOffset, _, err := fc.ReadBackward(fc.TailOffset(), len(want)+10, noFilter(), -1)
	if err != nil {
		t.Fatalf("ReadBackward error: %v", err)
	}
	if len(got) != len(want) {
		t.Fatalf("want %d lines, got %d (lengths %v)", len(want), len(got), lineLengths(got))
	}

	var expected int64
	for i, w := range want {
		if string(got[i]) != w {
			t.Fatalf("line %d differs: want %d bytes, got %d bytes", i, len(w), len(got[i]))
		}
		if offsets[i] != expected {
			t.Fatalf("offset[%d]: want %d, got %d", i, expected, offsets[i])
		}
		expected += int64(len(w)) + 1

		fwd, _, _, _, ferr := fc.ReadForward(offsets[i], 1, noFilter())
		if ferr != nil {
			t.Fatalf("ReadForward(%d) error: %v", offsets[i], ferr)
		}
		if len(fwd) != 1 || string(fwd[0]) != w {
			t.Fatalf("ReadForward from offset[%d]=%d did not re-read line %d", i, offsets[i], i)
		}
	}
	if firstOffset != offsets[0] {
		t.Errorf("firstOffset: want %d, got %d", offsets[0], firstOffset)
	}
}

// lineLengths summarises a result set without dumping megabytes into the log.
func lineLengths(lines [][]byte) []int {
	out := make([]int, len(lines))
	for i, l := range lines {
		out[i] = len(l)
	}
	return out
}

func TestReadBackward_LineLongerThanChunk_ReturnedIntact(t *testing.T) {
	// A single line several backwardChunkSize (64 KB) chunks long used to come
	// back as one fragment per chunk, none of them valid JSON, so the browser
	// dropped the entry entirely.
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")

	line := makeLine(strings.Repeat("x", 200*1024), "info", "daemon")
	writeLines(t, p, []string{line})

	fc := chainFromFiles(t, []string{p})
	checkBackwardRoundTrip(t, fc, []string{line})
}

func TestReadBackward_LongLineAmongShortLines_NoBytesLost(t *testing.T) {
	// The long line's chunk boundaries fall inside neighbouring short lines
	// too, which is where the old boundary adjustment ate one content byte.
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")

	lines := []string{
		makeLine("before-1", "info", "daemon"),
		makeLine("before-2", "warning", "auth"),
		makeLine(strings.Repeat("y", 150*1024), "info", "daemon"),
		makeLine("after-1", "err", "daemon"),
		makeLine(strings.Repeat("z", 70*1024), "notice", "daemon"),
		makeLine("after-2", "info", "daemon"),
	}
	writeLines(t, p, lines)

	fc := chainFromFiles(t, []string{p})
	checkBackwardRoundTrip(t, fc, lines)
}

func TestReadBackward_ShortLines_MisalignedChunkBoundary(t *testing.T) {
	// 63-byte records do not divide the 64 KB chunk size, so every chunk
	// boundary lands mid-line. Each one used to split a record in two and
	// silently drop one byte of it.
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")

	const lineBytes = 62 // + '\n' = 63
	var lines []string
	for i := 0; i < 1700; i++ { // ~107 KB, spanning two chunks
		lines = append(lines, padLine(t, lineBytes, fmt.Sprintf("m%04d", i)))
	}
	writeLines(t, p, lines)

	fc := chainFromFiles(t, []string{p})
	checkBackwardRoundTrip(t, fc, lines)
}

func TestReadBackward_MultiFileChain_MultiChunkLines(t *testing.T) {
	// Each generation holds a line spanning several chunks; the carried
	// fragment must not leak from one file into the next.
	dir := t.TempDir()
	p0 := filepath.Join(dir, "old.jsonl")
	p1 := filepath.Join(dir, "mid.jsonl")
	p2 := filepath.Join(dir, "new.jsonl")

	oldLines := []string{
		makeLine("old-short", "info", "daemon"),
		makeLine(strings.Repeat("a", 130*1024), "info", "daemon"),
	}
	midLines := []string{
		makeLine(strings.Repeat("b", 70*1024), "warning", "daemon"),
	}
	newLines := []string{
		makeLine(strings.Repeat("c", 200*1024), "err", "daemon"),
		makeLine("new-short", "info", "daemon"),
	}
	writeLines(t, p0, oldLines)
	writeLines(t, p1, midLines)
	writeLines(t, p2, newLines)

	fc := chainFromFiles(t, []string{p0, p1, p2})

	var want []string
	want = append(want, oldLines...)
	want = append(want, midLines...)
	want = append(want, newLines...)
	checkBackwardRoundTrip(t, fc, want)
}

func TestReadBackward_LineSpanningFileBoundary_StaysSplit(t *testing.T) {
	// A file whose last line has no terminating newline is not joined with the
	// next generation — each file is its own line space, as ReadForward sees it.
	dir := t.TempDir()
	p0 := filepath.Join(dir, "old.jsonl")
	p1 := filepath.Join(dir, "new.jsonl")

	head := makeLine("unterminated", "info", "daemon")
	if err := os.WriteFile(p0, []byte(head), 0o600); err != nil { // no trailing \n
		t.Fatalf("WriteFile: %v", err)
	}
	tail := makeLine("next-generation", "info", "daemon")
	writeLines(t, p1, []string{tail})

	fc := chainFromFiles(t, []string{p0, p1})
	got, offsets, _, _, err := fc.ReadBackward(fc.TailOffset(), 10, noFilter(), -1)
	if err != nil {
		t.Fatalf("ReadBackward error: %v", err)
	}
	if len(got) != 2 {
		t.Fatalf("want 2 lines, got %d: %v", len(got), lineTexts(got))
	}
	if string(got[0]) != head || string(got[1]) != tail {
		t.Errorf("want [%q %q], got %v", head, tail, lineTexts(got))
	}
	if offsets[0] != 0 || offsets[1] != int64(len(head)) {
		t.Errorf("offsets: want [0 %d], got %v", len(head), offsets)
	}
}

func TestReadBackward_LineOverMaxLineBytes_DroppedNotBlocking(t *testing.T) {
	// An over-long line is skipped rather than buffered, and must not hide the
	// older lines behind it.
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")

	older := makeLine("older", "info", "daemon")
	newer := makeLine("newer", "info", "daemon")
	huge := makeLine(strings.Repeat("h", maxLineBytes+1), "info", "daemon")
	writeLines(t, p, []string{older, huge, newer})

	fc := chainFromFiles(t, []string{p})
	got, _, _, _, err := fc.ReadBackward(fc.TailOffset(), 10, noFilter(), -1)
	if err != nil {
		t.Fatalf("ReadBackward error: %v", err)
	}
	if len(got) != 2 {
		t.Fatalf("want 2 lines (over-long one dropped), got %d (lengths %v)", len(got), lineLengths(got))
	}
	if string(got[0]) != older || string(got[1]) != newer {
		t.Errorf("want [%q %q], got %v", older, newer, lineTexts(got))
	}
}

func TestReadBackward_LongLine_CountLimitStopsEarly(t *testing.T) {
	// Asking for fewer lines than the file holds must still return whole lines
	// when the newest ones are multi-chunk.
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")

	lines := []string{
		makeLine("oldest", "info", "daemon"),
		makeLine(strings.Repeat("q", 80*1024), "info", "daemon"),
		makeLine(strings.Repeat("r", 80*1024), "info", "daemon"),
	}
	writeLines(t, p, lines)

	fc := chainFromFiles(t, []string{p})
	got, offsets, _, _, err := fc.ReadBackward(fc.TailOffset(), 2, noFilter(), -1)
	if err != nil {
		t.Fatalf("ReadBackward error: %v", err)
	}
	if len(got) != 2 {
		t.Fatalf("want 2 lines, got %d (lengths %v)", len(got), lineLengths(got))
	}
	if string(got[0]) != lines[1] || string(got[1]) != lines[2] {
		t.Fatalf("wrong lines returned: lengths %v", lineLengths(got))
	}
	wantOff := int64(len(lines[0]) + 1)
	if offsets[0] != wantOff {
		t.Errorf("offset[0]: want %d, got %d", wantOff, offsets[0])
	}
}

func TestReadBackward_LongLine_SinceClampStillApplies(t *testing.T) {
	// `since` must clamp the same way when the boundary line spans chunks.
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")

	lines := []string{
		makeLine(strings.Repeat("s", 90*1024), "info", "daemon"),
		makeLine(strings.Repeat("t", 90*1024), "info", "daemon"),
	}
	writeLines(t, p, lines)

	fc := chainFromFiles(t, []string{p})
	since := int64(len(lines[0]) + 1)

	got, offsets, _, _, err := fc.ReadBackward(fc.TailOffset(), 10, noFilter(), since)
	if err != nil {
		t.Fatalf("ReadBackward error: %v", err)
	}
	if len(got) != 1 {
		t.Fatalf("want 1 line at/after since, got %d (lengths %v)", len(got), lineLengths(got))
	}
	if string(got[0]) != lines[1] {
		t.Errorf("wrong line returned: %d bytes", len(got[0]))
	}
	if offsets[0] != since {
		t.Errorf("offset[0]: want %d, got %d", since, offsets[0])
	}
}

// ── NewFileChain — via naming convention (additional) ────────────────────────

func TestNewFileChain_IncludesRotatedFiles_ReadOrder(t *testing.T) {
	dir := t.TempDir()
	active := filepath.Join(dir, "syslog.jsonl")
	rot1 := filepath.Join(dir, "syslog.1.jsonl")
	rot2 := filepath.Join(dir, "syslog.2.jsonl")

	writeLines(t, rot2, []string{makeLine("oldest", "info", "daemon")})
	writeLines(t, rot1, []string{makeLine("middle", "info", "daemon")})
	writeLines(t, active, []string{makeLine("newest", "info", "daemon")})

	sink := Sink{Name: "test", Path: active, MaxFiles: 5}
	fc, err := NewFileChain(sink)
	if err != nil {
		t.Fatalf("NewFileChain error: %v", err)
	}
	if len(fc.files) != 3 {
		t.Fatalf("want 3 files in chain, got %d", len(fc.files))
	}

	// Read all lines — should be oldest first.
	got, _, _, _, err := fc.ReadForward(0, 100, noFilter())
	if err != nil {
		t.Fatalf("ReadForward error: %v", err)
	}
	if len(got) != 3 {
		t.Fatalf("want 3 lines, got %d", len(got))
	}
	if !strings.Contains(string(got[0]), "oldest") {
		t.Errorf("first line should be oldest, got %q", string(got[0]))
	}
	if !strings.Contains(string(got[2]), "newest") {
		t.Errorf("last line should be newest, got %q", string(got[2]))
	}
}

// ── ReadBackward / Search — since parameter ──────────────────────────────────

func TestReadBackward_Since_ClampsAtBoundary(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	lines := []string{
		makeLine("zero", "info", "daemon"),
		makeLine("one", "info", "daemon"),
		makeLine("two", "info", "daemon"),
		makeLine("three", "info", "daemon"),
		makeLine("four", "info", "daemon"),
	}
	writeLines(t, p, lines)
	fc := chainFromFiles(t, []string{p})

	// Compute offset of line 3 (0-indexed): sum of lengths of lines 0..2 + newlines.
	since := int64(len(lines[0])+1) + int64(len(lines[1])+1) + int64(len(lines[2])+1)

	got, offsets, _, _, err := fc.ReadBackward(fc.TailOffset(), 100, noFilter(), since)
	if err != nil {
		t.Fatalf("ReadBackward error: %v", err)
	}
	if len(got) != 2 {
		t.Fatalf("want 2 lines (three, four), got %d: %v", len(got), lineTexts(got))
	}
	if !strings.Contains(string(got[0]), "three") {
		t.Errorf("first line should be 'three', got %q", string(got[0]))
	}
	if !strings.Contains(string(got[1]), "four") {
		t.Errorf("second line should be 'four', got %q", string(got[1]))
	}
	for i, off := range offsets {
		if off < since {
			t.Errorf("offset[%d]=%d is below since=%d", i, off, since)
		}
	}
}

func TestReadBackward_Since_AtTail_ReturnsEmpty(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	writeLines(t, p, []string{
		makeLine("one", "info", "daemon"),
		makeLine("two", "info", "daemon"),
	})
	fc := chainFromFiles(t, []string{p})

	since := fc.TailOffset()
	got, _, _, _, err := fc.ReadBackward(fc.TailOffset(), 100, noFilter(), since)
	if err != nil {
		t.Fatalf("ReadBackward error: %v", err)
	}
	if len(got) != 0 {
		t.Errorf("want 0 lines when since=TailOffset, got %d", len(got))
	}
}

func TestReadBackward_Since_Negative_ReturnsAll(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	lines := []string{
		makeLine("one", "info", "daemon"),
		makeLine("two", "info", "daemon"),
		makeLine("three", "info", "daemon"),
	}
	writeLines(t, p, lines)
	fc := chainFromFiles(t, []string{p})

	got, _, _, _, err := fc.ReadBackward(fc.TailOffset(), 100, noFilter(), -1)
	if err != nil {
		t.Fatalf("ReadBackward error: %v", err)
	}
	if len(got) != 3 {
		t.Fatalf("want 3 lines with since=-1 (no boundary), got %d", len(got))
	}
	if !strings.Contains(string(got[0]), "one") {
		t.Errorf("first line should be 'one', got %q", string(got[0]))
	}
	if !strings.Contains(string(got[2]), "three") {
		t.Errorf("last line should be 'three', got %q", string(got[2]))
	}
}

func TestReadBackward_Since_MultiFile(t *testing.T) {
	dir := t.TempDir()
	p0 := filepath.Join(dir, "old.jsonl")
	p1 := filepath.Join(dir, "new.jsonl")
	oldLines := []string{
		makeLine("old0", "info", "daemon"),
		makeLine("old1", "info", "daemon"),
		makeLine("old2", "info", "daemon"),
	}
	newLines := []string{
		makeLine("new0", "info", "daemon"),
		makeLine("new1", "info", "daemon"),
	}
	writeLines(t, p0, oldLines)
	writeLines(t, p1, newLines)
	fc := chainFromFiles(t, []string{p0, p1})

	// Set since in the middle of the first file: offset of old1 (skip old0).
	since := int64(len(oldLines[0]) + 1)

	got, offsets, _, _, err := fc.ReadBackward(fc.TailOffset(), 100, noFilter(), since)
	if err != nil {
		t.Fatalf("ReadBackward error: %v", err)
	}
	// Should return old1, old2, new0, new1 (4 lines).
	if len(got) != 4 {
		t.Fatalf("want 4 lines (old1, old2, new0, new1), got %d: %v", len(got), lineTexts(got))
	}
	if !strings.Contains(string(got[0]), "old1") {
		t.Errorf("first line should be 'old1', got %q", string(got[0]))
	}
	if !strings.Contains(string(got[3]), "new1") {
		t.Errorf("last line should be 'new1', got %q", string(got[3]))
	}
	for i, off := range offsets {
		if off < since {
			t.Errorf("offset[%d]=%d is below since=%d", i, off, since)
		}
	}
}

func TestSearch_Since_SkipsOlderLines(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	lines := []string{
		makeLine("target zero", "info", "daemon"),
		makeLine("target one", "info", "daemon"),
		makeLine("target two", "info", "daemon"),
		makeLine("target three", "info", "daemon"),
		makeLine("target four", "info", "daemon"),
	}
	writeLines(t, p, lines)
	fc := chainFromFiles(t, []string{p})

	// Set since to offset of line 3 (0-indexed): skip lines 0, 1, 2.
	since := int64(len(lines[0])+1) + int64(len(lines[1])+1) + int64(len(lines[2])+1)

	results, total, err := fc.Search("target", 100, noFilter(), since)
	if err != nil {
		t.Fatalf("Search error: %v", err)
	}
	if total != 2 {
		t.Errorf("totalMatches: want 2, got %d", total)
	}
	if len(results) != 2 {
		t.Fatalf("results: want 2, got %d", len(results))
	}
	if !strings.Contains(string(results[0].Line), "three") {
		t.Errorf("first result should contain 'three', got %q", string(results[0].Line))
	}
	if !strings.Contains(string(results[1].Line), "four") {
		t.Errorf("second result should contain 'four', got %q", string(results[1].Line))
	}
	for i, r := range results {
		if r.Offset < since {
			t.Errorf("result[%d].Offset=%d is below since=%d", i, r.Offset, since)
		}
	}
}

func TestSearch_Since_Negative_SearchesAll(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	lines := []string{
		makeLine("target one", "info", "daemon"),
		makeLine("other", "info", "daemon"),
		makeLine("target two", "info", "daemon"),
	}
	writeLines(t, p, lines)
	fc := chainFromFiles(t, []string{p})

	results, total, err := fc.Search("target", 100, noFilter(), -1)
	if err != nil {
		t.Fatalf("Search error: %v", err)
	}
	if total != 2 {
		t.Errorf("totalMatches: want 2, got %d", total)
	}
	if len(results) != 2 {
		t.Fatalf("results: want 2, got %d", len(results))
	}
}

// ── Handler /lines — empty chain (file doesn't exist) ────────────────────────

func TestHandler_Lines_EmptyChain_Returns200(t *testing.T) {
	dir := t.TempDir()
	// Sink points at a non-existent file.
	sink := Sink{Name: "main", Path: filepath.Join(dir, "missing.jsonl"), MaxFiles: 5}
	ts := newTestServer(t, []Sink{sink})
	defer ts.Close()

	resp := get(t, ts, "/lines?sink=main&tail=true&count=10")
	if resp.StatusCode != 200 {
		t.Fatalf("want 200, got %d", resp.StatusCode)
	}
	var result linesResponse
	decodeJSON(t, resp, &result)
	if len(result.Lines) != 0 {
		t.Errorf("want 0 lines for empty chain, got %d", len(result.Lines))
	}
	if result.TailOffset != 0 {
		t.Errorf("want tail_offset=0 for empty chain, got %d", result.TailOffset)
	}
}

func TestHandler_Lines_ForwardOnEmptyChain_Returns200(t *testing.T) {
	dir := t.TempDir()
	sink := Sink{Name: "main", Path: filepath.Join(dir, "missing.jsonl"), MaxFiles: 5}
	ts := newTestServer(t, []Sink{sink})
	defer ts.Close()

	resp := get(t, ts, "/lines?sink=main&offset=0&dir=forward&count=10")
	if resp.StatusCode != 200 {
		t.Fatalf("want 200, got %d", resp.StatusCode)
	}
	var result linesResponse
	decodeJSON(t, resp, &result)
	if len(result.Lines) != 0 {
		t.Errorf("want 0 lines, got %d", len(result.Lines))
	}
}

// ── maxResponseBytes (the byte budget) ────────────────────────────────────────

// nearMaxLineBytes is the record size the byte-budget tests use: just under
// maxLineBytes, so every read path accepts it, and large enough that a handful
// of them exhausts maxResponseBytes. A 65507-byte datagram of control bytes
// escapes to roughly this much JSONL, so it is the real worst case rather than
// an invented one.
const nearMaxLineBytes = maxLineBytes - 1024

// budgetLinesWanted is how many nearMaxLineBytes records a read collects before
// the budget stops it. Exact rather than approximate: a read stops before the
// first line that would take the total over, and the first-line exemption
// cannot apply because one record is well inside the budget.
const budgetLinesWanted = maxResponseBytes / nearMaxLineBytes

// nearMaxLine returns a valid JSONL record of exactly nearMaxLineBytes bytes
// whose message starts with marker, so it stays both searchable and
// identifiable once padded.
func nearMaxLine(t *testing.T, marker string) string {
	t.Helper()
	line := makeLine(marker, "info", "daemon")
	if len(line) > nearMaxLineBytes {
		t.Fatalf("nearMaxLine: marker %q leaves no room in %d bytes", marker, nearMaxLineBytes)
	}
	return makeLine(marker+strings.Repeat("p", nearMaxLineBytes-len(line)), "info", "daemon")
}

// nearMaxLines returns n such records, each identifiable by its index.
func nearMaxLines(t *testing.T, n int) []string {
	t.Helper()
	out := make([]string, n)
	for i := range out {
		out[i] = nearMaxLine(t, fmt.Sprintf("line-%02d-", i))
	}
	return out
}

// budgetChain writes n near-maximum records to a one-file chain and returns
// both, so a test can compare what a read gave back against what was written.
func budgetChain(t *testing.T, n int) (*FileChain, []string) {
	t.Helper()
	lines := nearMaxLines(t, n)
	p := filepath.Join(t.TempDir(), "a.jsonl")
	writeLines(t, p, lines)
	return chainFromFiles(t, []string{p}), lines
}

// totalBytes sums the sizes of the collected lines — the quantity the budget
// bounds, as against the number of them, which is all maxLines bounds.
func totalBytes(lines [][]byte) int {
	n := 0
	for _, l := range lines {
		n += len(l)
	}
	return n
}

func TestReadForward_ByteBudget_StopsBeforeExceedingIt(t *testing.T) {
	// Asking for maxLines of these records would be gigabytes, so what bounds
	// the read has to be the bytes collected and not the line count.
	fc, lines := budgetChain(t, budgetLinesWanted+2)

	got, offsets, first, next, err := fc.ReadForward(0, maxLines, noFilter())
	if err != nil {
		t.Fatalf("ReadForward error: %v", err)
	}

	if total := totalBytes(got); total > maxResponseBytes {
		t.Errorf("collected %d bytes, over the %d budget", total, maxResponseBytes)
	}
	// Fewer than asked for and fewer than the chain holds, so it was the budget
	// that ended the read rather than the count or the end of the file.
	if len(got) != budgetLinesWanted {
		t.Fatalf("got %d lines, want %d (lengths %v)", len(got), budgetLinesWanted, lineLengths(got))
	}
	// Whole records. An honest long line is what this looks like from the
	// reader's side, and truncating one mid-message would leave the browser
	// unable to parse it at all.
	for i := range got {
		if string(got[i]) != lines[i] {
			t.Fatalf("line %d is not the record written (%d bytes, want %d)",
				i, len(got[i]), len(lines[i]))
		}
	}
	if first != 0 || offsets[0] != 0 {
		t.Errorf("first offset: got first=%d offsets[0]=%d, want 0", first, offsets[0])
	}
	// The cursor stops at the start of the first line left out, so the next
	// request returns that line rather than skipping past it.
	wantNext := int64(budgetLinesWanted * (nearMaxLineBytes + 1))
	if next != wantNext {
		t.Errorf("next offset: got %d, want %d", next, wantNext)
	}
}

func TestReadForward_ByteBudget_PagingReturnsEveryLine(t *testing.T) {
	// A page cut short by the budget says so only through next_offset, so
	// following that cursor has to cover the chain without skipping a line or
	// repeating one — the contract the live tail and infinite scroll rely on.
	fc, lines := budgetChain(t, budgetLinesWanted+2)

	var seen []string
	offset := int64(0)
	for pages := 0; ; pages++ {
		if pages > len(lines) {
			t.Fatalf("paging did not finish after %d requests", pages)
		}
		got, _, _, next, err := fc.ReadForward(offset, maxLines, noFilter())
		if err != nil {
			t.Fatalf("ReadForward error: %v", err)
		}
		if len(got) == 0 {
			break
		}
		if next <= offset {
			t.Fatalf("cursor did not advance past %d", offset)
		}
		seen = append(seen, lineTexts(got)...)
		offset = next
	}

	if len(seen) != len(lines) {
		t.Fatalf("paging returned %d lines, want %d", len(seen), len(lines))
	}
	for i := range lines {
		if seen[i] != lines[i] {
			t.Fatalf("paged line %d is not the record written", i)
		}
	}
}

func TestReadBackward_ByteBudget_KeepsNewestAndPagesFurtherBack(t *testing.T) {
	fc, lines := budgetChain(t, budgetLinesWanted+2)

	got, _, first, next, err := fc.ReadBackward(fc.TailOffset(), maxLines, noFilter(), -1)
	if err != nil {
		t.Fatalf("ReadBackward error: %v", err)
	}

	if total := totalBytes(got); total > maxResponseBytes {
		t.Errorf("collected %d bytes, over the %d budget", total, maxResponseBytes)
	}
	// The walk runs newest-first, so the budget drops the older end of the
	// window: the newest budgetLinesWanted records, still in forward order.
	want := lines[len(lines)-budgetLinesWanted:]
	if len(got) != len(want) {
		t.Fatalf("got %d lines, want %d (lengths %v)", len(got), len(want), lineLengths(got))
	}
	for i := range want {
		if string(got[i]) != want[i] {
			t.Fatalf("line %d is not the record written (%d bytes, want %d)",
				i, len(got[i]), len(want[i]))
		}
	}
	if next != fc.TailOffset() {
		t.Errorf("next offset: got %d, want the tail %d", next, fc.TailOffset())
	}

	// first is where scrolling further back resumes, and the two records the
	// budget dropped are what comes back from there.
	older, _, _, _, err := fc.ReadBackward(first, maxLines, noFilter(), -1)
	if err != nil {
		t.Fatalf("ReadBackward error: %v", err)
	}
	if len(older) != 2 {
		t.Fatalf("paging back from %d gave %d lines, want 2 (lengths %v)",
			first, len(older), lineLengths(older))
	}
	for i := range older {
		if string(older[i]) != lines[i] {
			t.Fatalf("older line %d is not the record written", i)
		}
	}
}

func TestSearch_ByteBudget_BoundsResultsNotTotalMatches(t *testing.T) {
	fc, lines := budgetChain(t, budgetLinesWanted+2)

	results, total, err := fc.Search("line-", maxLines, noFilter(), -1)
	if err != nil {
		t.Fatalf("Search error: %v", err)
	}

	collected := 0
	for _, r := range results {
		collected += len(r.Line)
	}
	if collected > maxResponseBytes {
		t.Errorf("collected %d bytes, over the %d budget", collected, maxResponseBytes)
	}
	if len(results) != budgetLinesWanted {
		t.Fatalf("got %d results, want %d", len(results), budgetLinesWanted)
	}
	for i := range results {
		if string(results[i].Line) != lines[i] {
			t.Fatalf("result %d is not the record written (%d bytes, want %d)",
				i, len(results[i].Line), len(lines[i]))
		}
	}
	// As with limit, the budget bounds what is returned and not what is
	// counted, so the UI can still report how many matches the chain holds.
	if total != len(lines) {
		t.Errorf("total_matches = %d, want %d", total, len(lines))
	}
}

func TestSearch_ByteBudget_DoesNotAdmitLaterShorterMatches(t *testing.T) {
	// One near-maximum record past the budget closes collection; a short match
	// after that one still fits in the bytes left over, and must not be let in
	// anyway. Results have to stay the *first* k matches, which is the run the
	// UI's "match i of total_matches" navigation steps through — a gap in the
	// middle would have it jump somewhere the count does not explain.
	lines := append(nearMaxLines(t, budgetLinesWanted+1), makeLine("line-tiny", "info", "daemon"))
	p := filepath.Join(t.TempDir(), "a.jsonl")
	writeLines(t, p, lines)
	fc := chainFromFiles(t, []string{p})

	results, total, err := fc.Search("line-", maxLines, noFilter(), -1)
	if err != nil {
		t.Fatalf("Search error: %v", err)
	}
	if len(results) != budgetLinesWanted {
		t.Fatalf("got %d results, want %d", len(results), budgetLinesWanted)
	}
	if last := results[len(results)-1].Line; string(last) != lines[budgetLinesWanted-1] {
		t.Errorf("last result is %d bytes; the short trailing match got in", len(last))
	}
	if total != len(lines) {
		t.Errorf("total_matches = %d, want %d", total, len(lines))
	}
}
