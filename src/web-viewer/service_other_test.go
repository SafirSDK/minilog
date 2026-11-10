// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

//go:build !windows

package main

import "testing"

// The mirror of TestTryRunAsServiceReportsInteractiveWhenNotStartedByTheSCM in
// service_windows_test.go. On this platform there is no SCM to be started by,
// so the answer is always "interactive" — and that is what makes runMain fall
// through to serving HTTP rather than waiting for a service dispatcher that
// will never arrive.
func TestTryRunAsServiceIsAlwaysInteractiveOffWindows(t *testing.T) {
	called := false

	isService, err := tryRunAsService(func(ready func()) error {
		called = true
		return nil
	})

	if err != nil {
		t.Fatalf("tryRunAsService: %v", err)
	}
	if isService {
		t.Error("tryRunAsService reported a service; there are none on this platform")
	}
	if called {
		t.Error("tryRunAsService ran the server; off Windows it must only report, not run")
	}
}

// setupShutdown is deliberately not tested: it closes globalStop, which
// main.go documents as single-use and panicking if closed twice, and it
// installs a process-wide signal handler. Neither belongs in a unit test
// sharing a process with the rest of this package.
