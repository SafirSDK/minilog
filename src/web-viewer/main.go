// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

package main

import (
	"context"
	"embed"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"time"
)

//go:embed assets
var assets embed.FS

// version is set at build time via -ldflags "-X main.version=x.y.z".
var version = "dev"

// globalStop is closed to signal all goroutines to shut down.  It is the stop
// channel main passes to serve(); tests pass their own.
// NOTE: this channel is single-use — closing it twice will panic.  Currently
// safe because serve() is only called once, but keep this in mind if the
// startup path is ever changed to support retries.
var globalStop = make(chan struct{})

func main() {
	// Default config path: same directory as the executable.
	exe, err := os.Executable()
	if err != nil {
		exe = "."
	}
	defaultConfig := filepath.Join(filepath.Dir(exe), "minilog.conf")

	configPath := flag.String("config", defaultConfig, "path to minilog.conf")
	addr := flag.String("addr", ":9514", "HTTP listen address")
	doInstall := flag.Bool("install", false, "install as a Windows service (Windows only)")
	doUninstall := flag.Bool("uninstall", false, "remove the Windows service (Windows only)")
	flag.Parse()

	if *doUninstall {
		if err := uninstallService(); err != nil {
			fmt.Fprintf(os.Stderr, "minilog-web-viewer: %v\n", err)
			os.Exit(1)
		}
		return
	}

	if *doInstall {
		exePath, err := os.Executable()
		if err != nil {
			fmt.Fprintf(os.Stderr, "minilog-web-viewer: cannot determine executable path: %v\n", err)
			os.Exit(1)
		}
		absConfig, err := filepath.Abs(*configPath)
		if err != nil {
			fmt.Fprintf(os.Stderr, "minilog-web-viewer: cannot resolve config path: %v\n", err)
			os.Exit(1)
		}
		if err := installService(exePath, absConfig, *addr); err != nil {
			fmt.Fprintf(os.Stderr, "minilog-web-viewer: %v\n", err)
			os.Exit(1)
		}
		return
	}

	// ready is called once the config has loaded and the listen address is
	// bound; the service wrapper uses it to delay reporting SERVICE_RUNNING
	// until startup has actually succeeded.
	run := func(ready func()) error {
		return serve(*configPath, *addr, globalStop, ready)
	}

	// Attempt to run as a Windows NT service. On Linux this is a no-op and
	// returns false immediately, so we fall through to interactive mode.
	if ok, err := tryRunAsService(run); ok {
		if err != nil {
			fmt.Fprintf(os.Stderr, "minilog-web-viewer: service error: %v\n", err)
			os.Exit(1)
		}
		return
	}

	// Interactive mode (Linux or Windows console).
	setupShutdown()
	if err := run(func() {}); err != nil {
		osLogError(fmt.Sprintf("minilog-web-viewer: %v", err))
		os.Exit(1)
	}
}

// serve loads the sinks, binds addr and serves until stop is closed.
//
// ready is called exactly once, after the config has loaded and the listen
// address is bound — i.e. after everything that can fail at startup has
// succeeded.  Every failure before that point is returned as an error rather
// than logged and swallowed, so that a service start reports failure instead of
// reporting success and then stopping a moment later.
func serve(configPath, addr string, stop <-chan struct{}, ready func()) error {
	sinks, err := loadSinks(configPath)
	if err != nil {
		return fmt.Errorf("config error: %w", err)
	}

	mux := http.NewServeMux()
	registerHandlers(mux, sinks)

	srv := &http.Server{
		Handler: mux,
	}

	// Bind explicitly rather than via ListenAndServe, so that an unusable listen
	// address is reported before ready() rather than after.
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return fmt.Errorf("cannot listen on %s: %w", addr, err)
	}

	log.Printf("minilog-web-viewer starting — %d sink(s), listening on %s", len(sinks), addr)
	for _, s := range sinks {
		log.Printf("  sink %q → %s", s.Name, s.Path)
	}

	// Shut down the HTTP server when stop is closed.
	go func() {
		<-stop
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = srv.Shutdown(ctx)
	}()

	ready()

	if err := srv.Serve(ln); err != nil && err != http.ErrServerClosed {
		return err
	}
	return nil
}
