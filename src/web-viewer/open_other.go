// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

//go:build !windows

package main

import "os"

// openShared opens a log file for reading. Off Windows an open file never
// blocks a rename or an unlink, so a plain open is enough; see the Windows
// version for why it is not there.
func openShared(path string) (*os.File, error) {
	return os.Open(path)
}
