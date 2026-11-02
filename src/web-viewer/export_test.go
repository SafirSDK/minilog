// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"mime"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// testRecord builds a JSONL record in minilog's field order. A nil field is
// written as null.
type testRecord struct {
	Rcv      *string `json:"rcv"`
	Src      *string `json:"src"`
	Proto    *string `json:"proto"`
	Facility *string `json:"facility"`
	Severity *string `json:"severity"`
	Hostname *string `json:"hostname"`
	App      *string `json:"app"`
	Pid      *string `json:"pid"`
	Msgid    *string `json:"msgid"`
	MsgTime  *string `json:"msg_time"`
	Message  *string `json:"message"`
}

func sp(s string) *string { return &s }

func (r testRecord) line(t *testing.T) string {
	t.Helper()
	b, err := json.Marshal(r)
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

// simple returns an RFC 3164 record received at rcv, with message msg.
func simple(rcv, sev, msg string) testRecord {
	return testRecord{
		Rcv: sp(rcv), Src: sp("10.0.0.1"), Proto: sp("RFC3164"), Facility: sp("daemon"), Severity: sp(sev),
		Hostname: sp("host"), App: sp("app"), Pid: sp("1"), MsgTime: sp("Nov  2 10:00:00"), Message: sp(msg),
	}
}

// exportBody GETs /export with query and returns the body, failing on any
// status other than 200.
func exportBody(t *testing.T, sinks []Sink, query string) (string, http.Header) {
	t.Helper()
	ts := newTestServer(t, sinks)
	defer ts.Close()
	resp := get(t, ts, "/export?"+query)
	defer resp.Body.Close()
	body, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatalf("read body: %v", err)
	}
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("status %d: %s", resp.StatusCode, body)
	}
	return string(body), resp.Header
}

func exportStatus(t *testing.T, sinks []Sink, query string) int {
	t.Helper()
	ts := newTestServer(t, sinks)
	defer ts.Close()
	resp := get(t, ts, "/export?"+query)
	resp.Body.Close()
	return resp.StatusCode
}

func TestExport_JSONL_IsTheStoredLinesUnchanged(t *testing.T) {
	dir := t.TempDir()
	lines := []string{
		simple("2026-11-02T10:00:00Z", "INFO", "one").line(t),
		simple("2026-11-02T10:00:01Z", "ERROR", "two \"quoted\" \\ é").line(t),
	}
	sink := makeSink(t, dir, "main", lines)

	body, hdr := exportBody(t, []Sink{sink}, "sink=main&format=jsonl")

	if want := strings.Join(lines, "\n") + "\n"; body != want {
		t.Errorf("body = %q, want %q", body, want)
	}
	if ct := hdr.Get("Content-Type"); ct != "application/x-ndjson" {
		t.Errorf("Content-Type = %q", ct)
	}
	_, params, err := mime.ParseMediaType(hdr.Get("Content-Disposition"))
	if err != nil {
		t.Fatalf("Content-Disposition: %v", err)
	}
	if name := params["filename"]; !strings.HasPrefix(name, "minilog-main-start-") || !strings.HasSuffix(name, ".jsonl") {
		t.Errorf("filename = %q", name)
	}
}

func TestExport_DefaultFormatIsJSONL(t *testing.T) {
	dir := t.TempDir()
	line := simple("2026-11-02T10:00:00Z", "INFO", "one").line(t)
	sink := makeSink(t, dir, "main", []string{line})

	body, _ := exportBody(t, []Sink{sink}, "sink=main")
	if body != line+"\n" {
		t.Errorf("body = %q", body)
	}
}

func TestExport_ReadsEveryGenerationOldestFirst(t *testing.T) {
	dir := t.TempDir()
	sink := makeSink(t, dir, "main", []string{simple("2026-11-02T10:00:02Z", "INFO", "active").line(t)})
	for n, msg := range map[int]string{1: "gen1", 2: "gen2"} {
		rcv := fmt.Sprintf("2026-11-02T10:00:0%dZ", 2-n)
		p := filepath.Join(dir, fmt.Sprintf("main.%d.jsonl", n))
		if err := os.WriteFile(p, []byte(simple(rcv, "INFO", msg).line(t)+"\n"), 0o600); err != nil {
			t.Fatal(err)
		}
	}

	body, _ := exportBody(t, []Sink{sink}, "sink=main&format=text")

	got := strings.Split(strings.TrimSuffix(body, "\n"), "\n")
	if len(got) != 3 || !strings.HasSuffix(got[0], "gen2") || !strings.HasSuffix(got[1], "gen1") ||
		!strings.HasSuffix(got[2], "active") {
		t.Errorf("lines = %q", got)
	}
}

func TestExport_TimeRange_FromInclusiveToExclusive(t *testing.T) {
	dir := t.TempDir()
	var lines []string
	for s := 0; s < 5; s++ {
		lines = append(lines, simple(fmt.Sprintf("2026-11-02T10:00:0%dZ", s), "INFO", fmt.Sprintf("s%d", s)).line(t))
	}
	sink := makeSink(t, dir, "main", lines)

	cases := []struct {
		query string
		want  []string
	}{
		{"from=2026-11-02T10:00:01Z&to=2026-11-02T10:00:03Z", []string{"s1", "s2"}},
		{"from=2026-11-02T10:00:03Z", []string{"s3", "s4"}},
		{"to=2026-11-02T10:00:02Z", []string{"s0", "s1"}},
		// A browser sends milliseconds, and a time zone other than UTC.
		{"from=2026-11-02T11:00:01.500%2B01:00", []string{"s1", "s2", "s3", "s4"}},
		{"", []string{"s0", "s1", "s2", "s3", "s4"}},
	}
	for _, c := range cases {
		body, _ := exportBody(t, []Sink{sink}, "sink=main&format=csv&"+c.query)
		var got []string
		for _, row := range strings.Split(body, "\r\n")[1:] {
			if row != "" {
				got = append(got, row[strings.LastIndexByte(row, ',')+1:])
			}
		}
		if strings.Join(got, ",") != strings.Join(c.want, ",") {
			t.Errorf("%s: got %q, want %q", c.query, got, c.want)
		}
	}
}

func TestExport_TimeRange_ReadsSpacedJSON(t *testing.T) {
	sink := makeSink(t, t.TempDir(), "main", []string{
		`{"rcv": "2026-11-02T10:00:00Z", "message": "early"}`,
		`{"rcv":  "2026-11-02T10:00:05Z", "message": "late"}`,
	})
	body, _ := exportBody(t, []Sink{sink}, "sink=main&format=jsonl&from=2026-11-02T10:00:01Z")
	if !strings.Contains(body, "late") || strings.Contains(body, "early") {
		t.Errorf("body = %q", body)
	}
}

func TestExport_BadRequests(t *testing.T) {
	dir := t.TempDir()
	sink := makeSink(t, dir, "main", []string{simple("2026-11-02T10:00:00Z", "INFO", "x").line(t)})

	for _, q := range []string{
		"sink=nope",
		"sink=main&format=xml",
		"sink=main&from=yesterday",
		"sink=main&to=2026-11-02",
		"sink=main&from=2026-11-02T10:00:00Z&to=2026-11-02T10:00:00Z",
		"sink=main&from=2026-11-02T11:00:00Z&to=2026-11-02T10:00:00Z",
	} {
		if code := exportStatus(t, []Sink{sink}, q); code != http.StatusBadRequest {
			t.Errorf("%s: status %d, want 400", q, code)
		}
	}
}

func TestExport_FiltersApplyOnlyWhenSent(t *testing.T) {
	dir := t.TempDir()
	sink := makeSink(t, dir, "main", []string{
		simple("2026-11-02T10:00:00Z", "INFO", "keep").line(t),
		simple("2026-11-02T10:00:01Z", "DEBUG", "noise").line(t),
	})

	all, _ := exportBody(t, []Sink{sink}, "sink=main&format=text")
	if !strings.Contains(all, "keep") || !strings.Contains(all, "noise") {
		t.Errorf("unfiltered export = %q", all)
	}
	filtered, _ := exportBody(t, []Sink{sink}, "sink=main&format=text&exc=noise")
	if !strings.Contains(filtered, "keep") || strings.Contains(filtered, "noise") {
		t.Errorf("filtered export = %q", filtered)
	}
}

func TestExport_ActiveFilePartialLastLineIsLeftOut(t *testing.T) {
	dir := t.TempDir()
	whole := simple("2026-11-02T10:00:00Z", "INFO", "whole").line(t)
	p := filepath.Join(dir, "main.jsonl")
	if err := os.WriteFile(p, []byte(whole+"\n"+`{"rcv":"2026-11-02T10:00:01Z","src`), 0o600); err != nil {
		t.Fatal(err)
	}
	sink := Sink{Name: "main", Path: p, MaxFiles: 5}

	body, _ := exportBody(t, []Sink{sink}, "sink=main&format=jsonl")
	if body != whole+"\n" {
		t.Errorf("body = %q", body)
	}
}

func TestExport_OverlongLineIsSkipped(t *testing.T) {
	dir := t.TempDir()
	long := simple("2026-11-02T10:00:00Z", "INFO", strings.Repeat("x", maxLineBytes)).line(t)
	after := simple("2026-11-02T10:00:01Z", "INFO", "after").line(t)
	sink := makeSink(t, dir, "main", []string{long, after})

	body, _ := exportBody(t, []Sink{sink}, "sink=main&format=jsonl")
	if body != after+"\n" {
		t.Errorf("body has %d bytes, want just the line after the overlong one", len(body))
	}
}

// ── Text format ──────────────────────────────────────────────────────────────

func TestExport_Text_RebuildsEachProtocol(t *testing.T) {
	cases := []struct {
		name string
		rec  testRecord
		want string
	}{
		{"malformed is the datagram", testRecord{
			Rcv: sp("2026-11-02T10:00:00Z"), Src: sp("10.0.0.1"), Proto: sp("UNKNOWN"),
			Message: sp("<999>whatever the sender sent"),
		}, "<999>whatever the sender sent"},
		{"3164 with app and pid", simple("2026-11-02T10:00:00Z", "ERROR", "disk full"),
			"<27>Nov  2 10:00:00 host app[1]: disk full"},
		{"3164 without a tag", testRecord{
			Rcv: sp("2026-11-02T10:00:00Z"), Proto: sp("RFC3164"), Facility: sp("user"), Severity: sp("NOTICE"),
			Hostname: sp("host"), MsgTime: sp("Nov  2 10:00:00"), Message: sp("no tag here"),
		}, "<13>Nov  2 10:00:00 host no tag here"},
		{"3164 with app only", testRecord{
			Rcv: sp("2026-11-02T10:00:00Z"), Proto: sp("RFC3164"), Facility: sp("local7"), Severity: sp("DEBUG"),
			Hostname: sp("h"), App: sp("cron"), MsgTime: sp("Nov  2 10:00:00"), Message: sp("tick"),
		}, "<191>Nov  2 10:00:00 h cron: tick"},
		{"3164 without a hostname", testRecord{
			Rcv: sp("2026-11-02T10:00:00Z"), Proto: sp("RFC3164"), Facility: sp("kern"), Severity: sp("EMERGENCY"),
			MsgTime: sp("Nov  2 10:00:00"),
		}, "<0>Nov  2 10:00:00"},
		{"5424 with nil fields", testRecord{
			Rcv: sp("2026-11-02T10:00:00Z"), Proto: sp("RFC5424"), Facility: sp("auth"), Severity: sp("WARNING"),
			Hostname: sp("h"), App: sp("sshd"), MsgTime: sp("2026-11-02T10:00:00Z"),
			Message: sp(`[x@1 a="b"] login`),
		}, `<36>1 2026-11-02T10:00:00Z h sshd - - [x@1 a="b"] login`},
		{"5424 all nil", testRecord{
			Rcv: sp("2026-11-02T10:00:00Z"), Proto: sp("RFC5424"), Facility: sp("clock2"), Severity: sp("INFO"),
			Message: sp("- hi"),
		}, "<126>1 - - - - - - hi"},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			sink := makeSink(t, t.TempDir(), "main", []string{c.rec.line(t)})
			body, hdr := exportBody(t, []Sink{sink}, "sink=main&format=text")
			if body != c.want+"\n" {
				t.Errorf("got %q, want %q", body, c.want+"\n")
			}
			if ct := hdr.Get("Content-Type"); ct != "text/plain; charset=utf-8" {
				t.Errorf("Content-Type = %q", ct)
			}
		})
	}
}

func TestExport_Text_EscapesLikeTheTextSink(t *testing.T) {
	rec := testRecord{
		Rcv: sp("2026-11-02T10:00:00Z"), Proto: sp("UNKNOWN"),
		Message: sp("a\nb\rc\\d\te\x1b[2J\x00\x7f\u009b\u009d 日本 café"),
	}
	sink := makeSink(t, t.TempDir(), "main", []string{rec.line(t)})

	body, _ := exportBody(t, []Sink{sink}, "sink=main&format=text")

	want := `a\nb\rc\\d` + "\t" + `e\x1B[2J\x00\x7F\u009B\u009D ` + "日本 café\n"
	if body != want {
		t.Errorf("got  %q\nwant %q", body, want)
	}
}

func TestEscapeControlChars_LeavesContinuationBytesAlone(t *testing.T) {
	// U+65E5 is E6 97 A5: 0x97 is in the C1 byte range, but only C2 80-9F is C1.
	if got := escapeControlChars("日"); got != "日" {
		t.Errorf("got %q", got)
	}
}

// ── CSV format ───────────────────────────────────────────────────────────────

func TestExport_CSV_HeaderBOMQuotingAndNulls(t *testing.T) {
	rec := testRecord{
		Rcv: sp("2026-11-02T10:00:00Z"), Src: sp("10.0.0.1"), Proto: sp("RFC3164"), Facility: sp("daemon"),
		Severity: sp("INFO"), Hostname: sp("host"), App: nil, Pid: nil, MsgTime: sp("Nov  2 10:00:00"),
		Message: sp("a, \"b\"\nc"),
	}
	sink := makeSink(t, t.TempDir(), "main", []string{rec.line(t)})

	body, hdr := exportBody(t, []Sink{sink}, "sink=main&format=csv")

	want := "\ufeffrcv,src,proto,facility,severity,hostname,app,pid,msgid,msg_time,message\r\n" +
		"2026-11-02T10:00:00Z,10.0.0.1,RFC3164,daemon,INFO,host,,,,Nov  2 10:00:00,\"a, \"\"b\"\"\nc\"\r\n"
	if body != want {
		t.Errorf("got  %q\nwant %q", body, want)
	}
	if ct := hdr.Get("Content-Type"); ct != "text/csv; charset=utf-8" {
		t.Errorf("Content-Type = %q", ct)
	}
}

func TestExport_CSV_DefusesFormulas(t *testing.T) {
	for _, msg := range []string{"=HYPERLINK(\"x\")", "+1", "-1+1", "@SUM(A1)", "\tx", "\rx"} {
		rec := simple("2026-11-02T10:00:00Z", "INFO", msg)
		sink := makeSink(t, t.TempDir(), "main", []string{rec.line(t)})
		body, _ := exportBody(t, []Sink{sink}, "sink=main&format=csv")
		row := strings.SplitN(body, "\r\n", 2)[1]
		if !strings.Contains(row, ",'"+strings.TrimPrefix(msg, "\"")) && !strings.Contains(row, "\"'") {
			t.Errorf("%q not defused: %q", msg, row)
		}
	}
	if got := defuseFormula("plain = text"); got != "plain = text" {
		t.Errorf("defused a value that does not start a formula: %q", got)
	}
}

func TestExport_CSV_EmptyExportStillHasHeader(t *testing.T) {
	sink := makeSink(t, t.TempDir(), "main", []string{simple("2026-11-02T10:00:00Z", "INFO", "x").line(t)})
	body, _ := exportBody(t, []Sink{sink}, "sink=main&format=csv&from=2030-01-01T00:00:00Z")
	if body != "\ufeffrcv,src,proto,facility,severity,hostname,app,pid,msgid,msg_time,message\r\n" {
		t.Errorf("got %q", body)
	}
}

// ── Rotation and cancellation ────────────────────────────────────────────────

// Once open, the export reads the files it opened even if the server rotates
// every name along by one before it gets to them.
func TestExport_RotationAfterOpeningDoesNotChangeWhatIsRead(t *testing.T) {
	dir := t.TempDir()
	sink := makeSink(t, dir, "main", []string{simple("2026-11-02T10:00:01Z", "INFO", "active").line(t)})
	gen1 := filepath.Join(dir, "main.1.jsonl")
	if err := os.WriteFile(gen1, []byte(simple("2026-11-02T10:00:00Z", "INFO", "old").line(t)+"\n"), 0o600); err != nil {
		t.Fatal(err)
	}

	files, err := openGenerations(sink)
	if err != nil {
		t.Fatal(err)
	}
	defer closeGenerations(files)

	// Rotate: .1 → .2, active → .1, and a new active file.
	if err := os.Rename(gen1, filepath.Join(dir, "main.2.jsonl")); err != nil {
		t.Fatal(err)
	}
	if err := os.Rename(sink.Path, gen1); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(sink.Path, []byte(simple("2026-11-02T10:00:02Z", "INFO", "new").line(t)+"\n"), 0o600); err != nil {
		t.Fatal(err)
	}

	var buf bytes.Buffer
	bw := bufio.NewWriter(&buf)
	if err := exportRecords(context.Background(), files, "", "", &Filter{}, &textWriter{bw}); err != nil {
		t.Fatal(err)
	}
	bw.Flush()
	got := buf.String()
	if !strings.Contains(got, "old\n") || !strings.Contains(got, "active\n") || strings.Contains(got, "new") ||
		strings.Index(got, "old") > strings.Index(got, "active") {
		t.Errorf("got %q", got)
	}
}

func TestExport_StopsWhenTheRequestIsCancelled(t *testing.T) {
	var lines []string
	for i := 0; i < 10000; i++ {
		lines = append(lines, simple("2026-11-02T10:00:00Z", "INFO", fmt.Sprint(i)).line(t))
	}
	sink := makeSink(t, t.TempDir(), "main", lines)
	files, err := openGenerations(sink)
	if err != nil {
		t.Fatal(err)
	}
	defer closeGenerations(files)

	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	bw := bufio.NewWriter(io.Discard)
	if err := exportRecords(ctx, files, "", "", &Filter{}, &jsonlWriter{bw}); err == nil {
		t.Error("a cancelled export finished without an error")
	}
}
