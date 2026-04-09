// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

package main

import (
	"bufio"
	"bytes"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

// ── Filter ───────────────────────────────────────────────────────────────────

// Filter describes which log lines should be returned by the file chain.
// All conditions must pass for a line to be included.
type Filter struct {
	Severities []string // allowed severity names; nil/empty = all
	Facilities []string // allowed facility names; nil/empty = all
	Include    []string // lowercase substrings; any must match raw line; empty = none required
	Exclude    []string // lowercase substrings; any match → skip line
}

// Match reports whether line passes all filter conditions.
// line should be the raw (non-lowercased) JSONL record bytes.
func (f *Filter) Match(line []byte) bool {
	lower := bytes.ToLower(line)

	// Exclude: any match → reject.
	for _, p := range f.Exclude {
		if bytes.Contains(lower, []byte(p)) {
			return false
		}
	}

	// Include: if specified, at least one must match.
	if len(f.Include) > 0 {
		found := false
		for _, p := range f.Include {
			if bytes.Contains(lower, []byte(p)) {
				found = true
				break
			}
		}
		if !found {
			return false
		}
	}

	// Severity allowlist: match the "severity" JSON string field.
	if len(f.Severities) > 0 {
		if !matchStringField(lower, "severity", f.Severities) {
			return false
		}
	}

	// Facility allowlist: match the "facility" JSON string field.
	if len(f.Facilities) > 0 {
		if !matchStringField(lower, "facility", f.Facilities) {
			return false
		}
	}

	return true
}

// matchStringField returns true if the JSON field `name` in line (already
// lowercased) has a quoted string value that appears in the allowed set.
// Handles both compact (`"severity":"info"`) and spaced (`"severity": "info"`) JSON.
// The allowed values must already be lowercased.
func matchStringField(lower []byte, name string, allowed []string) bool {
	prefix := fmt.Sprintf(`"%s":`, name)
	idx := bytes.Index(lower, []byte(prefix))
	if idx < 0 {
		return false
	}
	// Scan past the prefix and any whitespace to find the opening quote.
	rest := lower[idx+len(prefix):]
	for len(rest) > 0 && (rest[0] == ' ' || rest[0] == '\t') {
		rest = rest[1:]
	}
	if len(rest) == 0 || rest[0] != '"' {
		// Value is null or a number — only matches if we explicitly allow it (we don't).
		return false
	}
	rest = rest[1:] // skip opening quote
	end := bytes.IndexByte(rest, '"')
	if end < 0 {
		return false
	}
	val := string(rest[:end])
	for _, a := range allowed {
		if a == val {
			return true
		}
	}
	return false
}

// ── FileChain ─────────────────────────────────────────────────────────────────

// chainFile represents one file in the rotation chain.
type chainFile struct {
	path  string
	start int64 // logical offset of this file's first byte in the chain
	size  int64
}

// FileChain presents the full sequence of rotated log files as a single
// logical byte stream. The oldest file has logical offset 0; the active
// (newest) file ends at TailOffset().
//
// A new FileChain should be built on every request — it snapshots the
// current state of the filesystem and is safe under log rotation.
type FileChain struct {
	files []chainFile // oldest first
	total int64
}

// NewFileChain discovers all files in the rotation chain for sink and builds
// the logical offset map.
//
// Rotation naming: syslog.N.jsonl … syslog.1.jsonl, syslog.jsonl.
// MaxFiles controls how many rotated generations to probe.
func NewFileChain(sink Sink) (*FileChain, error) {
	base := sink.Path
	ext := filepath.Ext(base)
	stem := strings.TrimSuffix(base, ext)

	var candidates []string
	// Oldest generations first.
	for n := sink.MaxFiles; n >= 1; n-- {
		candidates = append(candidates, fmt.Sprintf("%s.%d%s", stem, n, ext))
	}
	// Active file last (newest).
	candidates = append(candidates, base)

	var files []chainFile
	var offset int64
	for _, path := range candidates {
		info, err := os.Stat(path)
		if err != nil || info.Size() == 0 {
			continue
		}
		files = append(files, chainFile{path: path, start: offset, size: info.Size()})
		offset += info.Size()
	}

	return &FileChain{files: files, total: offset}, nil
}

// TailOffset returns the current logical end of the chain.
func (fc *FileChain) TailOffset() int64 { return fc.total }

// Empty reports whether the chain contains no data.
func (fc *FileChain) Empty() bool { return len(fc.files) == 0 }

// fileAt returns the index of the file containing logicalOffset and the
// physical byte position within that file.
func (fc *FileChain) fileAt(logicalOffset int64) (fileIdx int, physOffset int64) {
	if logicalOffset >= fc.total {
		// Past end — clamp to last file's end.
		last := len(fc.files) - 1
		return last, fc.files[last].size
	}
	for i, f := range fc.files {
		if logicalOffset < f.start+f.size {
			return i, logicalOffset - f.start
		}
	}
	// Should not happen if logicalOffset < fc.total.
	last := len(fc.files) - 1
	return last, fc.files[last].size
}

// ── ReadForward ───────────────────────────────────────────────────────────────

// ReadForward reads up to count lines forward from logicalOffset, applying
// filter f. It crosses file boundaries transparently.
//
// Returns:
//   - lines: matching raw JSONL lines (no trailing newline)
//   - offsets: logical byte offset of the start of each returned line
//   - firstOffset: logical offset of the first returned line (0 if none)
//   - nextOffset: logical offset just past the last returned line
func (fc *FileChain) ReadForward(logicalOffset int64, count int, f *Filter) (
	lines [][]byte, offsets []int64, firstOffset, nextOffset int64, err error,
) {
	if fc.Empty() || count == 0 {
		return nil, nil, logicalOffset, logicalOffset, nil
	}

	fileIdx, physOffset := fc.fileAt(logicalOffset)
	nextOffset = logicalOffset

	for fileIdx < len(fc.files) && len(lines) < count {
		cf := fc.files[fileIdx]

		fh, ferr := os.Open(cf.path)
		if ferr != nil {
			fileIdx++
			physOffset = 0
			continue
		}

		if _, serr := fh.Seek(physOffset, io.SeekStart); serr != nil {
			fh.Close()
			fileIdx++
			physOffset = 0
			continue
		}

		// Limit read to what was snapshotted so we don't read partial lines
		// being written concurrently.
		reader := io.LimitReader(fh, cf.size-physOffset)
		scanner := bufio.NewScanner(reader)
		// Support lines up to 1 MB (generous for JSONL).
		scanner.Buffer(make([]byte, 64*1024), 1024*1024)

		currentLogical := cf.start + physOffset

		for scanner.Scan() && len(lines) < count {
			raw := scanner.Bytes()
			lineLen := int64(len(raw)) + 1 // +1 for the '\n' scanner strips

			if len(raw) > 0 && f.Match(raw) {
				if len(lines) == 0 {
					firstOffset = currentLogical
				}
				cp := make([]byte, len(raw))
				copy(cp, raw)
				lines = append(lines, cp)
				offsets = append(offsets, currentLogical)
			}
			currentLogical += lineLen
			nextOffset = currentLogical
		}
		fh.Close()

		fileIdx++
		physOffset = 0
	}

	if len(lines) == 0 {
		firstOffset = logicalOffset
		nextOffset = logicalOffset
	}

	return lines, offsets, firstOffset, nextOffset, nil
}

// ── ReadBackward ──────────────────────────────────────────────────────────────

const backwardChunkSize = 64 * 1024 // 64 KB per backward read

// ReadBackward reads up to count lines ending just before logicalOffset,
// applying filter f. Returns lines in forward (oldest-first) order so the
// caller can prepend them to the DOM without reversing.
//
// Returns:
//   - lines: matching raw JSONL lines
//   - offsets: logical byte offset of each returned line
//   - firstOffset: logical offset of the first (oldest) returned line
//   - nextOffset: == logicalOffset (the start of the already-loaded window)
func (fc *FileChain) ReadBackward(logicalOffset int64, count int, f *Filter) (
	lines [][]byte, offsets []int64, firstOffset, nextOffset int64, err error,
) {
	nextOffset = logicalOffset

	if fc.Empty() || count == 0 || logicalOffset == 0 {
		return nil, nil, logicalOffset, logicalOffset, nil
	}

	// We collect lines in reverse order, then flip at the end.
	type entry struct {
		line   []byte
		offset int64
	}
	var collected []entry

	fileIdx, physOffset := fc.fileAt(logicalOffset)
	// physOffset may equal the file size if logicalOffset is at a file boundary.
	// Move to the end of the previous file in that case.
	if physOffset == 0 && fileIdx > 0 {
		fileIdx--
		physOffset = fc.files[fileIdx].size
	}

	for fileIdx >= 0 && len(collected) < count {
		cf := fc.files[fileIdx]
		readEnd := physOffset // read up to this physical position in this file

		for readEnd > 0 && len(collected) < count {
			chunkStart := readEnd - backwardChunkSize
			if chunkStart < 0 {
				chunkStart = 0
			}
			chunkSize := readEnd - chunkStart

			fh, ferr := os.Open(cf.path)
			if ferr != nil {
				break
			}
			if _, serr := fh.Seek(chunkStart, io.SeekStart); serr != nil {
				fh.Close()
				break
			}
			buf := make([]byte, chunkSize)
			n, rerr := io.ReadFull(fh, buf)
			fh.Close()
			if n == 0 || (rerr != nil && rerr != io.ErrUnexpectedEOF) {
				break
			}
			buf = buf[:n]

			// Find newlines from right to left.
			// Each '\n' at position p means the line ending at p starts after
			// the previous '\n'.
			end := len(buf)
			// If the chunk ends exactly at readEnd and readEnd < file size,
			// the byte at end is the '\n' terminating the line before our
			// window — skip it.
			if readEnd < cf.size {
				// The last byte in buf is a '\n' we already account for;
				// scan from end-1 to avoid double-counting.
				end--
			}

			for i := end - 1; i >= 0 && len(collected) < count; i-- {
				if buf[i] == '\n' || i == 0 {
					lineStart := i
					if buf[i] == '\n' {
						lineStart = i + 1
					}
					raw := bytes.TrimRight(buf[lineStart:end], "\r")
					logicalLineStart := cf.start + chunkStart + int64(lineStart)

					if len(raw) > 0 && f.Match(raw) {
						cp := make([]byte, len(raw))
						copy(cp, raw)
						collected = append(collected, entry{line: cp, offset: logicalLineStart})
					}
					end = i // next iteration ends before this newline
				}
			}

			readEnd = chunkStart
		}

		fileIdx--
		if fileIdx >= 0 {
			physOffset = fc.files[fileIdx].size
		}
	}

	if len(collected) == 0 {
		return nil, nil, logicalOffset, logicalOffset, nil
	}

	// Trim to count and reverse to forward order.
	if len(collected) > count {
		collected = collected[:count]
	}
	lines = make([][]byte, len(collected))
	offsets = make([]int64, len(collected))
	for i, e := range collected {
		lines[len(collected)-1-i] = e.line
		offsets[len(collected)-1-i] = e.offset
	}
	firstOffset = offsets[0]

	return lines, offsets, firstOffset, nextOffset, nil
}

// ── Search ────────────────────────────────────────────────────────────────────

// SearchResult is one matching line and its logical offset.
type SearchResult struct {
	Line   []byte
	Offset int64
}

// Search scans the entire chain oldest-to-newest for lines matching query q
// (case-insensitive substring of raw JSON) that also pass filter f.
//
// Returns up to limit results plus the true total match count.
func (fc *FileChain) Search(q string, limit int, f *Filter) (results []SearchResult, totalMatches int, err error) {
	qLower := []byte(strings.ToLower(q))

	for _, cf := range fc.files {
		fh, ferr := os.Open(cf.path)
		if ferr != nil {
			continue
		}

		scanner := bufio.NewScanner(fh)
		scanner.Buffer(make([]byte, 64*1024), 1024*1024)

		var physOffset int64
		for scanner.Scan() {
			raw := scanner.Bytes()
			lineLen := int64(len(raw)) + 1
			logicalOffset := cf.start + physOffset
			physOffset += lineLen

			if len(raw) == 0 {
				continue
			}

			lower := bytes.ToLower(raw)
			if !bytes.Contains(lower, qLower) {
				continue
			}
			if !f.Match(raw) {
				continue
			}

			totalMatches++
			if len(results) < limit {
				cp := make([]byte, len(raw))
				copy(cp, raw)
				results = append(results, SearchResult{Line: cp, Offset: logicalOffset})
			}
		}
		fh.Close()
	}

	return results, totalMatches, nil
}
