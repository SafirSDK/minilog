// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

//go:build windows

package main

import (
	"errors"
	"os"
	"path/filepath"
	"testing"
)

// ── openShared ───────────────────────────────────────────────────────────────
//
// Every log read goes through it. The success path is covered by the reader and
// handler tests; what is not is the path refused before CreateFile is reached.

func TestOpenSharedOpensALogFile(t *testing.T) {
	path := filepath.Join(t.TempDir(), "syslog.jsonl")
	if err := os.WriteFile(path, []byte("{\"msg\":\"hello\"}\n"), 0o600); err != nil {
		t.Fatalf("writing the file: %v", err)
	}

	f, err := openShared(path)
	if err != nil {
		t.Fatalf("openShared: %v", err)
	}
	defer f.Close()

	buf := make([]byte, 5)
	if _, err := f.Read(buf); err != nil {
		t.Fatalf("reading what it opened: %v", err)
	}
}

func TestOpenSharedRefusesAPathWithANulByte(t *testing.T) {
	// A NUL cannot be passed to the Win32 wide-string API, which takes a
	// NUL-terminated string: the path would be silently truncated there and a
	// different file opened. UTF16PtrFromString refuses it instead, and the
	// error has to come back as a PathError naming the path rather than as a
	// panic or a successful open of something else.
	_, err := openShared("C:\\minilog\\sys\x00log.jsonl")

	if err == nil {
		t.Fatal("openShared accepted a path with a NUL byte")
	}
	var pathErr *os.PathError
	if !errors.As(err, &pathErr) {
		t.Errorf("openShared error = %T (%v), want *os.PathError", err, err)
	}
}
