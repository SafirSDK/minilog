// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

//go:build !windows

package main

import (
	"log"
	"os"
	"os/signal"
	"syscall"
)

// tryRunAsService is a no-op on non-Windows platforms.
// It always returns false (not running as a service).
func tryRunAsService(_ func()) (bool, error) {
	return false, nil
}

// installService is a no-op on non-Windows platforms.
func installService(_, _, _ string) error {
	log.Println("--install is only supported on Windows")
	return nil
}

// uninstallService is a no-op on non-Windows platforms.
func uninstallService() error {
	log.Println("--uninstall is only supported on Windows")
	return nil
}

// setupShutdown installs a SIGTERM/SIGINT handler that closes globalStop,
// which causes the HTTP server to shut down gracefully.
func setupShutdown() {
	ch := make(chan os.Signal, 1)
	signal.Notify(ch, syscall.SIGTERM, syscall.SIGINT)
	go func() {
		<-ch
		close(globalStop)
	}()
}
