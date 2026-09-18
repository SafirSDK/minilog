// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

package main

import (
	"errors"
	"testing"
	"time"
)

// ── pollUntil ────────────────────────────────────────────────────────────────
//
// The timeout logic behind --stop and --uninstall. Those wait on the Windows
// SCM, which cannot be made to refuse a stop without a fault-injection hook, so
// the waiting itself is tested here instead — with a condition the test owns.

func TestPollUntilReturnsImmediatelyWhenAlreadySatisfied(t *testing.T) {
	calls := 0
	start := time.Now()

	err := pollUntil(func() (bool, error) {
		calls++
		return true, nil
	}, 10*time.Second, 50*time.Millisecond)

	if err != nil {
		t.Fatalf("pollUntil: %v", err)
	}
	if calls != 1 {
		t.Errorf("condition checked %d times, want 1", calls)
	}
	if elapsed := time.Since(start); elapsed > 5*time.Second {
		t.Errorf("took %s, want next to nothing", elapsed)
	}
}

func TestPollUntilChecksOnceWithZeroTimeout(t *testing.T) {
	// Stopping a service that has already stopped must succeed even when the
	// caller allows no time at all for it.
	calls := 0
	if err := pollUntil(func() (bool, error) {
		calls++
		return true, nil
	}, 0, 50*time.Millisecond); err != nil {
		t.Fatalf("pollUntil: %v", err)
	}
	if calls != 1 {
		t.Errorf("condition checked %d times, want 1", calls)
	}

	calls = 0
	err := pollUntil(func() (bool, error) {
		calls++
		return false, nil
	}, 0, 50*time.Millisecond)
	if !errors.Is(err, errTimedOut) {
		t.Errorf("err = %v, want errTimedOut", err)
	}
	if calls != 1 {
		t.Errorf("condition checked %d times, want 1", calls)
	}
}

func TestPollUntilSucceedsOnceTheConditionComesTrue(t *testing.T) {
	calls := 0

	err := pollUntil(func() (bool, error) {
		calls++
		return calls >= 3, nil
	}, 10*time.Second, time.Millisecond)

	if err != nil {
		t.Fatalf("pollUntil: %v", err)
	}
	if calls != 3 {
		t.Errorf("condition checked %d times, want 3", calls)
	}
}

func TestPollUntilGivesUpAfterTheTimeout(t *testing.T) {
	calls := 0
	start := time.Now()

	err := pollUntil(func() (bool, error) {
		calls++
		return false, nil
	}, 200*time.Millisecond, 20*time.Millisecond)

	if !errors.Is(err, errTimedOut) {
		t.Fatalf("err = %v, want errTimedOut", err)
	}
	if elapsed := time.Since(start); elapsed < 200*time.Millisecond {
		t.Errorf("gave up after %s, want at least the 200ms timeout", elapsed)
	}
	if calls < 2 {
		t.Errorf("condition checked %d times, want repeated polling", calls)
	}
}

func TestPollUntilDoesNotOvershootACoarseInterval(t *testing.T) {
	// The last sleep is clamped to the remaining time, so an interval longer
	// than the timeout cannot make the caller wait far past its deadline.
	start := time.Now()

	err := pollUntil(func() (bool, error) { return false, nil },
		100*time.Millisecond, 30*time.Second)

	if !errors.Is(err, errTimedOut) {
		t.Fatalf("err = %v, want errTimedOut", err)
	}
	elapsed := time.Since(start)
	if elapsed < 100*time.Millisecond {
		t.Errorf("gave up after %s, want at least the 100ms timeout", elapsed)
	}
	if elapsed > 20*time.Second {
		t.Errorf("waited %s, want the sleep clamped to the deadline", elapsed)
	}
}

func TestPollUntilReturnsTheConditionsError(t *testing.T) {
	// A service that cannot be queried at all is a different failure from one
	// that is taking its time, and must not be reported as a timeout.
	want := errors.New("cannot query service")

	err := pollUntil(func() (bool, error) { return false, want },
		10*time.Second, time.Millisecond)

	if !errors.Is(err, want) {
		t.Errorf("err = %v, want %v", err, want)
	}
}
