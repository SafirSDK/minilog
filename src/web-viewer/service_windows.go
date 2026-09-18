// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

//go:build windows

package main

import (
	"errors"
	"fmt"
	"sync"
	"time"

	"golang.org/x/sys/windows"
	"golang.org/x/sys/windows/svc"
	"golang.org/x/sys/windows/svc/mgr"
)

const (
	serviceName    = "minilog-web-viewer"
	serviceDisplay = "minilog Web Viewer"
	serviceDesc    = "Web-based log viewer for minilog. https://github.com/SafirSDK/minilog"
)

const (
	// Upper bound on config load plus bind, reported to the SCM as the
	// SERVICE_START_PENDING wait hint.  Startup is milliseconds in practice.
	startupWaitHint = 10000

	// How long to wait for the HTTP server to finish after a stop request.
	stopTimeout = 10 * time.Second

	// How often the SCM is asked whether the service has stopped yet.  Short
	// enough that stopping a healthy service feels immediate, long enough not
	// to spin.
	stopPollInterval = 200 * time.Millisecond
)

// Recovery actions configured at install time: restart twice, then leave the
// service stopped.  The SCM repeats the last action for every further failure,
// so the trailing NoAction is what stops the restarts after the second attempt.
//
// The reset period is what separates the two failure modes.  A service that
// fails at startup fails again within seconds, so the counter keeps climbing
// and it stops after two attempts, leaving one clearly stopped service and a
// short, readable Event Log rather than an endless restart loop.  A service
// that crashes after running healthily for longer than the reset period is
// treated as a fresh first failure each time, so an intermittent fault is
// always retried instead of exhausting its restarts.
const (
	restartDelay       = 5 * time.Second
	failureResetPeriod = 300 // seconds
)

// windowsService implements svc.Handler.
type windowsService struct {
	// run serves until the stop channel is closed, calling ready once startup
	// has succeeded.  It returns nil on a clean shutdown.
	run func(ready func()) error
}

func (s *windowsService) Execute(args []string, req <-chan svc.ChangeRequest, status chan<- svc.Status) (bool, uint32) {
	status <- svc.Status{State: svc.StartPending, WaitHint: startupWaitHint}

	// readyCh is closed once the viewer has loaded its config and bound its
	// listen address.  Reporting svc.Running before that would make
	// `sc start minilog-web-viewer` succeed even when startup is about to fail.
	readyCh := make(chan struct{})
	var readyOnce sync.Once
	notifyReady := func() { readyOnce.Do(func() { close(readyCh) }) }

	// Start the HTTP server in a goroutine.  runErr is written before done is
	// closed, so it is safe to read on any path that has observed done.
	done := make(chan struct{})
	var runErr error
	go func() {
		defer close(done)
		runErr = s.run(notifyReady)
	}()

	// Set to nil once fired, so the closed channel stops selecting.
	ready := (<-chan struct{})(readyCh)

	for {
		select {
		case <-ready:
			ready = nil
			status <- svc.Status{State: svc.Running, Accepts: svc.AcceptStop | svc.AcceptShutdown}
		case c := <-req:
			switch c.Cmd {
			case svc.Stop, svc.Shutdown:
				status <- svc.Status{State: svc.StopPending}
				// Signal shutdown to the HTTP server via the global stop channel.
				close(globalStop)
				select {
				case <-done:
					return exitStatus(runErr)
				case <-time.After(stopTimeout):
					// Stopping on request, not failing: report a clean stop.
					return false, 0
				}
			default:
				// Ignore unhandled commands.
			}
		case <-done:
			return exitStatus(runErr)
		}
	}
}

// exitStatus maps the outcome of run() onto the status svc.Run reports to the
// SCM.  A non-zero service-specific exit code is what tells the SCM the service
// failed rather than stopped cleanly, and is the precondition for the recovery
// actions configured by installService to fire.  The reason goes to the Event
// Log, which on a machine where nobody can run the viewer interactively is the
// only place a startup failure leaves a trace.
func exitStatus(err error) (bool, uint32) {
	if err == nil {
		return false, 0
	}
	osLogError(fmt.Sprintf("minilog-web-viewer: %v", err))
	return true, 1
}

// tryRunAsService attempts to run as a Windows NT service.
// Returns true if the process was started by the SCM (and has now exited the
// service main), false if running interactively.
func tryRunAsService(run func(ready func()) error) (bool, error) {
	isService, err := svc.IsWindowsService()
	if err != nil {
		return false, fmt.Errorf("cannot determine if running as service: %w", err)
	}
	if !isService {
		return false, nil
	}
	err = svc.Run(serviceName, &windowsService{run: run})
	if err != nil {
		return true, fmt.Errorf("service run failed: %w", err)
	}
	return true, nil
}

// installService registers the binary as a Windows NT auto-start service.
func installService(exePath, configPath, addr string) error {
	m, err := mgr.Connect()
	if err != nil {
		return fmt.Errorf("cannot connect to SCM: %w", err)
	}
	defer m.Disconnect()

	// Check if the service already exists.
	s, err := m.OpenService(serviceName)
	if err == nil {
		s.Close()
		return fmt.Errorf("service %q already exists", serviceName)
	}

	s, err = m.CreateService(serviceName, exePath, mgr.Config{
		DisplayName: serviceDisplay,
		Description: serviceDesc,
		StartType:   mgr.StartAutomatic,
	}, "--config", configPath, "--addr", addr)
	if err != nil {
		return fmt.Errorf("cannot create service: %w", err)
	}
	defer s.Close()

	// Recovery actions and the Event Log source are what make a failure of this
	// service visible and self-healing; neither is worth failing the install
	// over, so they are reported and stepped over rather than returned.
	if err := s.SetRecoveryActions([]mgr.RecoveryAction{
		{Type: mgr.ServiceRestart, Delay: restartDelay},
		{Type: mgr.ServiceRestart, Delay: restartDelay},
		{Type: mgr.NoAction},
	}, failureResetPeriod); err != nil {
		osLogError(fmt.Sprintf("cannot configure recovery actions: %v", err))
	}

	// Without this the SCM runs the recovery actions only when the process dies
	// outright.  Execute reports its failures as a service-specific exit code,
	// which counts as a failure only when this flag is set.
	if err := s.SetRecoveryActionsOnNonCrashFailures(true); err != nil {
		osLogError(fmt.Sprintf("cannot enable recovery actions on non-crash failures: %v", err))
	}

	if err := installOsLog(); err != nil {
		osLogError(fmt.Sprintf("cannot register event log source %q: %v", eventLogSource, err))
	}

	osLogInfo(fmt.Sprintf("Service %q installed successfully", serviceName))
	return nil
}

// stopAndWait asks the service to stop and does not return until its process
// has exited.
//
// Two waits, because the SCM state and the process are not the same thing.  A
// service reports svc.Stopped from its own stop handler, before it has unwound
// and released its image file, and a running executable can be neither
// overwritten nor deleted.  Callers about to copy a new build over the old one,
// or to delete the service, care about the second wait, not the first.
//
// Fails if the process is still there when the timeout runs out — better a
// caller that knows the stop failed than one that goes on to overwrite a file
// still in use.
func stopAndWait(s *mgr.Service, timeout time.Duration) error {
	status, err := s.Query()
	if err != nil {
		return fmt.Errorf("cannot query service %q: %w", serviceName, err)
	}
	if status.State == svc.Stopped {
		return nil
	}

	// Opened before the stop is requested: once the process has exited its id
	// means nothing, and may already have been reused by something else.  A
	// handle that cannot be opened is not fatal — the SCM state is then all
	// there is to go on.
	var process windows.Handle
	if status.ProcessId != 0 {
		if h, err := windows.OpenProcess(windows.SYNCHRONIZE, false, status.ProcessId); err == nil {
			process = h
			defer func() { _ = windows.CloseHandle(process) }()
		}
	}

	if status.State != svc.StopPending {
		if _, err := s.Control(svc.Stop); err != nil &&
			!errors.Is(err, windows.ERROR_SERVICE_NOT_ACTIVE) {
			return fmt.Errorf("cannot stop service %q: %w", serviceName, err)
		}
	}

	// Control is asynchronous: it returns with the service still in
	// svc.StopPending.
	deadline := time.Now().Add(timeout)
	err = pollUntil(func() (bool, error) {
		st, err := s.Query()
		if err != nil {
			return false, fmt.Errorf("cannot query service %q: %w", serviceName, err)
		}
		return st.State == svc.Stopped, nil
	}, timeout, stopPollInterval)
	if err != nil {
		if errors.Is(err, errTimedOut) {
			return fmt.Errorf("service %q did not stop within %s", serviceName, timeout)
		}
		return err
	}

	if process == 0 {
		return nil
	}

	remaining := time.Until(deadline)
	if remaining < 0 {
		remaining = 0
	}
	event, err := windows.WaitForSingleObject(process, uint32(remaining.Milliseconds()))
	if err != nil {
		return fmt.Errorf("cannot wait for the %q process to exit: %w", serviceName, err)
	}
	if event != windows.WAIT_OBJECT_0 {
		return fmt.Errorf("service %q reported itself stopped but its process was still "+
			"running after %s", serviceName, timeout)
	}
	return nil
}

// stopService stops the Windows NT service and waits for its process to exit.
// A service that is not registered, or already stopped, is success: the state
// the caller asked for already holds.
func stopService(timeout time.Duration) error {
	m, err := mgr.Connect()
	if err != nil {
		return fmt.Errorf("cannot connect to SCM: %w", err)
	}
	defer m.Disconnect()

	s, err := m.OpenService(serviceName)
	if err != nil {
		if errors.Is(err, windows.ERROR_SERVICE_DOES_NOT_EXIST) {
			// Nothing registered is nothing to stop.  An upgrade script should
			// not have to know whether this machine has minilog on it yet.
			osLogInfo(fmt.Sprintf("Service %q is not registered; nothing to stop", serviceName))
			return nil
		}
		return fmt.Errorf("cannot open service %q: %w", serviceName, err)
	}
	defer s.Close()

	if err := stopAndWait(s, timeout); err != nil {
		return err
	}

	osLogInfo(fmt.Sprintf("Service %q stopped", serviceName))
	return nil
}

// uninstallService stops and removes the Windows NT service.
func uninstallService(timeout time.Duration) error {
	m, err := mgr.Connect()
	if err != nil {
		return fmt.Errorf("cannot connect to SCM: %w", err)
	}
	defer m.Disconnect()

	s, err := m.OpenService(serviceName)
	if err != nil {
		return fmt.Errorf("service %q not found: %w", serviceName, err)
	}
	defer s.Close()

	// Deleting a service that is still running only marks it for deletion: the
	// registration lingers, and the next CreateService fails with
	// ERROR_SERVICE_MARKED_FOR_DELETE.  Waiting here is what makes an install
	// that follows an uninstall reliable.
	if err := stopAndWait(s, timeout); err != nil {
		return err
	}

	if err := s.Delete(); err != nil {
		return fmt.Errorf("cannot delete service: %w", err)
	}

	// Log before removing the source, so this entry still reaches the Event Log.
	osLogInfo(fmt.Sprintf("Service %q uninstalled", serviceName))
	if err := removeOsLog(); err != nil {
		osLogError(fmt.Sprintf("cannot remove event log source %q: %v", eventLogSource, err))
	}
	return nil
}

// setupShutdown is a no-op on Windows when running as a service — shutdown is
// handled via the SCM stop command in windowsService.Execute.
// When running interactively on Windows, the console CTRL+C handler is set up
// in main.go via os/signal, so nothing extra is needed here.
func setupShutdown() {}
