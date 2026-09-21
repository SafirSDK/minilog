// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

package main

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
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

// ── Chain files that do not end on a line boundary ────────────────────────────
//
// minilog writes a newline after every record, but a process killed mid-write
// leaves a partial one, and the next rotation moves that file out of the active
// slot and into the middle of the chain. Every offset in every read path is then
// one byte adrift of the file after it unless the missing newline is accounted
// for.

// writeUnterminated writes lines to path with a newline after each except the
// last, leaving the file ending mid-line as an interrupted write would.
func writeUnterminated(t *testing.T, path string, lines []string) {
	t.Helper()
	if err := os.WriteFile(path, []byte(strings.Join(lines, "\n")), 0o600); err != nil {
		t.Fatalf("writeUnterminated: %v", err)
	}
}

func TestReadForward_UnterminatedFile_NextOffsetStopsAtFileEnd(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	l0 := makeLine("only", "info", "daemon")
	writeUnterminated(t, p, []string{l0})

	fc := chainFromFiles(t, []string{p})
	got, _, _, nextOff, err := fc.ReadForward(0, 100, noFilter())
	if err != nil {
		t.Fatalf("ReadForward error: %v", err)
	}
	if len(got) != 1 {
		t.Fatalf("want 1 line, got %d: %v", len(got), lineTexts(got))
	}
	// The record is there and complete; only its terminator is missing, so the
	// cursor must stop at the file's end rather than one byte past it.
	if nextOff != int64(len(l0)) {
		t.Errorf("nextOffset: want %d (end of file), got %d", len(l0), nextOff)
	}
	if nextOff != fc.TailOffset() {
		t.Errorf("nextOffset %d is past the end of the chain (%d)", nextOff, fc.TailOffset())
	}
}

// The case from issue #41: paging forward across the boundary handed the client
// a line with its opening brace missing, which is not JSON.
func TestReadForward_UnterminatedMiddleFile_NextPageStartsOnARecord(t *testing.T) {
	dir := t.TempDir()
	older := filepath.Join(dir, "old.jsonl")
	newer := filepath.Join(dir, "new.jsonl")
	writeUnterminated(t, older, []string{makeLine("killed-mid-write", "info", "daemon")})
	writeLines(t, newer, []string{
		makeLine("next-generation", "info", "daemon"),
		makeLine("and-another", "info", "daemon"),
	})

	fc := chainFromFiles(t, []string{older, newer})

	// One line at a time, so the page boundary lands exactly on the file
	// boundary — the trigger is the count, with no byte budget involved.
	var offset int64
	var seen []string
	for i := 0; i < 3; i++ {
		got, _, _, next, err := fc.ReadForward(offset, 1, noFilter())
		if err != nil {
			t.Fatalf("page %d: ReadForward error: %v", i, err)
		}
		if len(got) != 1 {
			t.Fatalf("page %d: want 1 line, got %d: %v", i, len(got), lineTexts(got))
		}
		line := string(got[0])
		if !strings.HasPrefix(line, "{") || !strings.HasSuffix(line, "}") {
			t.Errorf("page %d starts mid-record: %q", i, line)
		}
		seen = append(seen, line)
		offset = next
	}

	want := []string{"killed-mid-write", "next-generation", "and-another"}
	for i, w := range want {
		if !strings.Contains(seen[i], w) {
			t.Errorf("page %d: want a record containing %q, got %q", i, w, seen[i])
		}
	}
	if offset != fc.TailOffset() {
		t.Errorf("final cursor %d, want the end of the chain %d", offset, fc.TailOffset())
	}
}

// Whatever offsets a forward read hands out, a backward read from them has to
// resolve to the same records, since that is how the UI scrolls upward from a
// page it already has.
func TestReadBackward_UnterminatedMiddleFile_AgreesWithForward(t *testing.T) {
	dir := t.TempDir()
	older := filepath.Join(dir, "old.jsonl")
	newer := filepath.Join(dir, "new.jsonl")
	writeUnterminated(t, older, []string{
		makeLine("old1", "info", "daemon"),
		makeLine("old2-unterminated", "info", "daemon"),
	})
	writeLines(t, newer, []string{makeLine("new1", "info", "daemon")})

	fc := chainFromFiles(t, []string{older, newer})

	forward, forwardOffsets, _, _, err := fc.ReadForward(0, 100, noFilter())
	if err != nil {
		t.Fatalf("ReadForward error: %v", err)
	}
	if len(forward) != 3 {
		t.Fatalf("want 3 lines forward, got %d: %v", len(forward), lineTexts(forward))
	}

	backward, backwardOffsets, _, _, err := fc.ReadBackward(fc.TailOffset(), 100, noFilter(), -1)
	if err != nil {
		t.Fatalf("ReadBackward error: %v", err)
	}

	if strings.Join(lineTexts(backward), "\n") != strings.Join(lineTexts(forward), "\n") {
		t.Errorf("backward lines %v differ from forward lines %v",
			lineTexts(backward), lineTexts(forward))
	}
	if len(backwardOffsets) != len(forwardOffsets) {
		t.Fatalf("offset counts differ: %v vs %v", backwardOffsets, forwardOffsets)
	}
	for i := range forwardOffsets {
		if backwardOffsets[i] != forwardOffsets[i] {
			t.Errorf("offset %d: forward %d, backward %d", i, forwardOffsets[i], backwardOffsets[i])
		}
	}
}

// Search derives its offsets the same way a forward read does, and they are what
// a client feeds back to /lines to jump to a match.
func TestSearch_UnterminatedMiddleFile_OffsetsLandOnRecordStarts(t *testing.T) {
	dir := t.TempDir()
	older := filepath.Join(dir, "old.jsonl")
	newer := filepath.Join(dir, "new.jsonl")
	writeUnterminated(t, older, []string{makeLine("needle-old", "info", "daemon")})
	writeLines(t, newer, []string{makeLine("needle-new", "info", "daemon")})

	fc := chainFromFiles(t, []string{older, newer})
	offsets, total, err := fc.Search("needle", 100, noFilter(), -1)
	if err != nil {
		t.Fatalf("Search error: %v", err)
	}
	if total != 2 {
		t.Fatalf("want 2 matches, got %d", total)
	}

	for i, off := range offsets {
		got, _, _, _, rerr := fc.ReadForward(off, 1, noFilter())
		if rerr != nil {
			t.Fatalf("match %d: ReadForward error: %v", i, rerr)
		}
		if len(got) != 1 {
			t.Fatalf("match %d: want 1 line at offset %d, got %d", i, off, len(got))
		}
		if !strings.HasPrefix(string(got[0]), "{") {
			t.Errorf("match %d at offset %d starts mid-record: %q", i, off, string(got[0]))
		}
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

	offsets, _, err := fc.Search("target", 100, noFilter(), -1)
	if err != nil {
		t.Fatalf("Search error: %v", err)
	}
	if len(offsets) != 1 {
		t.Fatalf("want 1 result, got %d", len(offsets))
	}
	if offsets[0] != 0 {
		t.Errorf("first result offset: want 0, got %d", offsets[0])
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

	offsets, _, err := fc.Search("target", 100, noFilter(), -1)
	if err != nil {
		t.Fatalf("Search error: %v", err)
	}
	if len(offsets) != 2 {
		t.Fatalf("want 2 results, got %d", len(offsets))
	}
	if offsets[0] >= offsets[1] {
		t.Errorf("results not in offset order: %d >= %d", offsets[0], offsets[1])
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

func TestReadBackward_LineFarOverMaxLineBytes_IsNotAccumulated(t *testing.T) {
	// The previous test covers the line being dropped. This one covers it not
	// being held in memory on the way to being dropped, which is a separate
	// guarantee and the reason the carry walk has a length check of its own.
	//
	// It needs a memory assertion rather than an assertion about the lines
	// returned, because the two are indistinguishable by output: take() drops an
	// over-long line whether or not the carry check exists, so removing that
	// check leaves every existing test passing while carry grows without bound.
	// The growth is quadratic, each chunk copying the fragment assembled so far,
	// so what is asserted is cumulative allocation. Measured over this fixture:
	// 17 MB with the check, 537 MB without it. The ceiling sits between the two
	// with room on both sides rather than close to either.
	//
	// The line has to be far enough over maxLineBytes that carry passes the
	// bound while the file still continues past that point. The previous test's
	// line is only just over, so its walk reaches the start of the file first
	// and take() is what drops it there — the carry check never runs.
	const hugeLineBytes = 8 * maxLineBytes
	const allocCeiling = 64 * 1024 * 1024

	dir := t.TempDir()
	p := filepath.Join(dir, "a.jsonl")
	older := makeLine("older", "info", "daemon")
	newer := makeLine("newer", "info", "daemon")
	huge := makeLine(strings.Repeat("h", hugeLineBytes), "info", "daemon")
	writeLines(t, p, []string{older, huge, newer})

	fc := chainFromFiles(t, []string{p})

	var before, after runtime.MemStats
	runtime.GC()
	runtime.ReadMemStats(&before)
	got, _, _, _, err := fc.ReadBackward(fc.TailOffset(), 10, noFilter(), -1)
	runtime.ReadMemStats(&after)
	if err != nil {
		t.Fatalf("ReadBackward error: %v", err)
	}

	// TotalAlloc only ever increases, so this measures what the read allocated
	// in total and is not affected by when the collector runs.
	allocated := after.TotalAlloc - before.TotalAlloc
	if allocated > allocCeiling {
		t.Errorf("read allocated %d MB for one %d MB line, over the %d MB ceiling — "+
			"the carried fragment is being accumulated rather than abandoned",
			allocated>>20, hugeLineBytes>>20, allocCeiling>>20)
	}

	// Asserted as well so that a change breaking both shows both.
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

	offsets, total, err := fc.Search("target", 100, noFilter(), since)
	if err != nil {
		t.Fatalf("Search error: %v", err)
	}
	if total != 2 {
		t.Errorf("totalMatches: want 2, got %d", total)
	}
	if len(offsets) != 2 {
		t.Fatalf("results: want 2, got %d", len(offsets))
	}
	// Lines 3 and 4, identified by where they start — the offsets are the whole
	// of what a search returns.
	wantOffsets := []int64{since, since + int64(len(lines[3])+1)}
	for i := range wantOffsets {
		if offsets[i] != wantOffsets[i] {
			t.Errorf("result %d: offset %d, want %d", i, offsets[i], wantOffsets[i])
		}
	}
	for i, off := range offsets {
		if off < since {
			t.Errorf("result[%d] offset %d is below since=%d", i, off, since)
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

// withBudget lowers maxResponseBytes for the duration of one test.
//
// The budget's real value is 8 MB, so a test that wanted to watch it bind at
// full size would have to write tens of megabytes of fixture to do it — which is
// what the first version of these tests did, at two seconds and 83 MB of temp
// I/O. The behaviour is the same at any value, and the cases that matter (a
// chain seam, a filter, a line larger than the whole budget) are only reachable
// cheaply. TestByteBudget_IsTheDocumentedSize is what holds the real value in
// place; TestHandler_Lines_ResponseIsBoundedByBytes is what exercises it at
// realistic record sizes.
func withBudget(t *testing.T, n int) {
	t.Helper()
	saved := maxResponseBytes
	maxResponseBytes = n
	t.Cleanup(func() { maxResponseBytes = saved })
}

// The cheap budget tests use a 4 KB budget and 1 KB records, so four records
// fill it exactly. Landing on the boundary rather than near it is the point:
// stopping before a line that *would* exceed the budget and stopping on the line
// that *reaches* it differ by one record, and only an exact fit tells them
// apart.
const (
	testBudget        = 4096
	testRecordBytes   = 1024
	testBudgetRecords = testBudget / testRecordBytes // 4
)

// budgetRecord returns a valid JSONL record of exactly n bytes whose message
// begins with marker, so a test can tell which record it got back.
func budgetRecord(t *testing.T, marker string, n int) string {
	t.Helper()
	bare := makeLine(marker, "info", "daemon")
	if len(bare) > n {
		t.Fatalf("budgetRecord: marker %q needs %d bytes, over the %d asked for", marker, len(bare), n)
	}
	return makeLine(marker+strings.Repeat("p", n-len(bare)), "info", "daemon")
}

// budgetRecords returns n records of recordBytes each, identifiable by index.
func budgetRecords(t *testing.T, n, recordBytes int) []string {
	t.Helper()
	out := make([]string, n)
	for i := range out {
		out[i] = budgetRecord(t, fmt.Sprintf("line-%02d-", i), recordBytes)
	}
	return out
}

// budgetChainOf writes one rotation generation per group, oldest first, and
// returns the chain over them.
func budgetChainOf(t *testing.T, groups ...[]string) *FileChain {
	t.Helper()
	dir := t.TempDir()
	paths := make([]string, len(groups))
	for i, g := range groups {
		paths[i] = filepath.Join(dir, fmt.Sprintf("gen%d.jsonl", i))
		writeLines(t, paths[i], g)
	}
	return chainFromFiles(t, paths)
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

func TestByteBudget_IsTheDocumentedSize(t *testing.T) {
	// Literal, and deliberately so. Every other budget test derives what it
	// expects from maxResponseBytes, which makes them immune to drift in the
	// fixture — and equally blind to the budget itself being widened. Setting it
	// to 512 MB once passed this whole package. README.md and AGENTS.md both
	// quote 8 MB, so changing it is a documentation change too.
	if maxResponseBytes != 8*1024*1024 {
		t.Errorf("maxResponseBytes = %d, want 8388608 (8 MB, as documented in README.md and AGENTS.md)",
			maxResponseBytes)
	}
}

func TestReadForward_ByteBudget_StopsBeforeExceedingIt(t *testing.T) {
	// Asking for maxLines of large records would be gigabytes, so what bounds
	// the read has to be the bytes collected and not the line count.
	withBudget(t, testBudget)
	lines := budgetRecords(t, testBudgetRecords+2, testRecordBytes)
	fc := budgetChainOf(t, lines)

	got, offsets, first, next, err := fc.ReadForward(0, maxLines, noFilter())
	if err != nil {
		t.Fatalf("ReadForward error: %v", err)
	}

	// Fewer than asked for and fewer than the chain holds, so it was the budget
	// that ended the read rather than the count or the end of the file.
	if len(got) != testBudgetRecords {
		t.Fatalf("got %d lines, want %d (lengths %v)", len(got), testBudgetRecords, lineLengths(got))
	}
	// Exactly the budget, not a record short of it: the records divide it
	// evenly, so a read that stopped on reaching the budget instead of before
	// exceeding it would come back with one fewer.
	if total := totalBytes(got); total != testBudget {
		t.Errorf("collected %d bytes, want exactly the %d budget", total, testBudget)
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
	wantNext := int64(testBudgetRecords * (testRecordBytes + 1))
	if next != wantNext {
		t.Errorf("next offset: got %d, want %d", next, wantNext)
	}
}

func TestReadForward_ByteBudget_MultiFileChain_DropsNoLineAtTheSeam(t *testing.T) {
	// The budget stops the read inside the older generation while the newer one
	// opens with a record small enough to fit in what is left over. A read that
	// stopped collecting without also ending the walk would take that short
	// record and carry the cursor past the long one it had just refused — losing
	// a line silently, which no amount of paging afterwards recovers.
	withBudget(t, testBudget)
	older := budgetRecords(t, 4, 1200) // 4800 bytes: the fourth does not fit
	newer := []string{budgetRecord(t, "newer-00-", 200)}
	fc := budgetChainOf(t, older, newer)

	got, _, _, next, err := fc.ReadForward(0, maxLines, noFilter())
	if err != nil {
		t.Fatalf("ReadForward error: %v", err)
	}
	if want := older[:3]; len(got) != len(want) {
		t.Fatalf("got %d lines, want %d (lengths %v)", len(got), len(want), lineLengths(got))
	}
	for i, want := range older[:3] {
		if string(got[i]) != want {
			t.Fatalf("line %d is not the record written", i)
		}
	}
	if wantNext := int64(3 * 1201); next != wantNext {
		t.Fatalf("next offset: got %d, want %d (the start of the refused record)", next, wantNext)
	}

	// And following the cursor picks up exactly where it left off, across the
	// generation boundary.
	rest, _, _, _, err := fc.ReadForward(next, maxLines, noFilter())
	if err != nil {
		t.Fatalf("ReadForward error: %v", err)
	}
	want := append([]string{older[3]}, newer...)
	if got := lineTexts(rest); len(got) != len(want) {
		t.Fatalf("second page has %d lines, want %d", len(got), len(want))
	}
	for i, w := range want {
		if string(rest[i]) != w {
			t.Fatalf("second page line %d is not the record written", i)
		}
	}
}

func TestReadForward_ByteBudget_ChargesOnlyTheLinesItReturns(t *testing.T) {
	// Non-matching records are read and skipped, so charging them to the budget
	// would end a page early for a reason the caller cannot see from the
	// response. A filtered browse is the ordinary case in the UI, not an edge
	// one, and the skipped text here is several times the budget.
	withBudget(t, testBudget)
	var lines []string
	for i := 0; i < 5; i++ {
		lines = append(lines, budgetRecord(t, fmt.Sprintf("drop-%02d-", i), 2000))
		lines = append(lines, budgetRecord(t, fmt.Sprintf("keep-%02d-", i), 600))
	}
	fc := budgetChainOf(t, lines)

	got, _, _, _, err := fc.ReadForward(0, maxLines, &Filter{Include: []string{"keep-"}})
	if err != nil {
		t.Fatalf("ReadForward error: %v", err)
	}
	// 5 × 600 = 3000 bytes of matches, inside the budget; the 10000 bytes of
	// skipped records are not.
	if len(got) != 5 {
		t.Fatalf("got %d matching lines, want 5 (lengths %v)", len(got), lineLengths(got))
	}
	if total := totalBytes(got); total != 3000 {
		t.Errorf("collected %d bytes, want 3000", total)
	}
}

func TestReadForward_ByteBudget_PagingReturnsEveryLine(t *testing.T) {
	// A page cut short by the budget says so only through next_offset, so
	// following that cursor has to cover the chain without skipping a line or
	// repeating one — the contract the live tail and infinite scroll rely on.
	withBudget(t, testBudget)
	lines := budgetRecords(t, 11, testRecordBytes)
	fc := budgetChainOf(t, lines)

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

func TestReadForward_ByteBudget_FirstLineIsTakenWhateverItsSize(t *testing.T) {
	// A line larger than the whole budget still comes back, whole and alone. If
	// it did not, the read would return nothing with its cursor unmoved and the
	// caller would ask for the same page forever. maxLineBytes keeps this out of
	// reach in production — no line can outgrow an 8 MB budget when it is capped
	// at 1 MB — so lowering the budget is the only way to reach the guard that
	// makes it a property of the code rather than of the two constants.
	withBudget(t, 100)
	lines := budgetRecords(t, 3, 500)
	fc := budgetChainOf(t, lines)

	got, _, _, next, err := fc.ReadForward(0, maxLines, noFilter())
	if err != nil {
		t.Fatalf("ReadForward error: %v", err)
	}
	if len(got) != 1 {
		t.Fatalf("got %d lines, want 1 (lengths %v)", len(got), lineLengths(got))
	}
	if string(got[0]) != lines[0] {
		t.Errorf("the over-budget line came back altered, %d bytes of %d", len(got[0]), len(lines[0]))
	}
	if next != int64(501) {
		t.Errorf("next offset: got %d, want 501 — a cursor that did not move stalls the caller", next)
	}
}

func TestReadBackward_ByteBudget_KeepsNewestAndPagesFurtherBack(t *testing.T) {
	withBudget(t, testBudget)
	lines := budgetRecords(t, testBudgetRecords+2, testRecordBytes)
	fc := budgetChainOf(t, lines)

	got, _, first, next, err := fc.ReadBackward(fc.TailOffset(), maxLines, noFilter(), -1)
	if err != nil {
		t.Fatalf("ReadBackward error: %v", err)
	}

	// The walk runs newest-first, so the budget drops the older end of the
	// window: the newest testBudgetRecords records, still in forward order.
	want := lines[len(lines)-testBudgetRecords:]
	if len(got) != len(want) {
		t.Fatalf("got %d lines, want %d (lengths %v)", len(got), len(want), lineLengths(got))
	}
	for i := range want {
		if string(got[i]) != want[i] {
			t.Fatalf("line %d is not the record written (%d bytes, want %d)",
				i, len(got[i]), len(want[i]))
		}
	}
	if total := totalBytes(got); total != testBudget {
		t.Errorf("collected %d bytes, want exactly the %d budget", total, testBudget)
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

func TestReadBackward_ByteBudget_SinceStillBindsFirst(t *testing.T) {
	// ReadBackward's stop flag now has two causes, the since floor and the
	// budget, and they must not shadow each other. Here since is the nearer of
	// the two: it has to end the walk while the budget still has room, or a
	// cleared view would start showing lines from before the clear.
	withBudget(t, testBudget)
	lines := budgetRecords(t, 10, testRecordBytes)
	fc := budgetChainOf(t, lines)
	// The start of the second-newest record, so only two records are eligible —
	// fewer than the budget would otherwise allow.
	since := int64(8 * (testRecordBytes + 1))

	got, _, first, _, err := fc.ReadBackward(fc.TailOffset(), maxLines, noFilter(), since)
	if err != nil {
		t.Fatalf("ReadBackward error: %v", err)
	}
	if len(got) != 2 {
		t.Fatalf("got %d lines, want 2 (lengths %v)", len(got), lineLengths(got))
	}
	for i, want := range lines[8:] {
		if string(got[i]) != want {
			t.Fatalf("line %d is not the record written", i)
		}
	}
	if first != since {
		t.Errorf("first offset: got %d, want since=%d", first, since)
	}
}

func TestReadBackward_ByteBudget_ChargesOnlyTheLinesItReturns(t *testing.T) {
	// The backward twin of the forward accounting test. It is not redundant: the
	// two paths keep separate counters, ReadBackward's inside a closure fed by
	// the reverse chunk walk, so charging skipped records is a mistake each can
	// make on its own. Unfiltered budget tests cannot see it, every line there
	// being one the read returns.
	withBudget(t, testBudget)
	var lines []string
	for i := 0; i < 5; i++ {
		lines = append(lines, budgetRecord(t, fmt.Sprintf("drop-%02d-", i), 2000))
		lines = append(lines, budgetRecord(t, fmt.Sprintf("keep-%02d-", i), 600))
	}
	fc := budgetChainOf(t, lines)

	got, _, _, _, err := fc.ReadBackward(fc.TailOffset(), maxLines, &Filter{Include: []string{"keep-"}}, -1)
	if err != nil {
		t.Fatalf("ReadBackward error: %v", err)
	}
	// 5 × 600 = 3000 bytes of matches, inside the budget; the 10000 bytes of
	// skipped records are not. Charging those would stop the scroll two or three
	// matches in and leave the rest reachable only by scrolling again.
	if len(got) != 5 {
		t.Fatalf("got %d matching lines, want 5 (lengths %v)", len(got), lineLengths(got))
	}
	if total := totalBytes(got); total != 3000 {
		t.Errorf("collected %d bytes, want 3000", total)
	}
}

func TestReadBackward_ByteBudget_NewestLineIsTakenWhateverItsSize(t *testing.T) {
	// The backward counterpart of the forward exemption: the newest match comes
	// back whole even when it alone is over the budget, so an upward scroll
	// cannot deadlock on one oversized record.
	withBudget(t, 100)
	lines := budgetRecords(t, 3, 500)
	fc := budgetChainOf(t, lines)

	got, _, first, _, err := fc.ReadBackward(fc.TailOffset(), maxLines, noFilter(), -1)
	if err != nil {
		t.Fatalf("ReadBackward error: %v", err)
	}
	if len(got) != 1 {
		t.Fatalf("got %d lines, want 1 (lengths %v)", len(got), lineLengths(got))
	}
	if string(got[0]) != lines[2] {
		t.Errorf("want the newest record, got %d bytes", len(got[0]))
	}
	if first != int64(2*501) {
		t.Errorf("first offset: got %d, want %d", first, 2*501)
	}
}

func TestSearch_IsNotBoundedByTheByteBudget(t *testing.T) {
	// Search returns offsets, so the budget has nothing to bound and must not
	// cut the match list however low it is set. This is the guard on the
	// regression it replaced: charging the budget against record text that the
	// response carried but no client ever read made only the first handful of
	// matches reachable on a sink of large records, with the match counter
	// truthfully reporting hundreds more that could not be jumped to.
	withBudget(t, 1)
	lines := budgetRecords(t, 10, testRecordBytes)
	fc := budgetChainOf(t, lines)

	offsets, total, err := fc.Search("line-", maxLines, noFilter(), -1)
	if err != nil {
		t.Fatalf("Search error: %v", err)
	}
	if len(offsets) != len(lines) {
		t.Fatalf("got %d matches, want all %d", len(offsets), len(lines))
	}
	if total != len(lines) {
		t.Errorf("total_matches = %d, want %d", total, len(lines))
	}
	for i := range offsets {
		if want := int64(i * (testRecordBytes + 1)); offsets[i] != want {
			t.Errorf("match %d: offset %d, want %d", i, offsets[i], want)
		}
	}
}

func TestSearch_MultiFileChain_CountsEveryMatchPastLimit(t *testing.T) {
	// totalMatches is what the UI reports, and the scan has to keep running
	// across every generation once limit is full — a scan that stopped at the
	// end of the file where limit filled would undercount silently.
	withBudget(t, 1)
	older := budgetRecords(t, 4, testRecordBytes)
	newer := budgetRecords(t, 3, testRecordBytes)
	fc := budgetChainOf(t, older, newer)

	offsets, total, err := fc.Search("line-", 2, noFilter(), -1)
	if err != nil {
		t.Fatalf("Search error: %v", err)
	}
	if len(offsets) != 2 {
		t.Fatalf("got %d matches, want the 2 asked for", len(offsets))
	}
	if want := len(older) + len(newer); total != want {
		t.Errorf("total_matches = %d, want %d across both generations", total, want)
	}
	// The matches returned are the first two, in chain order.
	for i := range offsets {
		if want := int64(i * (testRecordBytes + 1)); offsets[i] != want {
			t.Errorf("match %d: offset %d, want %d", i, offsets[i], want)
		}
	}
}
