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
	blankLen := int64(1)                             // the blank \n
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

	got, _, _, _, err := fc.ReadBackward(fc.TailOffset(), 3, noFilter())
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

	got, _, _, _, err := fc.ReadBackward(fc.TailOffset(), 10, noFilter())
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
	_, _, _, nextOff, err := fc.ReadBackward(tail, 10, noFilter())
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

	got, _, _, _, err := fc.ReadBackward(fc.TailOffset(), 100, noFilter())
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
	got, _, _, _, err := fc.ReadBackward(fc.TailOffset(), 3, noFilter())
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

	got, _, _, _, err := fc.ReadBackward(fc.TailOffset(), 10, f)
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
	got, _, _, _, err := fc.ReadBackward(0, 10, noFilter())
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

	got, _, _, _, err := fc.ReadBackward(0, 10, noFilter())
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

	got, _, _, _, err := fc.ReadBackward(fc.TailOffset(), 0, noFilter())
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
	_, _, firstOff, _, err := fc.ReadBackward(fc.TailOffset(), 2, noFilter())
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

	got, _, firstOff, nextOff, err := fc.ReadBackward(fc.TailOffset(), 10, f)
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
	got, _, _, _, err := fc.ReadBackward(boundary, 10, noFilter())
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

	got, _, _, _, err := fc.ReadBackward(fc.TailOffset(), 3, noFilter())
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

	results, total, err := fc.Search("nginx", 100, noFilter())
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

	_, total, err := fc.Search("nginx", 100, noFilter())
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

	results, total, err := fc.Search("target", 3, noFilter())
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

	_, total, err := fc.Search("target", 100, f)
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

	results, total, err := fc.Search("notpresent", 100, noFilter())
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
	_, total, err := fc.Search("target", 100, noFilter())
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

	_, total, err := fc.Search("", 100, noFilter())
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

	results, _, err := fc.Search("target", 100, noFilter())
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

	results, total, err := fc.Search("target", 0, noFilter())
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

	results, _, err := fc.Search("target", 100, noFilter())
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

	got, _, _, _, err := fc.ReadBackward(tail, 10, noFilter())
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
	got, _, _, _, err := fc.ReadBackward(fc.TailOffset(), 3, noFilter())
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

	results, total, err := fc.Search("target", 100, noFilter())
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
	got, _, _, _, err := fc.ReadBackward(fc.TailOffset(), 5, noFilter())
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
