// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

//go:build windows

package main

import (
	"errors"
	"strings"
	"testing"
)

// The two functions in service_windows.go that need no SCM. Everything else in
// that file goes through mgr.Connect or svc.Run, which cannot be faked —
// mgr.Service is a concrete type — so the installer tests
// (tests/installer/test_zip_install.py) are what exercise those, against a real
// service. These two are pure, and were the whole file's only untested logic
// that did not need a machine to install on.

// ── serviceCommandLine ───────────────────────────────────────────────────────
//
// What a service registration stores. Getting the quoting wrong is not a
// crash: the SCM simply fails to start a service whose path has a space in it,
// which is every default install under Program Files.

func TestServiceCommandLineLeavesAPathWithoutSpacesAlone(t *testing.T) {
	line := serviceCommandLine(`C:\minilog\minilog-web-viewer.exe`, "--config", `C:\minilog\minilog.conf`)

	const want = `C:\minilog\minilog-web-viewer.exe --config C:\minilog\minilog.conf`
	if line != want {
		t.Errorf("serviceCommandLine = %q, want %q", line, want)
	}
}

func TestServiceCommandLineQuotesAPathWithSpaces(t *testing.T) {
	const exe = `C:\Program Files\minilog\minilog-web-viewer.exe`
	const cfg = `C:\ProgramData\minilog\minilog.conf`

	line := serviceCommandLine(exe, "--config", cfg)

	// The exe has to come back as one quoted token, or the SCM reads
	// "C:\Program" as the image and "Files\..." as an argument.
	if !strings.HasPrefix(line, `"`+exe+`"`) {
		t.Errorf("serviceCommandLine = %q, want it to start with the exe quoted", line)
	}
	if !strings.Contains(line, "--config") || !strings.Contains(line, cfg) {
		t.Errorf("serviceCommandLine = %q, want it to carry --config and %q", line, cfg)
	}
}

func TestServiceCommandLineQuotesAnArgumentWithSpaces(t *testing.T) {
	const cfg = `C:\Program Files\minilog\minilog.conf`

	line := serviceCommandLine(`C:\minilog\v.exe`, "--config", cfg)

	if !strings.Contains(line, `"`+cfg+`"`) {
		t.Errorf("serviceCommandLine = %q, want the config path quoted", line)
	}
}

func TestServiceCommandLineWithNoArgumentsIsJustTheExe(t *testing.T) {
	// minilog-web-viewer --install registers a command line with --config in
	// it, but the function takes a variadic and nothing stops a caller from
	// passing none; a stray trailing space would end up in the registry.
	line := serviceCommandLine(`C:\minilog\v.exe`)

	if line != `C:\minilog\v.exe` {
		t.Errorf("serviceCommandLine = %q, want the bare exe", line)
	}
}

// ── exitStatus ───────────────────────────────────────────────────────────────
//
// What svc.Run reports to the SCM. The service-specific exit code is the
// precondition for the recovery actions installService configures, so reporting
// a clean stop after a failed run is what would silently disable the restarts.

func TestExitStatusReportsNoFailureWhenRunSucceeded(t *testing.T) {
	failed, code := exitStatus(nil)

	if failed {
		t.Error("exitStatus(nil) reported a service-specific failure")
	}
	if code != 0 {
		t.Errorf("exitStatus(nil) code = %d, want 0", code)
	}
}

func TestExitStatusReportsAFailureWhenRunFailed(t *testing.T) {
	// Also reaches osLogError, which is how such a failure leaves any trace at
	// all on a machine where the viewer never runs interactively. It writes to
	// stderr and, if the source is registered, the Event Log; an unregistered
	// source is ignored, so this is safe to call here.
	failed, code := exitStatus(errors.New("cannot listen on 0.0.0.0:9514"))

	if !failed {
		t.Error("exitStatus(err) did not report a service-specific failure")
	}
	if code == 0 {
		t.Error("exitStatus(err) code = 0, which the SCM reads as a clean stop")
	}
}
