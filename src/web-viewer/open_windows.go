// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

//go:build windows

package main

import (
	"os"

	"golang.org/x/sys/windows"
)

// openShared opens a log file for reading without getting in the way of the
// server's rotation.
//
// os.Open shares only read and write. While such a handle is open, minilog's
// rotation cannot rename the file — the rename fails, and the server postpones
// the rotation until its next retry. FILE_SHARE_DELETE allows the
// rename (and the removal of the oldest generation); the handle goes on
// reading the same file under its new name. The cli-viewer opens files the
// same way, for the same reason.
func openShared(path string) (*os.File, error) {
	p, err := windows.UTF16PtrFromString(path)
	if err != nil {
		return nil, &os.PathError{Op: "open", Path: path, Err: err}
	}
	h, err := windows.CreateFile(p,
		windows.GENERIC_READ,
		windows.FILE_SHARE_READ|windows.FILE_SHARE_WRITE|windows.FILE_SHARE_DELETE,
		nil,
		windows.OPEN_EXISTING,
		windows.FILE_ATTRIBUTE_NORMAL,
		0)
	if err != nil {
		return nil, &os.PathError{Op: "open", Path: path, Err: err}
	}
	return os.NewFile(uintptr(h), path), nil
}
