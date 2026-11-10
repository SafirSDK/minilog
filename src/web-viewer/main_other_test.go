// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

//go:build !windows

package main

import (
	"log"
	"os"
	"strings"
	"testing"
)

// The runMain dispatch tests that reach a service function, and so must not run
// on Windows: there they would call the real SCM. An earlier version of this
// file had no build tag, and on the Windows coverage runner it stopped and then
// deleted the actual minilog-web-viewer service — the log read
// `Service "minilog-web-viewer" stopped` and `... uninstalled` — which also
// breaks the installer test that runs later in the same job and checks that
// nothing is registered beforehand.
//
// Off Windows these call the no-op stubs in service_other.go, which say which
// of them ran. The dispatch under test is in runMain and is identical on both
// platforms, so testing it here covers it everywhere; what is Windows-specific
// is the service work itself, and the installer tests cover that against a real
// SCM. Anything added here that touches installService, stopService or
// uninstallService belongs in this file and not in main_test.go.
// captureLog redirects the log package, which is where service_other.go's
// stubs say which of them ran, and returns what was written.
func captureLog(t *testing.T) func() string {
	t.Helper()
	var buf strings.Builder
	flags := log.Flags()
	log.SetOutput(&buf)
	log.SetFlags(0)
	t.Cleanup(func() {
		log.SetOutput(os.Stderr)
		log.SetFlags(flags)
	})
	return buf.String
}

func TestRunMainTimeoutIsOnlyCheckedWhenItIsUsed(t *testing.T) {
	// --timeout applies to --stop and --uninstall. A nonsensical one passed
	// without either must not fail a plain --install, which never waits.
	defer captureLog(t)()
	var stderr strings.Builder

	if code := runMain([]string{"--install", "--timeout=0"}, &stderr); code != 0 {
		t.Errorf("runMain(--install --timeout=0) = %d, want 0 (stderr: %q)", code, stderr.String())
	}
}

func TestRunMainStopChoosesStopService(t *testing.T) {
	logged := captureLog(t)
	var stderr strings.Builder

	if code := runMain([]string{"--stop"}, &stderr); code != 0 {
		t.Errorf("runMain(--stop) = %d, want 0 (stderr: %q)", code, stderr.String())
	}
	if !strings.Contains(logged(), "--stop") {
		t.Errorf("log = %q, want the --stop stub to have run", logged())
	}
}

func TestRunMainUninstallWinsOverStop(t *testing.T) {
	// Both given: --uninstall stops the service on its way to removing it, so
	// doing both is what was asked for either way. Picking stopService would
	// leave the registration behind and the caller none the wiser.
	logged := captureLog(t)
	var stderr strings.Builder

	if code := runMain([]string{"--stop", "--uninstall"}, &stderr); code != 0 {
		t.Errorf("runMain(--stop --uninstall) = %d, want 0 (stderr: %q)", code, stderr.String())
	}
	if !strings.Contains(logged(), "--uninstall") {
		t.Errorf("log = %q, want the --uninstall stub to have run", logged())
	}
	if strings.Contains(logged(), "--stop is only") {
		t.Errorf("log = %q, want --uninstall to have been chosen instead of --stop", logged())
	}
}

func TestRunMainInstallSucceedsWithARelativeConfig(t *testing.T) {
	// The path is made absolute before it is registered, because the SCM starts
	// the service with a working directory of its own. The stub cannot show the
	// resolved path, so this asserts only that a relative one is accepted —
	// filepath.Abs fails on no input a test can supply.
	defer captureLog(t)()
	var stderr strings.Builder

	if code := runMain([]string{"--install", "--config", "minilog.conf"}, &stderr); code != 0 {
		t.Errorf("runMain(--install) = %d, want 0 (stderr: %q)", code, stderr.String())
	}
}
