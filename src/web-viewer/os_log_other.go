// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

//go:build !windows

package main

import "log"

// osLogError logs msg to stderr.  There is no OS log integration off Windows —
// the viewer runs in the foreground there, or under an init system that
// captures stderr itself.
func osLogError(msg string) {
	log.Printf("[ERROR] %s", msg)
}

// osLogInfo logs msg to stderr.
func osLogInfo(msg string) {
	log.Printf("[INFO] %s", msg)
}
