// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

package main

import (
	"errors"
	"time"
)

// errTimedOut is what pollUntil returns when the condition never came true.
// Callers wrap it with what they were waiting for.
var errTimedOut = errors.New("timed out")

// pollUntil calls done every interval until it reports true, it reports an
// error, or timeout elapses.
//
// done is always called once before any waiting, so a zero timeout still gives
// the condition one chance: stopping a service that has already stopped must
// succeed however little patience the caller has.
//
// The last sleep is clamped to what is left of the timeout, so a coarse
// interval cannot push the return far past the deadline.
//
// This lives outside service_windows.go, free of anything Windows-specific, so
// that the timeout logic behind --stop and --uninstall is testable on the
// platform the project is developed on — the SCM code it serves is not.
func pollUntil(done func() (bool, error), timeout, interval time.Duration) error {
	deadline := time.Now().Add(timeout)
	for {
		ok, err := done()
		if err != nil {
			return err
		}
		if ok {
			return nil
		}

		remaining := time.Until(deadline)
		if remaining <= 0 {
			return errTimedOut
		}
		if interval < remaining {
			time.Sleep(interval)
		} else {
			time.Sleep(remaining)
		}
	}
}
