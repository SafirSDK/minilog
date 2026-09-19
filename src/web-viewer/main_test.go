// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// ── serve() startup reporting ─────────────────────────────────────────────────
//
// serve() must fail, rather than signal readiness and then stop, for anything
// that can go wrong at startup. Under the SCM the ready callback is what
// promotes the service from START_PENDING to RUNNING, so a premature call there
// makes `sc start` report success for a service that is about to die.

// freeAddr returns a loopback address that was free a moment ago. There is an
// unavoidable gap between releasing the port and serve() binding it, but the
// alternative — ":0" — leaves the caller no way to learn the chosen port.
func freeAddr(t *testing.T) string {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("cannot allocate a port: %v", err)
	}
	addr := ln.Addr().String()
	if err := ln.Close(); err != nil {
		t.Fatalf("cannot release the allocated port: %v", err)
	}
	return addr
}

// serveConfig writes a config with a single sink and returns its path.
func serveConfig(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	jsonl := filepath.Join(dir, "syslog.jsonl")
	if err := os.WriteFile(jsonl, nil, 0o600); err != nil {
		t.Fatalf("cannot create sink file: %v", err)
	}
	return writeConfig(t, dir, "[output.main]\njsonl_file = syslog.jsonl\n")
}

func TestServeSignalsReadyOnlyOnceListening(t *testing.T) {
	addr := freeAddr(t)
	stop := make(chan struct{})
	ready := make(chan struct{})
	errCh := make(chan error, 1)

	go func() { errCh <- serve(serveConfig(t), addr, stop, func() { close(ready) }) }()

	select {
	case <-ready:
	case err := <-errCh:
		t.Fatalf("serve returned before signalling ready: %v", err)
	case <-time.After(10 * time.Second):
		t.Fatal("serve never signalled ready")
	}

	// ready means the listener is already bound, so this must connect first try.
	resp, err := http.Get("http://" + addr + "/sinks")
	if err != nil {
		t.Fatalf("GET /sinks after ready: %v", err)
	}
	defer resp.Body.Close()

	var sinks []any
	if err := json.NewDecoder(resp.Body).Decode(&sinks); err != nil {
		t.Fatalf("decoding /sinks: %v", err)
	}
	if len(sinks) != 1 {
		t.Errorf("/sinks returned %d sinks, want 1", len(sinks))
	}

	close(stop)
	select {
	case err := <-errCh:
		if err != nil {
			t.Errorf("serve returned %v on a clean shutdown, want nil", err)
		}
	case <-time.After(10 * time.Second):
		t.Fatal("serve did not return after stop was closed")
	}
}

func TestServeUnreadableConfigFailsBeforeReady(t *testing.T) {
	ready := false
	err := serve(filepath.Join(t.TempDir(), "missing.conf"), freeAddr(t),
		make(chan struct{}), func() { ready = true })

	if err == nil {
		t.Fatal("serve accepted a config file that does not exist")
	}
	if !strings.Contains(err.Error(), "config error") {
		t.Errorf("error %q does not name the config as the cause", err)
	}
	if ready {
		t.Error("serve signalled ready despite failing to load the config")
	}
}

func TestServeUnbindableAddressFailsBeforeReady(t *testing.T) {
	// Hold the port for the lifetime of the test so serve() cannot bind it.
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("cannot allocate a port: %v", err)
	}
	defer ln.Close()

	addr := ln.Addr().String()
	ready := false
	err = serve(serveConfig(t), addr, make(chan struct{}), func() { ready = true })

	if err == nil {
		t.Fatalf("serve bound %s, which is already in use", addr)
	}
	if !strings.Contains(err.Error(), addr) {
		t.Errorf("error %q does not name the address it could not bind", err)
	}
	if ready {
		t.Error("serve signalled ready despite failing to bind")
	}
}

// ── Connection timeouts ───────────────────────────────────────────────────────
//
// http.Server has no timeouts by default, so a client that connects and then
// stops talking is held forever. Each held connection costs a goroutine, a file
// descriptor and a read buffer, and they accumulate until the process cannot
// accept anything. These tests run the real newServer with millisecond
// timeouts, so they prove the behaviour rather than restate the constants.

// startTimeoutServer runs newServer on a loopback listener and returns its
// address. handler may be nil, in which case a 204 handler is used.
func startTimeoutServer(
	t *testing.T, readHeader, read, idle time.Duration, handler http.Handler,
) string {
	t.Helper()
	if handler == nil {
		handler = http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			w.WriteHeader(http.StatusNoContent)
		})
	}

	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("cannot listen: %v", err)
	}

	srv := newServer(handler, readHeader, read, idle)
	go func() { _ = srv.Serve(ln) }()
	t.Cleanup(func() { _ = srv.Close() })

	return ln.Addr().String()
}

// readUntilClosed reads from conn until the server hangs up. It fails the test
// if conn's own deadline expires first, which is what "held open" looks like.
func readUntilClosed(t *testing.T, conn net.Conn, what string) {
	t.Helper()
	if err := conn.SetReadDeadline(time.Now().Add(10 * time.Second)); err != nil {
		t.Fatalf("cannot set a read deadline: %v", err)
	}

	buf := make([]byte, 512)
	for {
		if _, err := conn.Read(buf); err != nil {
			var ne net.Error
			if errors.As(err, &ne) && ne.Timeout() {
				t.Fatalf("%s: the server held the connection instead of closing it", what)
			}
			return // EOF or reset — the server closed it, which is the point
		}
	}
}

func TestServerClosesHalfOpenConnection(t *testing.T) {
	addr := startTimeoutServer(t, 200*time.Millisecond, time.Second, time.Second, nil)

	conn, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatalf("cannot connect: %v", err)
	}
	defer conn.Close()

	// The reproduction from the report: start a request and never finish the
	// headers — no terminating blank line — then go quiet.
	if _, err := conn.Write([]byte("GET /sinks HTTP/1.1\r\nHost: x\r\n")); err != nil {
		t.Fatalf("cannot write a partial header: %v", err)
	}

	readUntilClosed(t, conn, "half-open connection")
}

func TestServerClosesIdleKeepAliveConnection(t *testing.T) {
	addr := startTimeoutServer(t, time.Second, time.Second, 200*time.Millisecond, nil)

	conn, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatalf("cannot connect: %v", err)
	}
	defer conn.Close()

	// A complete request, answered, then silence. Without IdleTimeout the
	// keep-alive connection would stay up indefinitely.
	if _, err := conn.Write([]byte("GET /sinks HTTP/1.1\r\nHost: x\r\n\r\n")); err != nil {
		t.Fatalf("cannot write the request: %v", err)
	}

	readUntilClosed(t, conn, "idle keep-alive connection")
}

// The other half of the requirement: a response the server is still
// legitimately producing must not be cut off. A full-chain /search can take
// far longer than the read timeouts, so this fails if WriteTimeout is ever set
// to something a slow honest request can exceed.
func TestSlowResponseIsNotTruncated(t *testing.T) {
	const bodySize = 1 << 20 // 1 MiB, written in chunks after a long pause

	slow := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// Longer than every timeout newServer is given below.
		time.Sleep(600 * time.Millisecond)
		w.Header().Set("Content-Type", "application/json")
		chunk := make([]byte, 4096)
		for i := range chunk {
			chunk[i] = 'x'
		}
		for written := 0; written < bodySize; written += len(chunk) {
			if _, err := w.Write(chunk); err != nil {
				return
			}
			time.Sleep(time.Millisecond)
		}
	})

	addr := startTimeoutServer(t, 100*time.Millisecond, 200*time.Millisecond,
		200*time.Millisecond, slow)

	client := &http.Client{Timeout: 30 * time.Second}
	resp, err := client.Get(fmt.Sprintf("http://%s/search", addr))
	if err != nil {
		t.Fatalf("the slow response never arrived: %v", err)
	}
	defer resp.Body.Close()

	body, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatalf("the response was cut off mid-body: %v", err)
	}
	if len(body) != bodySize {
		t.Errorf("got %d bytes, want %d — the response was truncated", len(body), bodySize)
	}
}

// The values serve() actually uses. The behavioural tests above run with
// millisecond timeouts, so something has to say the shipped ones are set at
// all, and that WriteTimeout is left off on purpose rather than forgotten.
func TestServeUsesNonZeroTimeouts(t *testing.T) {
	srv := newServer(http.NewServeMux(), readHeaderTimeout, readTimeout, idleTimeout)

	if srv.ReadHeaderTimeout <= 0 {
		t.Error("ReadHeaderTimeout is unset")
	}
	if srv.ReadTimeout <= 0 {
		t.Error("ReadTimeout is unset")
	}
	if srv.IdleTimeout <= 0 {
		t.Error("IdleTimeout is unset")
	}
	if srv.ReadHeaderTimeout > srv.ReadTimeout {
		t.Errorf("ReadHeaderTimeout (%v) exceeds ReadTimeout (%v), so it can never fire",
			srv.ReadHeaderTimeout, srv.ReadTimeout)
	}
	if srv.WriteTimeout != 0 {
		t.Errorf("WriteTimeout is %v; it is left at 0 deliberately so that a slow "+
			"full-chain /search cannot be truncated", srv.WriteTimeout)
	}
}
