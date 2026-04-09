// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

package main

import (
	"os"
	"path/filepath"
	"testing"
)

// writeConfig writes content to a temp file named "minilog.conf" inside dir
// and returns the full path.
func writeConfig(t *testing.T, dir, content string) string {
	t.Helper()
	p := filepath.Join(dir, "minilog.conf")
	if err := os.WriteFile(p, []byte(content), 0o600); err != nil {
		t.Fatalf("writeConfig: %v", err)
	}
	return p
}

func TestLoadSinks_MinimalValid(t *testing.T) {
	dir := t.TempDir()
	p := writeConfig(t, dir, `
[output.main]
jsonl_file = syslog.jsonl
`)
	sinks, err := loadSinks(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(sinks) != 1 {
		t.Fatalf("want 1 sink, got %d", len(sinks))
	}
	if sinks[0].Name != "main" {
		t.Errorf("name: want %q, got %q", "main", sinks[0].Name)
	}
}

func TestLoadSinks_RelativePathResolvedAgainstConfigDir(t *testing.T) {
	dir := t.TempDir()
	p := writeConfig(t, dir, `
[output.main]
jsonl_file = logs/syslog.jsonl
`)
	sinks, err := loadSinks(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	want := filepath.Join(dir, "logs", "syslog.jsonl")
	if sinks[0].Path != want {
		t.Errorf("path: want %q, got %q", want, sinks[0].Path)
	}
}

func TestLoadSinks_AbsolutePathPassedThrough(t *testing.T) {
	dir := t.TempDir()
	abs := filepath.Join(dir, "data", "syslog.jsonl")
	p := writeConfig(t, dir, "[output.main]\njsonl_file = "+abs+"\n")
	sinks, err := loadSinks(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if sinks[0].Path != abs {
		t.Errorf("path: want %q, got %q", abs, sinks[0].Path)
	}
}

func TestLoadSinks_MaxFilesDefault(t *testing.T) {
	dir := t.TempDir()
	p := writeConfig(t, dir, "[output.main]\njsonl_file = syslog.jsonl\n")
	sinks, err := loadSinks(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if sinks[0].MaxFiles != 10 {
		t.Errorf("MaxFiles: want 10, got %d", sinks[0].MaxFiles)
	}
}

func TestLoadSinks_MaxFilesExplicit(t *testing.T) {
	dir := t.TempDir()
	p := writeConfig(t, dir, "[output.main]\njsonl_file = syslog.jsonl\nmax_files = 5\n")
	sinks, err := loadSinks(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if sinks[0].MaxFiles != 5 {
		t.Errorf("MaxFiles: want 5, got %d", sinks[0].MaxFiles)
	}
}

func TestLoadSinks_MultipleSinks(t *testing.T) {
	dir := t.TempDir()
	p := writeConfig(t, dir, `
[output.main]
jsonl_file = syslog.jsonl

[output.auth]
jsonl_file = auth.jsonl
max_files = 3
`)
	sinks, err := loadSinks(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(sinks) != 2 {
		t.Fatalf("want 2 sinks, got %d", len(sinks))
	}
	if sinks[0].Name != "main" || sinks[1].Name != "auth" {
		t.Errorf("names: want [main auth], got [%s %s]", sinks[0].Name, sinks[1].Name)
	}
	if sinks[1].MaxFiles != 3 {
		t.Errorf("auth MaxFiles: want 3, got %d", sinks[1].MaxFiles)
	}
}

func TestLoadSinks_NoOutputSections_Error(t *testing.T) {
	dir := t.TempDir()
	p := writeConfig(t, dir, "[server]\nhost = 127.0.0.1\n")
	_, err := loadSinks(p)
	if err == nil {
		t.Fatal("want error for config with no output sections, got nil")
	}
}

func TestLoadSinks_OutputSectionWithoutJsonlFile_Excluded(t *testing.T) {
	dir := t.TempDir()
	p := writeConfig(t, dir, `
[output.main]
text_file = syslog.log
max_files = 5

[output.auth]
jsonl_file = auth.jsonl
`)
	sinks, err := loadSinks(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(sinks) != 1 || sinks[0].Name != "auth" {
		t.Errorf("want only auth sink, got %v", sinks)
	}
}

func TestLoadSinks_InlineCommentStripped(t *testing.T) {
	dir := t.TempDir()
	p := writeConfig(t, dir, "[output.main]\njsonl_file = syslog.jsonl ; this is a comment\n")
	sinks, err := loadSinks(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	want := filepath.Join(dir, "syslog.jsonl")
	if sinks[0].Path != want {
		t.Errorf("path: want %q, got %q", want, sinks[0].Path)
	}
}

func TestLoadSinks_HashInlineCommentStripped(t *testing.T) {
	dir := t.TempDir()
	p := writeConfig(t, dir, "[output.main]\njsonl_file = syslog.jsonl # this is a comment\n")
	sinks, err := loadSinks(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	want := filepath.Join(dir, "syslog.jsonl")
	if sinks[0].Path != want {
		t.Errorf("path: want %q, got %q", want, sinks[0].Path)
	}
}

func TestLoadSinks_NonOutputSectionsIgnored(t *testing.T) {
	dir := t.TempDir()
	p := writeConfig(t, dir, `
[server]
host = 127.0.0.1
udp_port = 514

[forwarding]
target = 10.0.0.1:514

[output.main]
jsonl_file = syslog.jsonl
`)
	sinks, err := loadSinks(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(sinks) != 1 {
		t.Fatalf("want 1 sink, got %d", len(sinks))
	}
}

func TestLoadSinks_MissingConfigFile_Error(t *testing.T) {
	_, err := loadSinks("/nonexistent/path/minilog.conf")
	if err == nil {
		t.Fatal("want error for missing config file, got nil")
	}
}

// ── config — additional coverage ──────────────────────────────────────────────

func TestLoadSinks_MaxFilesZero_Unlimited(t *testing.T) {
	// max_files = 0 means unlimited rotation in the server config; the viewer
	// must map this to the unlimitedMaxFiles sentinel (1000) so it probes all
	// available rotated generations.
	dir := t.TempDir()
	p := writeConfig(t, dir, "[output.main]\njsonl_file = syslog.jsonl\nmax_files = 0\n")
	sinks, err := loadSinks(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if sinks[0].MaxFiles != 1000 {
		t.Errorf("MaxFiles: want 1000 (unlimited sentinel), got %d", sinks[0].MaxFiles)
	}
}

func TestLoadSinks_SectionNoKeys_Excluded(t *testing.T) {
	// An [output.x] section with no keys at all has no jsonl_file and must
	// be excluded; if it is the only output section, loadSinks should error.
	dir := t.TempDir()
	p := writeConfig(t, dir, "[output.empty]\n")
	_, err := loadSinks(p)
	if err == nil {
		t.Fatal("want error when output section has no jsonl_file, got nil")
	}
}

func TestLoadSinks_HashComment_Skipped(t *testing.T) {
	// Lines starting with '#' must be treated as comments.
	dir := t.TempDir()
	p := writeConfig(t, dir, `
# this is a hash comment
[output.main]
# another comment
jsonl_file = syslog.jsonl
`)
	sinks, err := loadSinks(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(sinks) != 1 || sinks[0].Name != "main" {
		t.Errorf("want 1 sink named main, got %v", sinks)
	}
}

func TestLoadSinks_SectionNameWithWhitespace(t *testing.T) {
	// [ output.main ] with leading/trailing spaces inside the brackets.
	dir := t.TempDir()
	p := writeConfig(t, dir, "[ output.main ]\njsonl_file = syslog.jsonl\n")
	sinks, err := loadSinks(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(sinks) != 1 || sinks[0].Name != "main" {
		t.Errorf("want sink named main, got %v", sinks)
	}
}

func TestLoadSinks_MaxFiles_InlineComment_Stripped(t *testing.T) {
	dir := t.TempDir()
	p := writeConfig(t, dir, `[output.main]
jsonl_file = syslog.jsonl
max_files = 7 ; keep 7 generations
`)
	sinks, err := loadSinks(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if sinks[0].MaxFiles != 7 {
		t.Errorf("MaxFiles: want 7, got %d", sinks[0].MaxFiles)
	}
}

func TestLoadSinks_MalformedSectionHeader_NoClosingBracket_Skipped(t *testing.T) {
	// A section header with no closing ']' must be skipped; the subsequent
	// key=value lines are not in any output section, so no sinks are produced.
	dir := t.TempDir()
	p := writeConfig(t, dir, `
[output.broken
jsonl_file = syslog.jsonl

[output.good]
jsonl_file = good.jsonl
`)
	sinks, err := loadSinks(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	// Only the well-formed section should produce a sink.
	if len(sinks) != 1 || sinks[0].Name != "good" {
		t.Errorf("want 1 sink named good, got %v", sinks)
	}
}

func TestLoadSinks_KeyLineWithoutEquals_Skipped(t *testing.T) {
	// A key line inside an output section that has no '=' must be silently
	// skipped; the section is still valid if jsonl_file is set elsewhere.
	dir := t.TempDir()
	p := writeConfig(t, dir, `[output.main]
not_a_key_value_line
jsonl_file = syslog.jsonl
`)
	sinks, err := loadSinks(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(sinks) != 1 || sinks[0].Name != "main" {
		t.Errorf("want 1 sink named main, got %v", sinks)
	}
}
