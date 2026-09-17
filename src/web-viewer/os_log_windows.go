// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

//go:build windows

package main

import (
	"log"

	"golang.org/x/sys/windows/svc/eventlog"
)

// eventLogSource is the Windows Event Log source registered by --install.  It
// is deliberately distinct from the C++ server's "minilog" source so the two
// services' entries can be told apart in Event Viewer.
const eventLogSource = serviceName

// eventLogID is the message id used for every entry.  EventCreate.exe serves as
// the message file and maps ids 1–1000 to a bare "%1", so an entry shows exactly
// the text passed in.  The Go binary has no message table of its own.
const eventLogID = 1

// installOsLog registers the Event Log source.  Called from installService.
func installOsLog() error {
	// Remove first so that re-running --install over an existing registration
	// succeeds — eventlog.Install fails outright if the key already exists.
	_ = eventlog.Remove(eventLogSource)
	return eventlog.InstallAsEventCreate(eventLogSource,
		eventlog.Error|eventlog.Warning|eventlog.Info)
}

// removeOsLog deregisters the Event Log source.  Called from uninstallService.
func removeOsLog() error {
	return eventlog.Remove(eventLogSource)
}

// osLogError logs msg to stderr and to the Windows Event Log.
//
// A service process started by the SCM has no console, so stderr goes nowhere;
// the Event Log is the only trace such a failure leaves.  The source handle is
// opened per call, mirroring the server's os_log_win.cpp — these are startup and
// failure paths, never a hot path.  A failure to open is ignored: it means the
// source is not registered, which is the normal case for an interactive run out
// of a build tree, where stderr is all that is wanted anyway.
func osLogError(msg string) {
	log.Printf("[ERROR] %s", msg)
	if l, err := eventlog.Open(eventLogSource); err == nil {
		_ = l.Error(eventLogID, msg)
		_ = l.Close()
	}
}

// osLogInfo logs msg to stderr and to the Windows Event Log.
func osLogInfo(msg string) {
	log.Printf("[INFO] %s", msg)
	if l, err := eventlog.Open(eventLogSource); err == nil {
		_ = l.Info(eventLogID, msg)
		_ = l.Close()
	}
}
