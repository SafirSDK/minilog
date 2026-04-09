// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

//go:build windows

package main

import (
	"fmt"
	"log"
	"time"

	"golang.org/x/sys/windows/svc"
	"golang.org/x/sys/windows/svc/mgr"
)

const (
	serviceName    = "minilog-web-viewer"
	serviceDisplay = "minilog Web Viewer"
	serviceDesc    = "Web-based log viewer for minilog. https://github.com/SafirSDK/minilog"
)

// windowsService implements svc.Handler.
type windowsService struct {
	run func()
}

func (s *windowsService) Execute(args []string, req <-chan svc.ChangeRequest, status chan<- svc.Status) (bool, uint32) {
	status <- svc.Status{State: svc.StartPending}

	// Start the HTTP server in a goroutine.
	done := make(chan struct{})
	go func() {
		s.run()
		close(done)
	}()

	status <- svc.Status{State: svc.Running, Accepts: svc.AcceptStop | svc.AcceptShutdown}

	for {
		select {
		case c := <-req:
			switch c.Cmd {
			case svc.Stop, svc.Shutdown:
				status <- svc.Status{State: svc.StopPending}
				// Signal shutdown to the HTTP server via the global stop channel.
				close(globalStop)
				select {
				case <-done:
				case <-time.After(10 * time.Second):
				}
				return false, 0
			default:
				// Ignore unhandled commands.
			}
		case <-done:
			return false, 0
		}
	}
}

// tryRunAsService attempts to run as a Windows NT service.
// Returns true if the process was started by the SCM (and has now exited the
// service main), false if running interactively.
func tryRunAsService(run func()) (bool, error) {
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

	log.Printf("Service %q installed successfully", serviceName)
	return nil
}

// uninstallService stops and removes the Windows NT service.
func uninstallService() error {
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

	// Best-effort stop.
	_, _ = s.Control(svc.Stop)
	time.Sleep(500 * time.Millisecond)

	if err := s.Delete(); err != nil {
		return fmt.Errorf("cannot delete service: %w", err)
	}

	log.Printf("Service %q uninstalled", serviceName)
	return nil
}

// setupShutdown is a no-op on Windows when running as a service — shutdown is
// handled via the SCM stop command in windowsService.Execute.
// When running interactively on Windows, the console CTRL+C handler is set up
// in main.go via os/signal, so nothing extra is needed here.
func setupShutdown() {}
