// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

package main

import (
	"context"
	"embed"
	"errors"
	"flag"
	"fmt"
	"io"
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

// defaultStopTimeoutSeconds is how long --stop and --uninstall wait for the
// service process to go away.  Generous: the cost of waiting a little longer is
// nothing next to the cost of concluding too early that the executable is free
// to overwrite.
const defaultStopTimeoutSeconds = 30

// globalStop is closed to signal all goroutines to shut down.  It is the stop
// channel main passes to serve(); tests pass their own.
// NOTE: this channel is single-use — closing it twice will panic.  Currently
// safe because serve() is only called once, but keep this in mind if the
// startup path is ever changed to support retries.
var globalStop = make(chan struct{})

func main() {
	os.Exit(runMain(os.Args[1:], os.Stderr))
}

// runMain is main with the two things that make main untestable taken out: the
// arguments come in rather than off os.Args, and the exit code goes out rather
// than into os.Exit. Everything that decides which of --stop, --uninstall and
// --install happens, and what the exit code is, is therefore reachable from a
// test; what is not is the tail below, which serves HTTP until it is stopped.
//
// Exit codes: 2 for a usage error, which is what flag's own ExitOnError would
// have produced, and 1 for anything that failed after the arguments were
// understood.
func runMain(args []string, stderr io.Writer) int {
	// Default config path: same directory as the executable.
	exe, err := os.Executable()
	if err != nil {
		exe = "."
	}
	defaultConfig := filepath.Join(filepath.Dir(exe), "minilog.conf")

	// Its own FlagSet, not the package-level one: that one is global state, so
	// a second parse in the same process — which is every test after the first
	// — would redefine flags and panic. ContinueOnError for the same reason
	// ExitOnError is wrong here, that it would take the process down.
	fs := flag.NewFlagSet("minilog-web-viewer", flag.ContinueOnError)
	fs.SetOutput(stderr)
	configPath := fs.String("config", defaultConfig, "path to minilog.conf")
	doInstall := fs.Bool("install", false, "install as a Windows service (Windows only)")
	doStop := fs.Bool("stop", false,
		"stop the Windows service and wait for its process to exit (Windows only)")
	doUninstall := fs.Bool("uninstall", false, "remove the Windows service (Windows only)")
	timeout := fs.Int("timeout", defaultStopTimeoutSeconds,
		"seconds to wait for --stop and --uninstall")
	if err := fs.Parse(args); err != nil {
		// Parse has already written the error, or the usage, to stderr.
		//
		// --help is not an error: ExitOnError, which this FlagSet replaced,
		// special-cases flag.ErrHelp and exits 0, and the installer test
		// requires exactly that of the other two executables — running --help
		// is how it proves they are runnable at all. Returning 2 here would
		// make this the one that fails such a check.
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}

	if *doStop || *doUninstall {
		if *timeout <= 0 {
			fmt.Fprintln(stderr, "minilog-web-viewer: --timeout must be at least 1 second")
			return 1
		}
		// --uninstall wins when both are given: it stops the service on its way
		// to removing it, so doing both is what was asked for either way.
		stop := uninstallService
		if !*doUninstall {
			stop = stopService
		}
		if err := stop(time.Duration(*timeout) * time.Second); err != nil {
			fmt.Fprintf(stderr, "minilog-web-viewer: %v\n", err)
			return 1
		}
		return 0
	}

	if *doInstall {
		exePath, err := os.Executable()
		if err != nil {
			fmt.Fprintf(stderr, "minilog-web-viewer: cannot determine executable path: %v\n", err)
			return 1
		}
		// Absolute, because the service is started by the SCM with a working
		// directory of its own that has nothing to do with this one.
		absConfig, err := filepath.Abs(*configPath)
		if err != nil {
			fmt.Fprintf(stderr, "minilog-web-viewer: cannot resolve config path: %v\n", err)
			return 1
		}
		if err := installService(exePath, absConfig); err != nil {
			fmt.Fprintf(stderr, "minilog-web-viewer: %v\n", err)
			return 1
		}
		return 0
	}

	// ready is called once the config has loaded and the listen address is
	// bound; the service wrapper uses it to delay reporting SERVICE_RUNNING
	// until startup has actually succeeded.
	//
	// The listen address comes out of the config inside serve(), not out of
	// main, so that an unusable one fails where every other startup failure
	// does — inside the service main, where the SCM is told about it.
	run := func(ready func()) error {
		return serve(*configPath, globalStop, ready)
	}

	// Attempt to run as a Windows NT service. On Linux this is a no-op and
	// returns false immediately, so we fall through to interactive mode.
	if ok, err := tryRunAsService(run); ok {
		if err != nil {
			fmt.Fprintf(stderr, "minilog-web-viewer: service error: %v\n", err)
			return 1
		}
		return 0
	}

	// Interactive mode (Linux or Windows console).
	setupShutdown()
	if err := run(func() {}); err != nil {
		osLogError(fmt.Sprintf("minilog-web-viewer: %v", err))
		return 1
	}
	return 0
}

// Connection timeouts for the HTTP server.
//
// http.Server applies none of these by default, so a client that opens a
// connection and then stops talking is held open indefinitely — the slowloris
// case. Each held connection costs a goroutine, a file descriptor and a read
// buffer, and they accumulate until the process runs out of descriptors and
// stops accepting anything. It takes no traffic volume and no authentication to
// do, and on Windows the viewer is an auto-start LocalSystem service that stays
// down once it is wedged. gosec G112 flags the missing ReadHeaderTimeout
// specifically.
const (
	readHeaderTimeout = 10 * time.Second
	readTimeout       = 30 * time.Second
	idleTimeout       = 120 * time.Second
)

// newServer builds the HTTP server.
//
// The timeouts are parameters rather than read from the constants directly so
// that a test can demonstrate the behaviour in milliseconds instead of waiting
// out the real values.
//
// WriteTimeout is deliberately left unset. It is the one timeout that can cut
// off a response the server is still legitimately producing, and /search reads
// the whole rotation chain — up to max_size x max_files, a gigabyte at the
// documented defaults. The client would see a truncated body with no way to
// tell it from a complete one. The three timeouts above already close every
// connection an idle or half-open client can hold, so WriteTimeout buys nothing
// against that and only risks breaking a slow honest request.
func newServer(handler http.Handler, readHeader, read, idle time.Duration) *http.Server {
	return &http.Server{
		Handler:           handler,
		ReadHeaderTimeout: readHeader,
		ReadTimeout:       read,
		IdleTimeout:       idle,
	}
}

// serve loads the sinks, binds addr and serves until stop is closed.
//
// ready is called exactly once, after the config has loaded and the listen
// address is bound — i.e. after everything that can fail at startup has
// succeeded.  Every failure before that point is returned as an error rather
// than logged and swallowed, so that a service start reports failure instead of
// reporting success and then stopping a moment later.
func serve(configPath string, stop <-chan struct{}, ready func()) error {
	cfg, err := loadConfig(configPath)
	if err != nil {
		return fmt.Errorf("config error: %w", err)
	}
	sinks, addr := cfg.Sinks, cfg.Addr

	mux := http.NewServeMux()
	registerHandlers(mux, sinks)

	srv := newServer(withSecurityHeaders(mux), readHeaderTimeout, readTimeout, idleTimeout)

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
