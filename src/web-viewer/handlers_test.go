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
	"testing"
)

// newTestServer registers handlers with a sink pointing at dir/syslog.jsonl
// and returns an httptest.Server. The caller must call ts.Close().
func newTestServer(t *testing.T, sinks []Sink) *httptest.Server {
	t.Helper()
	mux := http.NewServeMux()
	registerHandlers(mux, sinks)
	return httptest.NewServer(mux)
}

// makeSink creates a Sink whose jsonl file lives in dir.
func makeSink(t *testing.T, dir, name string, lines []string) Sink {
	t.Helper()
	p := filepath.Join(dir, name+".jsonl")
	var sb strings.Builder
	for _, l := range lines {
		sb.WriteString(l)
		sb.WriteByte('\n')
	}
	if err := os.WriteFile(p, []byte(sb.String()), 0o600); err != nil {
		t.Fatalf("makeSink: %v", err)
	}
	return Sink{Name: name, Path: p, MaxFiles: 5}
}

// get issues a GET request to ts and returns the response.
func get(t *testing.T, ts *httptest.Server, path string) *http.Response {
	t.Helper()
	resp, err := http.Get(ts.URL + path)
	if err != nil {
		t.Fatalf("GET %s: %v", path, err)
	}
	return resp
}

// decodeJSON decodes the JSON body of resp into v.
func decodeJSON(t *testing.T, resp *http.Response, v any) {
	t.Helper()
	defer resp.Body.Close()
	if err := json.NewDecoder(resp.Body).Decode(v); err != nil {
		t.Fatalf("decodeJSON: %v", err)
	}
}

// ── /version ──────────────────────────────────────────────────────────────────

func TestHandler_Version_ReturnsVersionString(t *testing.T) {
	ts := newTestServer(t, nil)
	defer ts.Close()

	resp := get(t, ts, "/version")
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("status: want 200, got %d", resp.StatusCode)
	}
	var result map[string]string
	decodeJSON(t, resp, &result)
	if result["version"] == "" {
		t.Error("version should not be empty")
	}
}

func TestHandler_Version_ContentType_IsJSON(t *testing.T) {
	ts := newTestServer(t, nil)
	defer ts.Close()

	resp := get(t, ts, "/version")
	resp.Body.Close()
	ct := resp.Header.Get("Content-Type")
	if !strings.Contains(ct, "application/json") {
		t.Errorf("Content-Type: want application/json, got %q", ct)
	}
}

// ── /sinks ────────────────────────────────────────────────────────────────────

func TestHandler_Sinks_ReturnsAllSinks(t *testing.T) {
	dir := t.TempDir()
	sinks := []Sink{
		makeSink(t, dir, "main", nil),
		makeSink(t, dir, "auth", nil),
	}
	ts := newTestServer(t, sinks)
	defer ts.Close()

	resp := get(t, ts, "/sinks")
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("status: want 200, got %d", resp.StatusCode)
	}
	var result []struct {
		Name string `json:"name"`
	}
	decodeJSON(t, resp, &result)
	if len(result) != 2 {
		t.Fatalf("want 2 sinks, got %d", len(result))
	}
	if result[0].Name != "main" || result[1].Name != "auth" {
		t.Errorf("names: want [main auth], got [%s %s]", result[0].Name, result[1].Name)
	}
}

// ── /lines ────────────────────────────────────────────────────────────────────

type linesResponse struct {
	Lines       []string `json:"lines"`
	Offsets     []int64  `json:"offsets"`
	FirstOffset int64    `json:"first_offset"`
	NextOffset  int64    `json:"next_offset"`
	TailOffset  int64    `json:"tail_offset"`
}

func TestHandler_Lines_Tail_ReturnsLastLines(t *testing.T) {
	dir := t.TempDir()
	var lines []string
	for i := 1; i <= 5; i++ {
		lines = append(lines, makeLine(fmt.Sprintf("msg%d", i), "info", "daemon"))
	}
	sink := makeSink(t, dir, "main", lines)
	ts := newTestServer(t, []Sink{sink})
	defer ts.Close()

	resp := get(t, ts, "/lines?sink=main&tail=true&count=3")
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("status: want 200, got %d", resp.StatusCode)
	}
	var result linesResponse
	decodeJSON(t, resp, &result)
	if len(result.Lines) != 3 {
		t.Fatalf("want 3 lines, got %d", len(result.Lines))
	}
	if !strings.Contains(result.Lines[2], "msg5") {
		t.Errorf("last line should be msg5, got %q", result.Lines[2])
	}
}

func TestHandler_Lines_Forward_ReturnsFromOffset(t *testing.T) {
	dir := t.TempDir()
	l0 := makeLine("first", "info", "daemon")
	l1 := makeLine("second", "info", "daemon")
	sink := makeSink(t, dir, "main", []string{l0, l1})
	ts := newTestServer(t, []Sink{sink})
	defer ts.Close()

	off := int64(len(l0) + 1)
	resp := get(t, ts, fmt.Sprintf("/lines?sink=main&offset=%d&dir=forward&count=10", off))
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("status: want 200, got %d", resp.StatusCode)
	}
	var result linesResponse
	decodeJSON(t, resp, &result)
	if len(result.Lines) != 1 || !strings.Contains(result.Lines[0], "second") {
		t.Errorf("want [second], got %v", result.Lines)
	}
}

func TestHandler_Lines_Backward_ReturnsBeforeOffset(t *testing.T) {
	dir := t.TempDir()
	l0 := makeLine("first", "info", "daemon")
	l1 := makeLine("second", "info", "daemon")
	sink := makeSink(t, dir, "main", []string{l0, l1})
	ts := newTestServer(t, []Sink{sink})
	defer ts.Close()

	off := int64(len(l0) + 1)
	resp := get(t, ts, fmt.Sprintf("/lines?sink=main&offset=%d&dir=backward&count=10", off))
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("status: want 200, got %d", resp.StatusCode)
	}
	var result linesResponse
	decodeJSON(t, resp, &result)
	if len(result.Lines) != 1 || !strings.Contains(result.Lines[0], "first") {
		t.Errorf("want [first], got %v", result.Lines)
	}
}

func TestHandler_Lines_SeverityFilter(t *testing.T) {
	dir := t.TempDir()
	sink := makeSink(t, dir, "main", []string{
		makeLine("keep", "info", "daemon"),
		makeLine("drop", "error", "daemon"),
	})
	ts := newTestServer(t, []Sink{sink})
	defer ts.Close()

	resp := get(t, ts, "/lines?sink=main&tail=true&count=10&sev=info")
	var result linesResponse
	decodeJSON(t, resp, &result)
	if len(result.Lines) != 1 || !strings.Contains(result.Lines[0], "keep") {
		t.Errorf("want [keep], got %v", result.Lines)
	}
}

func TestHandler_Lines_ExcludeFilter(t *testing.T) {
	dir := t.TempDir()
	sink := makeSink(t, dir, "main", []string{
		makeLine("keep this", "info", "daemon"),
		makeLine("drop this", "info", "daemon"),
	})
	ts := newTestServer(t, []Sink{sink})
	defer ts.Close()

	resp := get(t, ts, "/lines?sink=main&tail=true&count=10&exc=drop")
	var result linesResponse
	decodeJSON(t, resp, &result)
	if len(result.Lines) != 1 || !strings.Contains(result.Lines[0], "keep") {
		t.Errorf("want [keep this], got %v", result.Lines)
	}
}

func TestHandler_Lines_IncludeFilter(t *testing.T) {
	dir := t.TempDir()
	sink := makeSink(t, dir, "main", []string{
		makeLine("target line", "info", "daemon"),
		makeLine("other line", "info", "daemon"),
	})
	ts := newTestServer(t, []Sink{sink})
	defer ts.Close()

	resp := get(t, ts, "/lines?sink=main&tail=true&count=10&inc=target")
	var result linesResponse
	decodeJSON(t, resp, &result)
	if len(result.Lines) != 1 || !strings.Contains(result.Lines[0], "target") {
		t.Errorf("want [target line], got %v", result.Lines)
	}
}

func TestHandler_Lines_UnknownSink_Returns400(t *testing.T) {
	dir := t.TempDir()
	sink := makeSink(t, dir, "main", nil)
	ts := newTestServer(t, []Sink{sink})
	defer ts.Close()

	resp := get(t, ts, "/lines?sink=nonexistent")
	resp.Body.Close()
	if resp.StatusCode != http.StatusBadRequest {
		t.Errorf("want 400, got %d", resp.StatusCode)
	}
}

func TestHandler_Lines_EmptyLinesNotNull(t *testing.T) {
	// When no lines match, the response must have [] not null for lines/offsets.
	dir := t.TempDir()
	sink := makeSink(t, dir, "main", []string{makeLine("msg", "debug", "daemon")})
	ts := newTestServer(t, []Sink{sink})
	defer ts.Close()

	resp := get(t, ts, "/lines?sink=main&tail=true&count=10&sev=info")
	var result linesResponse
	decodeJSON(t, resp, &result)
	if result.Lines == nil {
		t.Error("lines should be [] not null")
	}
	if result.Offsets == nil {
		t.Error("offsets should be [] not null")
	}
}

func TestHandler_Lines_TailOffsetAlwaysPresent(t *testing.T) {
	dir := t.TempDir()
	sink := makeSink(t, dir, "main", []string{makeLine("msg", "info", "daemon")})
	ts := newTestServer(t, []Sink{sink})
	defer ts.Close()

	resp := get(t, ts, "/lines?sink=main&tail=true&count=10")
	var result linesResponse
	decodeJSON(t, resp, &result)
	if result.TailOffset == 0 {
		t.Error("tail_offset should be non-zero when file has content")
	}
}

// ── /search ───────────────────────────────────────────────────────────────────

type searchResponse struct {
	Results []struct {
		Line   string `json:"line"`
		Offset int64  `json:"offset"`
	} `json:"results"`
	TotalMatches int `json:"total_matches"`
}

func TestHandler_Search_FindsMatches(t *testing.T) {
	dir := t.TempDir()
	sink := makeSink(t, dir, "main", []string{
		makeLine("nginx: GET /api", "info", "daemon"),
		makeLine("postgres query", "info", "daemon"),
		makeLine("nginx: POST /login", "info", "daemon"),
	})
	ts := newTestServer(t, []Sink{sink})
	defer ts.Close()

	resp := get(t, ts, "/search?sink=main&q=nginx")
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("status: want 200, got %d", resp.StatusCode)
	}
	var result searchResponse
	decodeJSON(t, resp, &result)
	if result.TotalMatches != 2 {
		t.Errorf("total_matches: want 2, got %d", result.TotalMatches)
	}
	if len(result.Results) != 2 {
		t.Errorf("results: want 2, got %d", len(result.Results))
	}
}

func TestHandler_Search_MissingQ_Returns400(t *testing.T) {
	dir := t.TempDir()
	sink := makeSink(t, dir, "main", nil)
	ts := newTestServer(t, []Sink{sink})
	defer ts.Close()

	resp := get(t, ts, "/search?sink=main")
	resp.Body.Close()
	if resp.StatusCode != http.StatusBadRequest {
		t.Errorf("want 400, got %d", resp.StatusCode)
	}
}

func TestHandler_Search_TotalMatchesVsLimit(t *testing.T) {
	dir := t.TempDir()
	var lines []string
	for i := 0; i < 10; i++ {
		lines = append(lines, makeLine(fmt.Sprintf("target %d", i), "info", "daemon"))
	}
	sink := makeSink(t, dir, "main", lines)
	ts := newTestServer(t, []Sink{sink})
	defer ts.Close()

	resp := get(t, ts, "/search?sink=main&q=target&limit=3")
	var result searchResponse
	decodeJSON(t, resp, &result)
	if result.TotalMatches != 10 {
		t.Errorf("total_matches: want 10, got %d", result.TotalMatches)
	}
	if len(result.Results) != 3 {
		t.Errorf("results: want 3 (capped), got %d", len(result.Results))
	}
}

func TestHandler_Search_UnknownSink_Returns400(t *testing.T) {
	dir := t.TempDir()
	sink := makeSink(t, dir, "main", nil)
	ts := newTestServer(t, []Sink{sink})
	defer ts.Close()

	resp := get(t, ts, "/search?sink=bad&q=foo")
	resp.Body.Close()
	if resp.StatusCode != http.StatusBadRequest {
		t.Errorf("want 400, got %d", resp.StatusCode)
	}
}

// ── /assets ───────────────────────────────────────────────────────────────────

func TestHandler_Assets_AppJS_OK(t *testing.T) {
	ts := newTestServer(t, nil)
	defer ts.Close()

	resp := get(t, ts, "/assets/app.js")
	resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Errorf("want 200 for /assets/app.js, got %d", resp.StatusCode)
	}
	ct := resp.Header.Get("Content-Type")
	if !strings.Contains(ct, "javascript") {
		t.Errorf("content-type: want javascript, got %q", ct)
	}
}

func TestHandler_Root_ServesIndexHTML(t *testing.T) {
	ts := newTestServer(t, nil)
	defer ts.Close()

	resp := get(t, ts, "/")
	resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Errorf("want 200 for /, got %d", resp.StatusCode)
	}
}

func TestHandler_UnknownPath_Returns404(t *testing.T) {
	ts := newTestServer(t, nil)
	defer ts.Close()

	resp := get(t, ts, "/nonexistent")
	resp.Body.Close()
	if resp.StatusCode != http.StatusNotFound {
		t.Errorf("want 404 for unknown path, got %d", resp.StatusCode)
	}
}

// ── /lines — additional coverage ──────────────────────────────────────────────

func TestHandler_Lines_MissingSink_Returns400(t *testing.T) {
	// An absent sink= parameter should be treated the same as an unknown sink.
	dir := t.TempDir()
	sink := makeSink(t, dir, "main", nil)
	ts := newTestServer(t, []Sink{sink})
	defer ts.Close()

	resp := get(t, ts, "/lines")
	resp.Body.Close()
	if resp.StatusCode != http.StatusBadRequest {
		t.Errorf("want 400 for missing sink param, got %d", resp.StatusCode)
	}
}

func TestHandler_Lines_ContentType_IsJSON(t *testing.T) {
	dir := t.TempDir()
	sink := makeSink(t, dir, "main", []string{makeLine("msg", "info", "daemon")})
	ts := newTestServer(t, []Sink{sink})
	defer ts.Close()

	resp := get(t, ts, "/lines?sink=main&tail=true")
	resp.Body.Close()
	ct := resp.Header.Get("Content-Type")
	if !strings.Contains(ct, "application/json") {
		t.Errorf("Content-Type: want application/json, got %q", ct)
	}
}

func TestHandler_Lines_BackwardAtOffsetZero_ReturnsEmpty(t *testing.T) {
	// ReadBackward(0, ...) hits the early-return guard; the handler must still
	// return a valid 200 response with empty arrays, not an error.
	dir := t.TempDir()
	sink := makeSink(t, dir, "main", []string{makeLine("msg", "info", "daemon")})
	ts := newTestServer(t, []Sink{sink})
	defer ts.Close()

	resp := get(t, ts, "/lines?sink=main&dir=backward&offset=0&count=10")
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("want 200, got %d", resp.StatusCode)
	}
	var result linesResponse
	decodeJSON(t, resp, &result)
	if len(result.Lines) != 0 {
		t.Errorf("want 0 lines, got %d", len(result.Lines))
	}
}

func TestHandler_Lines_TotalField_EqualsFileSize(t *testing.T) {
	dir := t.TempDir()
	line := makeLine("msg", "info", "daemon")
	sink := makeSink(t, dir, "main", []string{line})
	ts := newTestServer(t, []Sink{sink})
	defer ts.Close()

	type resp struct {
		Total int64 `json:"total"`
	}
	r := get(t, ts, "/lines?sink=main&tail=true")
	var result resp
	decodeJSON(t, r, &result)
	expected := int64(len(line) + 1) // +1 for newline
	if result.Total != expected {
		t.Errorf("total: want %d, got %d", expected, result.Total)
	}
}

func TestHandler_Lines_FacilityFilter(t *testing.T) {
	dir := t.TempDir()
	sink := makeSink(t, dir, "main", []string{
		makeLine("keep", "info", "auth"), // facility auth
		makeLine("drop", "info", "daemon"), // facility daemon
	})
	ts := newTestServer(t, []Sink{sink})
	defer ts.Close()

	resp := get(t, ts, "/lines?sink=main&tail=true&count=10&fac=auth")
	var result linesResponse
	decodeJSON(t, resp, &result)
	if len(result.Lines) != 1 || !strings.Contains(result.Lines[0], "keep") {
		t.Errorf("want [keep], got %v", result.Lines)
	}
}

func TestHandler_Lines_MultipleIncludeParams(t *testing.T) {
	dir := t.TempDir()
	sink := makeSink(t, dir, "main", []string{
		makeLine("nginx request", "info", "daemon"),
		makeLine("postgres query", "info", "daemon"),
		makeLine("redis command", "info", "daemon"),
	})
	ts := newTestServer(t, []Sink{sink})
	defer ts.Close()

	// Two separate inc= parameters; any match should include the line.
	resp := get(t, ts, "/lines?sink=main&tail=true&count=10&inc=nginx&inc=postgres")
	var result linesResponse
	decodeJSON(t, resp, &result)
	if len(result.Lines) != 2 {
		t.Errorf("want 2 lines (nginx + postgres), got %d: %v", len(result.Lines), result.Lines)
	}
}

func TestHandler_Lines_MultipleExcludeParams(t *testing.T) {
	dir := t.TempDir()
	sink := makeSink(t, dir, "main", []string{
		makeLine("keep this", "info", "daemon"),
		makeLine("drop alpha", "info", "daemon"),
		makeLine("drop beta", "info", "daemon"),
	})
	ts := newTestServer(t, []Sink{sink})
	defer ts.Close()

	resp := get(t, ts, "/lines?sink=main&tail=true&count=10&exc=alpha&exc=beta")
	var result linesResponse
	decodeJSON(t, resp, &result)
	if len(result.Lines) != 1 || !strings.Contains(result.Lines[0], "keep") {
		t.Errorf("want [keep this], got %v", result.Lines)
	}
}

func TestHandler_Lines_BadCountFallsBackToDefault(t *testing.T) {
	// A non-numeric count should fall back to the default (200) rather than
	// returning an error; we just verify the response is still 200.
	dir := t.TempDir()
	sink := makeSink(t, dir, "main", []string{makeLine("msg", "info", "daemon")})
	ts := newTestServer(t, []Sink{sink})
	defer ts.Close()

	resp := get(t, ts, "/lines?sink=main&tail=true&count=notanumber")
	resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Errorf("want 200 for bad count, got %d", resp.StatusCode)
	}
}

// ── /search — additional coverage ─────────────────────────────────────────────

func TestHandler_Search_ContentType_IsJSON(t *testing.T) {
	dir := t.TempDir()
	sink := makeSink(t, dir, "main", []string{makeLine("msg", "info", "daemon")})
	ts := newTestServer(t, []Sink{sink})
	defer ts.Close()

	resp := get(t, ts, "/search?sink=main&q=msg")
	resp.Body.Close()
	ct := resp.Header.Get("Content-Type")
	if !strings.Contains(ct, "application/json") {
		t.Errorf("Content-Type: want application/json, got %q", ct)
	}
}

func TestHandler_Search_WithFilters(t *testing.T) {
	// Combine facility + exclude filters through the HTTP layer.
	dir := t.TempDir()
	sink := makeSink(t, dir, "main", []string{
		makeLine("target auth", "info", "auth"),   // facility 4, passes all
		makeLine("target daemon", "info", "daemon"), // facility daemon, rejected by fac=auth
		makeLine("target debug", "info", "auth"),  // facility auth but excluded by exc=debug
	})
	ts := newTestServer(t, []Sink{sink})
	defer ts.Close()

	resp := get(t, ts, "/search?sink=main&q=target&fac=auth&exc=debug")
	var result searchResponse
	decodeJSON(t, resp, &result)
	if result.TotalMatches != 1 {
		t.Errorf("total_matches: want 1, got %d", result.TotalMatches)
	}
	if len(result.Results) != 1 || !strings.Contains(result.Results[0].Line, "auth") {
		t.Errorf("want [auth], got %v", result.Results)
	}
}

func TestHandler_Search_EmptyChain_Returns200(t *testing.T) {
	// If the JSONL file does not exist, the chain is empty; search should
	// return 200 with zero results, not an error.
	dir := t.TempDir()
	// Sink points at a non-existent file.
	sink := Sink{Name: "main", Path: filepath.Join(dir, "missing.jsonl"), MaxFiles: 5}
	ts := newTestServer(t, []Sink{sink})
	defer ts.Close()

	resp := get(t, ts, "/search?sink=main&q=anything")
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("want 200, got %d", resp.StatusCode)
	}
	var result searchResponse
	decodeJSON(t, resp, &result)
	if result.TotalMatches != 0 || len(result.Results) != 0 {
		t.Errorf("want empty result, got total=%d results=%d", result.TotalMatches, len(result.Results))
	}
}

// ── /sinks — additional coverage ──────────────────────────────────────────────

func TestHandler_Sinks_Empty_ReturnsEmptyArray(t *testing.T) {
	// Zero sinks should return [] not null.
	ts := newTestServer(t, []Sink{})
	defer ts.Close()

	resp := get(t, ts, "/sinks")
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("want 200, got %d", resp.StatusCode)
	}
	// Decode into a raw slice so we can distinguish [] from null.
	var result []struct {
		Name string `json:"name"`
	}
	decodeJSON(t, resp, &result)
	if result == nil {
		t.Error("sinks should be [] not null for empty list")
	}
	if len(result) != 0 {
		t.Errorf("want 0 sinks, got %d", len(result))
	}
}

// ── /assets — additional coverage ─────────────────────────────────────────────

func TestHandler_Assets_StyleCSS_OK(t *testing.T) {
	ts := newTestServer(t, nil)
	defer ts.Close()

	resp := get(t, ts, "/assets/style.css")
	resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Errorf("want 200 for /assets/style.css, got %d", resp.StatusCode)
	}
	ct := resp.Header.Get("Content-Type")
	if !strings.Contains(ct, "css") {
		t.Errorf("content-type: want css, got %q", ct)
	}
}

func TestHandler_Root_ContentType_IsHTML(t *testing.T) {
	ts := newTestServer(t, nil)
	defer ts.Close()

	resp := get(t, ts, "/")
	resp.Body.Close()
	ct := resp.Header.Get("Content-Type")
	if !strings.Contains(ct, "html") {
		t.Errorf("content-type for /: want html, got %q", ct)
	}
}
