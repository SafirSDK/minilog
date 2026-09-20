// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

package main

import (
	"os"
	"path/filepath"
	"strings"
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

func TestLoadConfig_MinimalValid(t *testing.T) {
	dir := t.TempDir()
	p := writeConfig(t, dir, `
[output.main]
jsonl_file = syslog.jsonl
`)
	cfg, err := loadConfig(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	sinks := cfg.Sinks
	if len(sinks) != 1 {
		t.Fatalf("want 1 sink, got %d", len(sinks))
	}
	if sinks[0].Name != "main" {
		t.Errorf("name: want %q, got %q", "main", sinks[0].Name)
	}
}

func TestLoadConfig_RelativePathUsedVerbatim(t *testing.T) {
	// The viewer does not resolve a relative path against anything: minilog's
	// config loader rejects one, so this can only be reached by pointing the
	// viewer at a config the server would refuse to start on. Resolving it here
	// is what used to make one config name two different files.
	dir := t.TempDir()
	p := writeConfig(t, dir, `
[output.main]
jsonl_file = logs/syslog.jsonl
`)
	cfg, err := loadConfig(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	sinks := cfg.Sinks
	if sinks[0].Path != "logs/syslog.jsonl" {
		t.Errorf("path: want %q, got %q", "logs/syslog.jsonl", sinks[0].Path)
	}
}

func TestLoadConfig_AbsolutePathPassedThrough(t *testing.T) {
	dir := t.TempDir()
	abs := filepath.Join(dir, "data", "syslog.jsonl")
	p := writeConfig(t, dir, "[output.main]\njsonl_file = "+abs+"\n")
	cfg, err := loadConfig(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	sinks := cfg.Sinks
	if sinks[0].Path != abs {
		t.Errorf("path: want %q, got %q", abs, sinks[0].Path)
	}
}

func TestLoadConfig_MaxFilesDefault(t *testing.T) {
	dir := t.TempDir()
	p := writeConfig(t, dir, "[output.main]\njsonl_file = syslog.jsonl\n")
	cfg, err := loadConfig(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	sinks := cfg.Sinks
	if sinks[0].MaxFiles != 10 {
		t.Errorf("MaxFiles: want 10, got %d", sinks[0].MaxFiles)
	}
}

func TestLoadConfig_MaxFilesExplicit(t *testing.T) {
	dir := t.TempDir()
	p := writeConfig(t, dir, "[output.main]\njsonl_file = syslog.jsonl\nmax_files = 5\n")
	cfg, err := loadConfig(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	sinks := cfg.Sinks
	if sinks[0].MaxFiles != 5 {
		t.Errorf("MaxFiles: want 5, got %d", sinks[0].MaxFiles)
	}
}

func TestLoadConfig_MultipleSinks(t *testing.T) {
	dir := t.TempDir()
	p := writeConfig(t, dir, `
[output.main]
jsonl_file = syslog.jsonl

[output.auth]
jsonl_file = auth.jsonl
max_files = 3
`)
	cfg, err := loadConfig(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	sinks := cfg.Sinks
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

func TestLoadConfig_NoOutputSections_Error(t *testing.T) {
	dir := t.TempDir()
	p := writeConfig(t, dir, "[server]\nhost = 127.0.0.1\n")
	_, err := loadConfig(p)
	if err == nil {
		t.Fatal("want error for config with no output sections, got nil")
	}
}

func TestLoadConfig_OutputSectionWithoutJsonlFile_Excluded(t *testing.T) {
	dir := t.TempDir()
	p := writeConfig(t, dir, `
[output.main]
text_file = syslog.log
max_files = 5

[output.auth]
jsonl_file = auth.jsonl
`)
	cfg, err := loadConfig(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	sinks := cfg.Sinks
	if len(sinks) != 1 || sinks[0].Name != "auth" {
		t.Errorf("want only auth sink, got %v", sinks)
	}
}

// A value runs to the end of the line, exactly as the server reads it. The
// viewer used to strip from the first ';' or '#', so it opened a different file
// from the one minilog writes — silently, because a missing sink file is
// indistinguishable from one that has had no traffic yet.

func TestLoadConfig_SemicolonIsPartOfTheValue(t *testing.T) {
	dir := t.TempDir()
	abs := filepath.Join(dir, "semi;colon.jsonl")
	p := writeConfig(t, dir, "[output.main]\njsonl_file = "+abs+"\n")
	cfg, err := loadConfig(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	sinks := cfg.Sinks
	if sinks[0].Path != abs {
		t.Errorf("path: want %q, got %q", abs, sinks[0].Path)
	}
}

func TestLoadConfig_HashIsPartOfTheValue(t *testing.T) {
	// '#' is a legal filename character on NTFS and ext4 alike.
	dir := t.TempDir()
	abs := filepath.Join(dir, "hash#name.jsonl")
	p := writeConfig(t, dir, "[output.main]\njsonl_file = "+abs+"\n")
	cfg, err := loadConfig(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	sinks := cfg.Sinks
	if sinks[0].Path != abs {
		t.Errorf("path: want %q, got %q", abs, sinks[0].Path)
	}
}

func TestLoadConfig_TrailingCommentIsNotStripped(t *testing.T) {
	// What somebody writing a comment after a value would get: the comment is
	// part of the path, so the sink names a file that does not exist. That is
	// the server's behaviour too, which is the point — one file, one reading.
	dir := t.TempDir()
	p := writeConfig(t, dir, "[output.main]\njsonl_file = /var/log/syslog.jsonl ; the main sink\n")
	cfg, err := loadConfig(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	sinks := cfg.Sinks
	const want = "/var/log/syslog.jsonl ; the main sink"
	if sinks[0].Path != want {
		t.Errorf("path: want %q, got %q", want, sinks[0].Path)
	}
}

func TestLoadConfig_NonOutputSectionsIgnored(t *testing.T) {
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
	cfg, err := loadConfig(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	sinks := cfg.Sinks
	if len(sinks) != 1 {
		t.Fatalf("want 1 sink, got %d", len(sinks))
	}
}

func TestLoadConfig_MissingConfigFile_Error(t *testing.T) {
	_, err := loadConfig("/nonexistent/path/minilog.conf")
	if err == nil {
		t.Fatal("want error for missing config file, got nil")
	}
}

// ── config — additional coverage ──────────────────────────────────────────────

func TestLoadConfig_MaxFilesZero_Unlimited(t *testing.T) {
	// max_files = 0 means unlimited rotation in the server config; the viewer
	// must map this to the unlimitedMaxFiles sentinel (1000) so it probes all
	// available rotated generations.
	dir := t.TempDir()
	p := writeConfig(t, dir, "[output.main]\njsonl_file = syslog.jsonl\nmax_files = 0\n")
	cfg, err := loadConfig(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	sinks := cfg.Sinks
	if sinks[0].MaxFiles != 1000 {
		t.Errorf("MaxFiles: want 1000 (unlimited sentinel), got %d", sinks[0].MaxFiles)
	}
}

func TestLoadConfig_SectionNoKeys_Excluded(t *testing.T) {
	// An [output.x] section with no keys at all has no jsonl_file and must
	// be excluded; if it is the only output section, loadConfig should error.
	dir := t.TempDir()
	p := writeConfig(t, dir, "[output.empty]\n")
	_, err := loadConfig(p)
	if err == nil {
		t.Fatal("want error when output section has no jsonl_file, got nil")
	}
}

func TestLoadConfig_HashComment_Skipped(t *testing.T) {
	// Lines starting with '#' must be treated as comments.
	dir := t.TempDir()
	p := writeConfig(t, dir, `
# this is a hash comment
[output.main]
# another comment
jsonl_file = syslog.jsonl
`)
	cfg, err := loadConfig(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	sinks := cfg.Sinks
	if len(sinks) != 1 || sinks[0].Name != "main" {
		t.Errorf("want 1 sink named main, got %v", sinks)
	}
}

func TestLoadConfig_SectionNameWithWhitespace(t *testing.T) {
	// [ output.main ] with leading/trailing spaces inside the brackets.
	dir := t.TempDir()
	p := writeConfig(t, dir, "[ output.main ]\njsonl_file = syslog.jsonl\n")
	cfg, err := loadConfig(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	sinks := cfg.Sinks
	if len(sinks) != 1 || sinks[0].Name != "main" {
		t.Errorf("want sink named main, got %v", sinks)
	}
}

func TestLoadConfig_MaxFiles_TrailingComment_FallsBackToDefault(t *testing.T) {
	// "7 ; keep 7 generations" is not a number, so the value is ignored and the
	// default applies. Boost does the same thing with the same line in the
	// server — ptree::get<int>(path, default) returns the default when the
	// value will not translate — so both ends rotate to the same depth.
	dir := t.TempDir()
	p := writeConfig(t, dir, `[output.main]
jsonl_file = /var/log/syslog.jsonl
max_files = 7 ; keep 7 generations
`)
	cfg, err := loadConfig(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	sinks := cfg.Sinks
	if sinks[0].MaxFiles != 10 {
		t.Errorf("MaxFiles: want 10 (default), got %d", sinks[0].MaxFiles)
	}
}

func TestLoadConfig_MalformedSectionHeader_NoClosingBracket_Skipped(t *testing.T) {
	// A section header with no closing ']' must be skipped; the subsequent
	// key=value lines are not in any output section, so no sinks are produced.
	dir := t.TempDir()
	p := writeConfig(t, dir, `
[output.broken
jsonl_file = syslog.jsonl

[output.good]
jsonl_file = good.jsonl
`)
	cfg, err := loadConfig(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	sinks := cfg.Sinks
	// Only the well-formed section should produce a sink.
	if len(sinks) != 1 || sinks[0].Name != "good" {
		t.Errorf("want 1 sink named good, got %v", sinks)
	}
}

func TestLoadConfig_KeyLineWithoutEquals_Skipped(t *testing.T) {
	// A key line inside an output section that has no '=' must be silently
	// skipped; the section is still valid if jsonl_file is set elsewhere.
	dir := t.TempDir()
	p := writeConfig(t, dir, `[output.main]
not_a_key_value_line
jsonl_file = syslog.jsonl
`)
	cfg, err := loadConfig(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	sinks := cfg.Sinks
	if len(sinks) != 1 || sinks[0].Name != "main" {
		t.Errorf("want 1 sink named main, got %v", sinks)
	}
}

// ── [web_viewer] — the viewer's own section ───────────────────────────────────
//
// The listen address is a deployment fact, like the UDP port and the log paths,
// so it lives in minilog.conf rather than in a flag frozen into the service
// registration at install time. Changing the port is "edit the conf, restart
// the service"; it used to be "re-register the service".

func TestLoadConfig_ListenAddrDefaultsToAllInterfaces(t *testing.T) {
	// No [web_viewer] section: the address must stay what the removed --addr
	// flag defaulted to, so existing deployments do not move.
	dir := t.TempDir()
	p := writeConfig(t, dir, "[output.main]\njsonl_file = /var/log/syslog.jsonl\n")
	cfg, err := loadConfig(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if cfg.Addr != ":9514" {
		t.Errorf("Addr: want %q, got %q", ":9514", cfg.Addr)
	}
}

func TestLoadConfig_ListenHostAndPort(t *testing.T) {
	dir := t.TempDir()
	p := writeConfig(t, dir, `[output.main]
jsonl_file = /var/log/syslog.jsonl

[web_viewer]
host = 127.0.0.1
port = 8080
`)
	cfg, err := loadConfig(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if cfg.Addr != "127.0.0.1:8080" {
		t.Errorf("Addr: want %q, got %q", "127.0.0.1:8080", cfg.Addr)
	}
}

func TestLoadConfig_EmptyHostMeansEveryInterface(t *testing.T) {
	// The trap in splitting ":9514" into host and port: defaulting the host to
	// "0.0.0.0" would quietly drop IPv6, which the combined form listened on.
	// An absent or empty host has to assemble back to ":port".
	for _, content := range []string{
		"[output.main]\njsonl_file = /var/log/syslog.jsonl\n\n[web_viewer]\nport = 8080\n",
		"[output.main]\njsonl_file = /var/log/syslog.jsonl\n\n[web_viewer]\nhost =\nport = 8080\n",
	} {
		dir := t.TempDir()
		cfg, err := loadConfig(writeConfig(t, dir, content))
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if cfg.Addr != ":8080" {
			t.Errorf("Addr: want %q, got %q (config %q)", ":8080", cfg.Addr, content)
		}
	}
}

func TestLoadConfig_HostOnlyKeepsDefaultPort(t *testing.T) {
	dir := t.TempDir()
	p := writeConfig(t, dir,
		"[output.main]\njsonl_file = /var/log/syslog.jsonl\n\n[web_viewer]\nhost = 127.0.0.1\n")
	cfg, err := loadConfig(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if cfg.Addr != "127.0.0.1:9514" {
		t.Errorf("Addr: want %q, got %q", "127.0.0.1:9514", cfg.Addr)
	}
}

func TestLoadConfig_IPv6HostIsBracketed(t *testing.T) {
	dir := t.TempDir()
	p := writeConfig(t, dir,
		"[output.main]\njsonl_file = /var/log/syslog.jsonl\n\n[web_viewer]\nhost = ::1\nport = 8080\n")
	cfg, err := loadConfig(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if cfg.Addr != "[::1]:8080" {
		t.Errorf("Addr: want %q, got %q", "[::1]:8080", cfg.Addr)
	}
}

func TestLoadConfig_BadPortIsAnError(t *testing.T) {
	// net.Listen would reject these too, but with an error that says nothing
	// about which key in which section is wrong — and under the SCM that error
	// is all anybody gets.
	for _, port := range []string{"80a", "-1", "65536", ""} {
		dir := t.TempDir()
		content := "[output.main]\njsonl_file = /var/log/syslog.jsonl\n\n[web_viewer]\nport = " +
			port + "\n"
		cfg, err := loadConfig(writeConfig(t, dir, content))
		if port == "" {
			// An empty value is "not configured", which is the default, not an error.
			if err != nil || cfg.Addr != ":9514" {
				t.Errorf("empty port: got addr %q, err %v; want %q, nil", cfg.Addr, err, ":9514")
			}
			continue
		}
		if err == nil {
			t.Errorf("port %q was accepted, giving %q", port, cfg.Addr)
			continue
		}
		if !strings.Contains(err.Error(), "[web_viewer] port") {
			t.Errorf("error for port %q does not name the key: %v", port, err)
		}
	}
}

func TestLoadConfig_ViewerSectionKeysDoNotLeakIntoSinks(t *testing.T) {
	// [web_viewer] is not an output section; a max_files there must not reach a
	// sink, and the section ending must not leave the parser still inside it.
	dir := t.TempDir()
	p := writeConfig(t, dir, `[web_viewer]
port = 8080
max_files = 3

[output.main]
jsonl_file = /var/log/syslog.jsonl
`)
	cfg, err := loadConfig(p)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(cfg.Sinks) != 1 || cfg.Sinks[0].MaxFiles != 10 {
		t.Errorf("sinks: %+v, want one sink with the default MaxFiles", cfg.Sinks)
	}
	if cfg.Addr != ":8080" {
		t.Errorf("Addr: want %q, got %q", ":8080", cfg.Addr)
	}
}
