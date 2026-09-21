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
//
// This is a substring scan, not a JSON parse, and that is a deliberate trade:
// the filter runs over every line of a rotation chain that can be gigabytes, on
// every request, and parsing each line would be several times the cost of
// reading it.
//
// It is exact for files minilog writes. Two properties make it so, and both are
// guaranteed by the writer rather than checked here:
//
//   - Field order is fixed (see the JSONL record format in AGENTS.md and the
//     README), and `message` comes last. So the first `"severity":` in a line
//     is the real field, never one quoted inside message text.
//   - `facility` and `severity` values are table-driven names, so the value
//     between the quotes cannot itself contain a quote.
//
// For a foreign JSONL file neither holds. A line whose message body contains
// `"severity": "error"` earlier than the real field would be matched on the
// wrong value — lines returned that do not match the filter, and lines hidden
// that do. That is a quiet wrong-results failure rather than an error, and it
// is the known cost of not parsing here.
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

// maxLineBytes bounds a single log line in every read path: the scanners in
// ReadForward and Search reject longer tokens, and ReadBackward drops them.
// Generous for JSONL — a maximum-size datagram with every byte escaped as
// \u00NN still fits.
const maxLineBytes = 1024 * 1024

// maxResponseBytes bounds the total size of the log lines one read collects.
// ReadForward and ReadBackward stop collecting once the lines gathered would
// exceed it. Search needs no such bound; see why on Search itself.
//
// maxLines bounds how many lines a request returns, which is only a memory
// bound if lines are of typical size — and a syslog sender chooses the size. A
// 65507-byte datagram of control bytes escapes to roughly 400 KB of JSONL
// (boost::json writes \u00XX, six bytes out for one in), comfortably under
// maxLineBytes, so maxLines of those is about 2 GB materialised as [][]byte,
// copied again into []string and buffered whole by json.Encoder before a byte
// reaches the socket. This is what makes the bound real rather than
// typical-case.
//
// What it bounds is the JSONL read off disk, not the response body, and the two
// differ by more than framing: encoding/json escapes <, > and & to the \u00NN
// form, one byte in for six out, while boost::json leaves those three alone when
// writing the record. So a sink of records dense in them — an application
// logging XML, no attacker needed — still turns this 8 MB into a body and a
// marshal buffer of roughly 48 MB each. That expansion is the documented cost of
// one request rather than a defect: the escaping is what makes the body safe to
// embed, and the point of the budget is that the figure is tens of megabytes
// instead of gigabytes.
//
// 8 MB is above anything a legitimate caller asks for: the browser UI requests
// 200 lines a page, so it meets the budget only on a sink whose records average
// over 40 KB, and an explicit count of maxLines typical records is a couple of
// megabytes. So the budget costs nothing on honest traffic.
//
// That is the bound for one request, and nothing bounds how many requests are in
// flight — the figure for the process is this times the number of concurrent
// readers. Left that way deliberately. The viewer serves an operator or a
// handful of them, and assets/app.js issues one read at a time per tab: every
// fetch is awaited, and the live tail skips a poll while the previous one is
// still running, so a tab cannot multiply itself into several. A handful of tabs
// each paging a sink of 40 KB records at once is therefore a few hundred
// megabytes transient, and that is the accepted ceiling for this process. A cap
// on concurrent reads would bound it tighter, at the cost of a number to pick
// and of refusing honest requests, which is not worth it at this scale. What
// would change the trade is fan-out this does not assume: a crawler following a
// bookmarked count=5000 URL, or many more operators than a handful. Then the cap
// is worth adding, and the per-request figure above is what to multiply.
//
// Collected bytes therefore stay at or under maxResponseBytes, with one
// exemption: a line that would exceed the budget on its own is still collected
// when it is the first one, so an honest long line is never truncated
// mid-record and a read never comes back empty with its cursor unmoved, which
// would stall the caller on that line forever. While maxLineBytes is the
// smaller of the two that exemption cannot fire, every line already fitting —
// it is there to make the invariant a property of the code rather than of the
// current pair of numbers.
//
// A read cut short by the budget is not signalled separately: every path leaves
// its cursor just past the last line collected, so the next request continues
// from there exactly as it does when the line count runs out.
//
// A var rather than a const only so tests can lower it: at 8 MB, seeing the
// budget bind costs tens of megabytes of fixture per test. Nothing outside the
// tests assigns it.
var maxResponseBytes = 8 * 1024 * 1024

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

	// Size of the lines collected so far, and whether the budget ended the read;
	// see maxResponseBytes.
	collectedBytes := 0
	budgetHit := false

	for fileIdx < len(fc.files) && len(lines) < count && !budgetHit {
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
		// Long lines are handled explicitly; see maxLineBytes.
		scanner.Buffer(make([]byte, 64*1024), maxLineBytes)

		currentLogical := cf.start + physOffset

		for scanner.Scan() && len(lines) < count {
			raw := scanner.Bytes()
			lineLen := int64(len(raw)) + 1 // +1 for the '\n' scanner strips

			if len(raw) > 0 && f.Match(raw) {
				// Stop before a line that would take the page over the byte
				// budget, without advancing nextOffset past it, so the next
				// request starts with that line rather than skipping it. The
				// first match is always taken, whatever its size.
				if len(lines) > 0 && collectedBytes+len(raw) > maxResponseBytes {
					budgetHit = true
					break
				}
				if len(lines) == 0 {
					firstOffset = currentLogical
				}
				cp := make([]byte, len(raw))
				copy(cp, raw)
				lines = append(lines, cp)
				offsets = append(offsets, currentLogical)
				collectedBytes += len(raw)
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

// readChunk returns the bytes of path in [start, end). The whole range must be
// readable: a short read means the file was truncated or rotated since the
// chain was snapshotted, and every offset derived from that snapshot is then
// meaningless, so the caller must give up on the file rather than splice
// together bytes from two generations.
func readChunk(path string, start, end int64) ([]byte, error) {
	fh, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer fh.Close()

	if _, err := fh.Seek(start, io.SeekStart); err != nil {
		return nil, err
	}
	buf := make([]byte, end-start)
	if _, err := io.ReadFull(fh, buf); err != nil {
		return nil, err
	}
	return buf, nil
}

// ReadBackward reads up to count lines ending just before logicalOffset,
// applying filter f. Returns lines in forward (oldest-first) order so the
// caller can prepend them to the DOM without reversing.
//
// Returns:
//   - lines: matching raw JSONL lines
//   - offsets: logical byte offset of each returned line
//   - firstOffset: logical offset of the first (oldest) returned line
//   - nextOffset: == logicalOffset (the start of the already-loaded window)
func (fc *FileChain) ReadBackward(logicalOffset int64, count int, f *Filter, since int64) (
	lines [][]byte, offsets []int64, firstOffset, nextOffset int64, err error,
) {
	nextOffset = logicalOffset

	if fc.Empty() || count == 0 || logicalOffset == 0 {
		return nil, nil, logicalOffset, logicalOffset, nil
	}

	if since >= 0 && logicalOffset <= since {
		return nil, nil, logicalOffset, logicalOffset, nil
	}

	// We collect lines in reverse order, then flip at the end.
	type entry struct {
		line   []byte
		offset int64
	}
	var collected []entry
	// Size of the lines collected so far; see maxResponseBytes.
	collectedBytes := 0

	// Set once a line starting before `since` is reached, or once the byte
	// budget is full. Offsets only shrink as the walk goes back, so nothing
	// further back can qualify either.
	stop := false

	fileIdx, physOffset := fc.fileAt(logicalOffset)
	// physOffset may equal the file size if logicalOffset is at a file boundary.
	// Move to the end of the previous file in that case.
	if physOffset == 0 && fileIdx > 0 {
		fileIdx--
		physOffset = fc.files[fileIdx].size
	}

	for fileIdx >= 0 && !stop && len(collected) < count {
		cf := fc.files[fileIdx]

		// A line longer than a chunk spans several of them, and its bytes are
		// only complete once the '\n' that precedes it has been found. carry
		// holds the already-seen tail of such a line while the chunks holding
		// its head are read. Lines never span *files*: each rotated generation
		// begins on a line boundary, so carry is per file.
		var carry []byte
		// A line past maxLineBytes is dropped instead of buffered, so that one
		// pathological line can neither pin unbounded memory nor — as simply
		// bailing out would — hide every older line behind it.
		carryTooLong := false

		// take records one line: seg is its head, and when joinCarry is set the
		// carried fragment is its tail. physPos is the line's start within cf.
		// Returns false when the walk should stop.
		take := func(seg []byte, physPos int64, joinCarry bool) bool {
			lineStart := cf.start + physPos
			if since >= 0 && lineStart < since {
				return false
			}

			raw, dropped := seg, false
			if joinCarry {
				switch {
				case carryTooLong || len(seg)+len(carry) > maxLineBytes:
					dropped = true
				case len(carry) > 0:
					raw = make([]byte, 0, len(seg)+len(carry))
					raw = append(raw, seg...)
					raw = append(raw, carry...)
				}
				carry, carryTooLong = nil, false
			}
			if dropped {
				return true
			}

			raw = bytes.TrimRight(raw, "\r")
			if len(raw) > 0 && f.Match(raw) {
				// Stop before a line that would take the page over the byte
				// budget; the newest match is always taken, whatever its size.
				// The walk runs newest-first, so the budget drops the older end
				// of the requested window and firstOffset still marks where
				// paging further back resumes.
				if len(collected) > 0 && collectedBytes+len(raw) > maxResponseBytes {
					return false
				}
				cp := make([]byte, len(raw))
				copy(cp, raw)
				collected = append(collected, entry{line: cp, offset: lineStart})
				collectedBytes += len(raw)
			}
			return true
		}

		for readEnd := physOffset; readEnd > 0 && !stop && len(collected) < count; {
			chunkStart := readEnd - backwardChunkSize
			if chunkStart < 0 {
				chunkStart = 0
			}

			buf, rerr := readChunk(cf.path, chunkStart, readEnd)
			if rerr != nil {
				break
			}

			// Walk the chunk right to left. end is the exclusive end of the
			// segment being built; the one segment reaching len(buf) is the
			// one the carried fragment belongs to.
			end := len(buf)
			for i := end - 1; i >= 0 && !stop && len(collected) < count; i-- {
				if buf[i] != '\n' {
					continue
				}
				stop = !take(buf[i+1:end], chunkStart+int64(i)+1, end == len(buf))
				end = i
			}
			if stop || len(collected) >= count {
				break
			}

			if chunkStart == 0 {
				// The first byte of a file always starts a line, so buf[:end]
				// needs no further bytes to be complete.
				stop = !take(buf[:end], 0, end == len(buf))
				break
			}

			// buf[:end] is the tail of a line that starts below this chunk.
			switch {
			case carryTooLong || len(carry)+end > maxLineBytes:
				carry, carryTooLong = nil, true
			case end > 0:
				joined := make([]byte, 0, end+len(carry))
				joined = append(joined, buf[:end]...)
				joined = append(joined, carry...)
				carry = joined
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

// Search scans the entire chain oldest-to-newest for lines matching query q
// (case-insensitive substring of raw JSON) that also pass filter f.
//
// Returns the logical offsets of up to limit matches, plus the true total match
// count. Offsets and not the matching lines: search is how a client locates a
// match, and it then fetches the text through the ordinary paged read, so
// returning the records here would be up to limit × maxLineBytes of body that
// nothing reads — and the byte budget, charged against text no caller wants,
// would cut the number of reachable matches by the same factor. It is also why
// Search needs no budget of its own: limit offsets are a few tens of kilobytes
// however large the records behind them are.
func (fc *FileChain) Search(q string, limit int, f *Filter, since int64) (offsets []int64, totalMatches int, err error) {
	qLower := []byte(strings.ToLower(q))

	for _, cf := range fc.files {
		fh, ferr := os.Open(cf.path)
		if ferr != nil {
			continue
		}

		scanner := bufio.NewScanner(fh)
		scanner.Buffer(make([]byte, 64*1024), maxLineBytes)

		var physOffset int64
		for scanner.Scan() {
			raw := scanner.Bytes()
			lineLen := int64(len(raw)) + 1
			logicalOffset := cf.start + physOffset

			if since >= 0 && logicalOffset < since {
				physOffset += lineLen
				continue
			}

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
			// The scan carries on past limit, so totalMatches stays the true
			// count for the whole chain and a client can report how many
			// matches exist even though it was handed only the first few.
			if len(offsets) < limit {
				offsets = append(offsets, logicalOffset)
			}
		}
		fh.Close()
	}

	return offsets, totalMatches, nil
}
