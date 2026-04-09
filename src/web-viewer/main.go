// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

package main

import (
	"context"
	"embed"
	"flag"
	"fmt"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"time"
)

//go:embed assets
var assets embed.FS

// version is set at build time via -ldflags "-X main.version=x.y.z".
var version = "dev"

// globalStop is closed to signal all goroutines to shut down.
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
	addr := flag.String("addr", ":8080", "HTTP listen address")
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

	run := func() {
		if err := serve(*configPath, *addr); err != nil {
			log.Printf("minilog-web-viewer: %v", err)
		}
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
	run()
}

func serve(configPath, addr string) error {
	sinks, err := loadSinks(configPath)
	if err != nil {
		return fmt.Errorf("config error: %w", err)
	}

	log.Printf("minilog-web-viewer starting — %d sink(s), listening on %s", len(sinks), addr)
	for _, s := range sinks {
		log.Printf("  sink %q → %s", s.Name, s.Path)
	}

	mux := http.NewServeMux()
	registerHandlers(mux, sinks)

	srv := &http.Server{
		Addr:    addr,
		Handler: mux,
	}

	// Shut down the HTTP server when globalStop is closed.
	go func() {
		<-globalStop
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = srv.Shutdown(ctx)
	}()

	if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		return err
	}
	return nil
}
