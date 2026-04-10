// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

package main

import (
	"encoding/json"
	"io/fs"
	"net/http"
	"strconv"
	"strings"
)

// registerHandlers wires up all HTTP routes onto mux.
func registerHandlers(mux *http.ServeMux, sinks []Sink) {
	// Build a lookup map: sink name → Sink.
	sinkMap := make(map[string]Sink, len(sinks))
	for _, s := range sinks {
		sinkMap[s.Name] = s
	}

	// Serve static assets under /assets/* from the embedded FS.
	staticFS, err := fs.Sub(assets, "assets")
	if err != nil {
		panic(err)
	}
	mux.Handle("GET /assets/", http.StripPrefix("/assets/", http.FileServer(http.FS(staticFS))))

	// GET / — serve index.html.
	mux.HandleFunc("GET /", func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/" {
			http.NotFound(w, r)
			return
		}
		http.ServeFileFS(w, r, staticFS, "index.html")
	})

	// GET /version — return the build version string.
	mux.HandleFunc("GET /version", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]string{"version": version})
	})

	// GET /sinks — return JSON array of sink names.
	mux.HandleFunc("GET /sinks", func(w http.ResponseWriter, r *http.Request) {
		type sinkInfo struct {
			Name string `json:"name"`
		}
		out := make([]sinkInfo, len(sinks))
		for i, s := range sinks {
			out[i] = sinkInfo{Name: s.Name}
		}
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(out)
	})

	// GET /lines — fetch a page of filtered log lines.
	mux.HandleFunc("GET /lines", func(w http.ResponseWriter, r *http.Request) {
		sink, ok := resolveSink(w, r, sinkMap)
		if !ok {
			return
		}

		q := r.URL.Query()
		tail := q.Get("tail") == "true"
		offset := parseInt64(q.Get("offset"), 0)
		count := parseInt(q.Get("count"), 200)
		dir := q.Get("dir")
		if dir == "" {
			dir = "forward"
		}
		since := parseInt64(q.Get("since"), -1)
		f := parseFilter(r)

		fc, err := NewFileChain(sink)
		if err != nil {
			http.Error(w, "cannot build file chain: "+err.Error(), http.StatusInternalServerError)
			return
		}

		type response struct {
			Lines       []string `json:"lines"`
			Offsets     []int64  `json:"offsets"`
			FirstOffset int64    `json:"first_offset"`
			NextOffset  int64    `json:"next_offset"`
			Total       int64    `json:"total"`
			TailOffset  int64    `json:"tail_offset"`
		}

		resp := response{
			Total:      fc.TailOffset(),
			TailOffset: fc.TailOffset(),
			Lines:      []string{},
			Offsets:    []int64{},
		}

		var rawLines [][]byte
		var offsets []int64

		if fc.Empty() || (since >= 0 && since >= fc.TailOffset()) {
			// Nothing to read yet, or since is at/past tail.
		} else if tail {
			// Read last `count` matching lines from the end.
			rawLines, offsets, resp.FirstOffset, resp.NextOffset, err =
				fc.ReadBackward(fc.TailOffset(), count, f, since)
		} else if dir == "backward" {
			rawLines, offsets, resp.FirstOffset, resp.NextOffset, err =
				fc.ReadBackward(offset, count, f, since)
		} else {
			if since >= 0 && offset < since {
				offset = since
			}
			rawLines, offsets, resp.FirstOffset, resp.NextOffset, err =
				fc.ReadForward(offset, count, f)
		}

		if err != nil {
			http.Error(w, "read error: "+err.Error(), http.StatusInternalServerError)
			return
		}

		if len(rawLines) > 0 {
			resp.Lines = make([]string, len(rawLines))
			for i, l := range rawLines {
				resp.Lines[i] = string(l)
			}
			resp.Offsets = offsets
		}

		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(resp)
	})

	// GET /search — full-chain search with filters.
	mux.HandleFunc("GET /search", func(w http.ResponseWriter, r *http.Request) {
		sink, ok := resolveSink(w, r, sinkMap)
		if !ok {
			return
		}

		q := r.URL.Query()
		query := q.Get("q")
		limit := parseInt(q.Get("limit"), 200)
		since := parseInt64(q.Get("since"), -1)
		f := parseFilter(r)

		if query == "" {
			http.Error(w, "q parameter required", http.StatusBadRequest)
			return
		}

		fc, err := NewFileChain(sink)
		if err != nil {
			http.Error(w, "cannot build file chain: "+err.Error(), http.StatusInternalServerError)
			return
		}

		results, total, err := fc.Search(query, limit, f, since)
		if err != nil {
			http.Error(w, "search error: "+err.Error(), http.StatusInternalServerError)
			return
		}

		type resultItem struct {
			Line   string `json:"line"`
			Offset int64  `json:"offset"`
		}
		type response struct {
			Results      []resultItem `json:"results"`
			TotalMatches int          `json:"total_matches"`
		}

		resp := response{
			TotalMatches: total,
			Results:      make([]resultItem, len(results)),
		}
		for i, res := range results {
			resp.Results[i] = resultItem{Line: string(res.Line), Offset: res.Offset}
		}

		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(resp)
	})
}

// ── Helpers ───────────────────────────────────────────────────────────────────

func resolveSink(w http.ResponseWriter, r *http.Request, sinkMap map[string]Sink) (Sink, bool) {
	name := r.URL.Query().Get("sink")
	s, ok := sinkMap[name]
	if !ok {
		http.Error(w, "unknown sink: "+name, http.StatusBadRequest)
	}
	return s, ok
}

// parseFilter extracts filter parameters from the request query string.
//
// Query params:
//   - sev=3,4,5   comma-separated severity codes (absent = all)
//   - fac=3,4     comma-separated facility codes (absent = all)
//   - inc=nginx   repeatable include pattern
//   - exc=debug   repeatable exclude pattern
func parseFilter(r *http.Request) *Filter {
	q := r.URL.Query()
	f := &Filter{}

	if sev := q.Get("sev"); sev != "" {
		for _, s := range strings.Split(sev, ",") {
			if t := strings.ToLower(strings.TrimSpace(s)); t != "" {
				f.Severities = append(f.Severities, t)
			}
		}
	}

	if fac := q.Get("fac"); fac != "" {
		for _, s := range strings.Split(fac, ",") {
			if t := strings.ToLower(strings.TrimSpace(s)); t != "" {
				f.Facilities = append(f.Facilities, t)
			}
		}
	}

	for _, p := range q["inc"] {
		if t := strings.ToLower(strings.TrimSpace(p)); t != "" {
			f.Include = append(f.Include, t)
		}
	}

	for _, p := range q["exc"] {
		if t := strings.ToLower(strings.TrimSpace(p)); t != "" {
			f.Exclude = append(f.Exclude, t)
		}
	}

	return f
}

func parseInt(s string, def int) int {
	if n, err := strconv.Atoi(s); err == nil && n > 0 {
		return n
	}
	return def
}

func parseInt64(s string, def int64) int64 {
	if n, err := strconv.ParseInt(s, 10, 64); err == nil && n >= 0 {
		return n
	}
	return def
}
