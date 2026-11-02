// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

package main

import (
	"bufio"
	"bytes"
	"compress/gzip"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"mime"
	"net/http"
	"os"
	"strings"
	"time"
)

// ── Export ───────────────────────────────────────────────────────────────────
//
// GET /export streams every record of a sink's whole rotation chain that falls
// in a time range, optionally filtered, as JSONL, rebuilt syslog lines or CSV.
// Unlike /lines it has no line or byte cap: nothing is collected, each record
// is written out as it is read, so memory stays flat however much is exported.
// The time range is what bounds it.

// rcvLayout is how minilog writes `rcv`: UTC, whole seconds, fixed width. Two
// timestamps in it compare as strings in time order, which is what lets the
// range check work on the raw line without parsing it.
const rcvLayout = "2006-01-02T15:04:05Z"

type exportFormat struct {
	ext         string
	contentType string
	newWriter   func(*bufio.Writer) recordWriter
}

var exportFormats = map[string]exportFormat{
	"jsonl": {"jsonl", "application/x-ndjson", func(w *bufio.Writer) recordWriter { return &jsonlWriter{w} }},
	"text":  {"log", "text/plain; charset=utf-8", func(w *bufio.Writer) recordWriter { return &textWriter{w} }},
	"csv":   {"csv", "text/csv; charset=utf-8", func(w *bufio.Writer) recordWriter { return newCSVWriter(w) }},
}

// recordWriter writes one JSONL record in an export format. finish is called
// once after the last record, and also when there were none.
type recordWriter interface {
	write(line []byte) error
	finish() error
}

func handleExport(w http.ResponseWriter, r *http.Request, sinkMap map[string]Sink) {
	sink, ok := resolveSink(w, r, sinkMap)
	if !ok {
		return
	}
	q := r.URL.Query()

	formatName := q.Get("format")
	if formatName == "" {
		formatName = "jsonl"
	}
	format, ok := exportFormats[formatName]
	if !ok {
		http.Error(w, "unknown format: "+formatName, http.StatusBadRequest)
		return
	}
	from, err := parseExportTime(q.Get("from"))
	if err != nil {
		http.Error(w, "from: "+err.Error(), http.StatusBadRequest)
		return
	}
	to, err := parseExportTime(q.Get("to"))
	if err != nil {
		http.Error(w, "to: "+err.Error(), http.StatusBadRequest)
		return
	}
	if from != "" && to != "" && from >= to {
		http.Error(w, "from must be before to", http.StatusBadRequest)
		return
	}

	files, err := openGenerations(sink)
	if err != nil {
		http.Error(w, "cannot open log files: "+err.Error(), http.StatusInternalServerError)
		return
	}
	defer closeGenerations(files)

	name := fmt.Sprintf("minilog-%s-%s-%s.%s", fileNamePart(sink.Name),
		fileStamp(from, "start"), fileStamp(to, time.Now().UTC().Format(rcvLayout)), format.ext)
	w.Header().Set("Content-Type", format.contentType)
	w.Header().Set("Content-Disposition", mime.FormatMediaType("attachment", map[string]string{"filename": name}))
	w.WriteHeader(http.StatusOK)

	bw := bufio.NewWriterSize(w, 64*1024)
	out := format.newWriter(bw)
	err = exportRecords(r.Context(), files, from, to, parseFilter(r), out)
	if err == nil {
		err = out.finish()
	}
	if err == nil {
		err = bw.Flush()
	}
	if err != nil {
		// The status line went out with the first byte, so a failure can no
		// longer be reported as one. Breaking the connection makes the browser
		// mark the download failed; returning would end the body cleanly and
		// leave a truncated file that looks complete.
		panic(http.ErrAbortHandler)
	}
}

// exportRecords writes every record in files that is in [from, to) and passes
// f. from and to are in rcvLayout, or empty for an open end.
//
// Every generation is read in full. Skipping the ones whose first and last
// `rcv` lie outside the range would be quicker, but `rcv` is the collector's
// wall clock: a clock stepped back by NTP or by hand puts later records before
// earlier ones, and an export that silently leaves out records is the one
// result it must not have.
func exportRecords(ctx context.Context, files []generation, from, to string, f *Filter, out recordWriter) error {
	filtering := !f.Empty()
	for i, g := range files {
		var r io.Reader = io.NewSectionReader(g.file, 0, g.size)
		if g.compressed {
			zr, err := gzip.NewReader(r)
			if err != nil {
				return fmt.Errorf("%s: %w", g.file.Name(), err)
			}
			r = zr
		}
		// The active file is the last one, and minilog may be partway through
		// writing a record at the size it was opened with. A compressed file
		// is complete: minilog renames it into place only once it is.
		active := i == len(files)-1 && !g.compressed
		n := 0
		err := eachLine(r, active, func(line []byte) error {
			n++
			if n%4096 == 0 && ctx.Err() != nil {
				return ctx.Err()
			}
			if from != "" || to != "" {
				rcv := recordRcv(line)
				if rcv == "" || (from != "" && rcv < from) || (to != "" && rcv >= to) {
					return nil
				}
			}
			if filtering && !f.Match(line) {
				return nil
			}
			return out.write(line)
		})
		if err != nil {
			return err
		}
	}
	return ctx.Err()
}

// parseExportTime turns an RFC 3339 time into rcvLayout, so it can be compared
// with `rcv` as a string. Fractions of a second are dropped, as `rcv` has none.
func parseExportTime(s string) (string, error) {
	if s == "" {
		return "", nil
	}
	t, err := time.Parse(time.RFC3339Nano, s)
	if err != nil {
		return "", errors.New("not an RFC 3339 time: " + s)
	}
	return t.UTC().Format(rcvLayout), nil
}

// recordRcv returns the `rcv` value of a JSONL record, or "" if it has none.
// `rcv` is the first field minilog writes, so it is found at the start of the
// line without parsing the rest. Whitespace after the colon is allowed, as in
// matchStringField: minilog writes none, but JSON written by other tools —
// the demo's generator among them — does.
func recordRcv(line []byte) string {
	const key = `"rcv":`
	i := bytes.Index(line, []byte(key))
	if i < 0 {
		return ""
	}
	rest := bytes.TrimLeft(line[i+len(key):], " \t")
	if len(rest) == 0 || rest[0] != '"' {
		return ""
	}
	rest = rest[1:]
	end := bytes.IndexByte(rest, '"')
	if end < 0 {
		return ""
	}
	return string(rest[:end])
}

// fileStamp formats an rcvLayout time for a file name: no colons, which
// Windows does not allow there.
func fileStamp(t, empty string) string {
	if t == "" {
		t = empty
	}
	return strings.NewReplacer("-", "", ":", "").Replace(t)
}

// fileNamePart keeps a sink name usable in a file name on every platform.
func fileNamePart(s string) string {
	return strings.Map(func(r rune) rune {
		switch {
		case r >= 'a' && r <= 'z', r >= 'A' && r <= 'Z', r >= '0' && r <= '9', r == '-', r == '_', r == '.':
			return r
		}
		return '_'
	}, s)
}

// ── Opening the chain ────────────────────────────────────────────────────────

// generation is one open file of a sink's rotation chain, and the size it had
// when it was opened. Nothing past that size is read: for the active file it is
// still being written.
type generation struct {
	file       *os.File
	size       int64
	info       os.FileInfo
	compressed bool // gzipped by minilog's `compress`; read through gzip
}

// openGenerations opens every non-empty generation of sink, oldest first.
//
// An export holds them all for as long as the download takes, which can be far
// longer than a rotation interval. Opening them one at a time as the export
// reaches them would read the wrong files after a rotation — each name moves
// one generation along — so they are all opened first, and the open handles
// follow their files through any rename that comes later (openShared is what
// lets the server rename them at all on Windows).
//
// A rotation can still land while they are being opened, leaving some handles
// on files that have since moved. That is detected by checking every name still
// leads to the file opened under it, and the whole set is opened again.
func openGenerations(sink Sink) ([]generation, error) {
	const attempts = 5
	for range attempts {
		files, paths, err := tryOpenGenerations(sink)
		if err != nil {
			return nil, err
		}
		if stillNamed(files, paths) {
			return files, nil
		}
		closeGenerations(files)
	}
	return nil, errors.New("the log kept rotating while it was being opened")
}

func tryOpenGenerations(sink Sink) ([]generation, []string, error) {
	var files []generation
	var paths []string
	for _, plain := range chainPaths(sink) {
		// A generation is plain or gzipped. Both exist only briefly, between
		// minilog's rename of the .gz into place and its removal of the plain
		// file (or for good after a crash in between); they hold the same
		// records, and the plain one is read.
		path, compressed := plain, false
		fh, err := openShared(path)
		if errors.Is(err, fs.ErrNotExist) {
			path, compressed = plain+".gz", true
			fh, err = openShared(path)
		}
		if err != nil {
			if errors.Is(err, fs.ErrNotExist) {
				continue
			}
			closeGenerations(files)
			return nil, nil, err
		}
		info, err := fh.Stat()
		if err != nil {
			fh.Close()
			closeGenerations(files)
			return nil, nil, err
		}
		if info.Size() == 0 {
			fh.Close()
			continue
		}
		files = append(files, generation{file: fh, size: info.Size(), info: info, compressed: compressed})
		paths = append(paths, path)
	}
	return files, paths, nil
}

// stillNamed reports whether each path still names the file opened from it.
func stillNamed(files []generation, paths []string) bool {
	for i, g := range files {
		info, err := os.Stat(paths[i])
		if err != nil || !os.SameFile(info, g.info) {
			return false
		}
	}
	return true
}

func closeGenerations(files []generation) {
	for _, g := range files {
		g.file.Close()
	}
}

// eachLine calls fn with every line of r, without its '\n'. Empty lines are
// skipped, and so is a line longer than maxLineBytes, as the other read paths
// do. With dropPartial, a final line that has no '\n' is skipped too: it is a
// record the writer has not finished.
func eachLine(r io.Reader, dropPartial bool, fn func([]byte) error) error {
	br := bufio.NewReaderSize(r, 64*1024)
	var long []byte
	tooLong := false
	for {
		chunk, err := br.ReadSlice('\n')
		if errors.Is(err, bufio.ErrBufferFull) {
			// A line longer than the buffer: gather it, up to the limit.
			if !tooLong {
				long = append(long, chunk...)
				tooLong = len(long) > maxLineBytes
			}
			continue
		}
		line := chunk
		if long != nil || tooLong {
			if !tooLong {
				long = append(long, chunk...)
				tooLong = len(long) > maxLineBytes+1
			}
			line = long
		}
		terminated := len(line) > 0 && line[len(line)-1] == '\n'
		line = bytes.TrimSuffix(line, []byte("\n"))
		skip := tooLong || len(line) == 0 || (!terminated && dropPartial)
		long, tooLong = nil, false

		if !skip {
			if ferr := fn(line); ferr != nil {
				return ferr
			}
		}
		if errors.Is(err, io.EOF) {
			return nil
		}
		if err != nil {
			return err
		}
	}
}

// ── Formats ──────────────────────────────────────────────────────────────────

// jsonlWriter writes the stored records unchanged.
type jsonlWriter struct{ w *bufio.Writer }

func (j *jsonlWriter) write(line []byte) error {
	if _, err := j.w.Write(line); err != nil {
		return err
	}
	return j.w.WriteByte('\n')
}

func (j *jsonlWriter) finish() error { return nil }

// textWriter rebuilds each record as the line the text sink would have
// written; see syslogLine for how close that gets.
type textWriter struct{ w *bufio.Writer }

func (t *textWriter) write(line []byte) error {
	var rec record
	if json.Unmarshal(line, &rec) != nil {
		return nil // not a record minilog wrote; there is nothing to rebuild
	}
	if _, err := t.w.WriteString(escapeControlChars(syslogLine(&rec))); err != nil {
		return err
	}
	return t.w.WriteByte('\n')
}

func (t *textWriter) finish() error { return nil }

// csvColumns are the JSONL fields, in the order minilog writes them.
var csvColumns = []string{
	"rcv", "src", "proto", "facility", "severity", "hostname", "app", "pid", "msgid", "msg_time", "message",
}

// csvWriter writes RFC 4180 CSV: a header row, one row per record, CRLF
// between rows, with a byte order mark so that Excel reads the file as UTF-8
// rather than in the local code page.
//
// encoding/csv is not used because with CRLF row ends it also turns a newline
// inside a quoted field into CRLF, which changes the message.
type csvWriter struct {
	out     *bufio.Writer
	started bool
}

func newCSVWriter(out *bufio.Writer) *csvWriter {
	return &csvWriter{out: out}
}

func (c *csvWriter) start() error {
	if c.started {
		return nil
	}
	c.started = true
	if _, err := c.out.WriteString("\ufeff"); err != nil {
		return err
	}
	return c.writeRow(csvColumns)
}

func (c *csvWriter) writeRow(row []string) error {
	for i, v := range row {
		if i > 0 {
			c.out.WriteByte(',')
		}
		if strings.ContainsAny(v, ",\"\r\n") || strings.HasPrefix(v, " ") || strings.HasSuffix(v, " ") {
			c.out.WriteByte('"')
			c.out.WriteString(strings.ReplaceAll(v, `"`, `""`))
			c.out.WriteByte('"')
		} else {
			c.out.WriteString(v)
		}
	}
	_, err := c.out.WriteString("\r\n")
	return err
}

func (c *csvWriter) write(line []byte) error {
	var rec record
	if json.Unmarshal(line, &rec) != nil {
		return nil
	}
	if err := c.start(); err != nil {
		return err
	}
	row := make([]string, len(csvColumns))
	for i, raw := range rec.fields() {
		v, _ := fieldString(raw)
		row[i] = defuseFormula(v)
	}
	return c.writeRow(row)
}

func (c *csvWriter) finish() error {
	return c.start()
}

// defuseFormula keeps a spreadsheet from running a cell as a formula. Every
// value comes from whoever sent the datagram, so a message beginning with "="
// is someone else's formula, run when the export is opened. A leading
// apostrophe makes Excel and its relatives treat the cell as text.
func defuseFormula(v string) string {
	if v != "" && strings.ContainsRune("=+-@\t\r", rune(v[0])) {
		return "'" + v
	}
	return v
}

// record is a JSONL record with every field kept raw: fieldString decides how
// each reads, so a field that is a number in some foreign file, or null, does
// not fail the whole record.
type record struct {
	Rcv      json.RawMessage `json:"rcv"`
	Src      json.RawMessage `json:"src"`
	Proto    json.RawMessage `json:"proto"`
	Facility json.RawMessage `json:"facility"`
	Severity json.RawMessage `json:"severity"`
	Hostname json.RawMessage `json:"hostname"`
	App      json.RawMessage `json:"app"`
	Pid      json.RawMessage `json:"pid"`
	Msgid    json.RawMessage `json:"msgid"`
	MsgTime  json.RawMessage `json:"msg_time"`
	Message  json.RawMessage `json:"message"`
}

// fields returns the fields in csvColumns order.
func (r *record) fields() []json.RawMessage {
	return []json.RawMessage{
		r.Rcv, r.Src, r.Proto, r.Facility, r.Severity, r.Hostname, r.App, r.Pid, r.Msgid, r.MsgTime, r.Message,
	}
}

// fieldString returns a field's value as text, and whether it had one: false
// for null and for a field that is not there.
func fieldString(raw json.RawMessage) (string, bool) {
	if len(raw) == 0 || string(raw) == "null" {
		return "", false
	}
	var s string
	if json.Unmarshal(raw, &s) == nil {
		return s, true
	}
	return string(raw), true
}

// syslogLine rebuilds the datagram a record was parsed from, as far as the
// record allows (src/server/parser/syslog_parser.cpp is the other direction):
//
//   - UNKNOWN: message is the whole datagram, so this is exact.
//   - RFC 3164: <PRI>timestamp hostname app[pid]: message. The sender's own
//     spacing is not stored, so whitespace in the header can differ.
//   - RFC 5424: <PRI>1 timestamp hostname app procid msgid message, with "-"
//     for a nil field; structured data is still the start of message.
//
// Invalid UTF-8 was replaced with U+FFFD before the record was written and
// cannot be brought back. The result is escaped by the caller, as the text
// sink escapes the datagram.
func syslogLine(r *record) string {
	msg, _ := fieldString(r.Message)
	proto, _ := fieldString(r.Proto)

	pri := ""
	fac, _ := fieldString(r.Facility)
	sev, _ := fieldString(r.Severity)
	f, fok := facilityAliases[strings.ToLower(fac)]
	s, sok := severityAliases[strings.ToLower(sev)]
	if fok && sok {
		pri = fmt.Sprintf("<%d>", f*8+s)
	}

	ts, _ := fieldString(r.MsgTime)
	host, hasHost := fieldString(r.Hostname)
	app, hasApp := fieldString(r.App)
	pid, hasPid := fieldString(r.Pid)

	switch proto {
	case "RFC5424":
		nilable := func(raw json.RawMessage) string {
			if v, ok := fieldString(raw); ok {
				return v
			}
			return "-"
		}
		line := pri + "1 " + strings.Join([]string{
			nilable(r.MsgTime), nilable(r.Hostname), nilable(r.App), nilable(r.Pid), nilable(r.Msgid),
		}, " ")
		if msg != "" {
			line += " " + msg
		}
		return line
	case "RFC3164":
		line := pri + ts
		if !hasHost {
			return line
		}
		line += " " + host + " "
		if hasApp || hasPid {
			line += app
			if hasPid {
				line += "[" + pid + "]"
			}
			line += ": "
		}
		return line + msg
	default:
		return msg
	}
}

// escapeControlChars is the text sink's escaping, so that one record is one
// line and a terminal paging the file cannot be driven by it. It must match
// escapeControlChars in src/server/output/log_file.cpp, which explains each
// rule; the README's "Text file" section lists them.
func escapeControlChars(s string) string {
	var b strings.Builder
	b.Grow(len(s))
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c == 0xC2 && i+1 < len(s) && s[i+1] >= 0x80 && s[i+1] <= 0x9F {
			fmt.Fprintf(&b, `\u%04X`, s[i+1])
			i++
			continue
		}
		switch {
		case c == '\\':
			b.WriteString(`\\`)
		case c == '\n':
			b.WriteString(`\n`)
		case c == '\r':
			b.WriteString(`\r`)
		case (c < 0x20 && c != '\t') || c == 0x7F:
			fmt.Fprintf(&b, `\x%02X`, c)
		default:
			b.WriteByte(c)
		}
	}
	return b.String()
}
