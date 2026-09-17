// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

package main

import (
	"encoding/json"
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
