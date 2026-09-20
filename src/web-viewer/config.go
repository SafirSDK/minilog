// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

package main

import (
	"bufio"
	"fmt"
	"net"
	"os"
	"strconv"
	"strings"
)

// Sink represents a single minilog [output.<name>] section that has a jsonl_file configured.
type Sink struct {
	Name     string
	Path     string
	MaxFiles int // maximum number of rotated generations (default 10)
}

// Config is everything minilog-web-viewer takes from minilog.conf: the sinks it
// can show, from the server's [output.*] sections, and the address it listens
// on, from its own [web_viewer] section.
type Config struct {
	Sinks []Sink
	Addr  string // as passed to net.Listen — "host:port", or ":port" for every interface
}

// defaultPort is the listen port when [web_viewer] does not name one. The
// installer's shortcuts read the port back out of the installed config, so this
// is the one place the default lives.
const defaultPort = "9514"

// loadConfig parses a minilog INI config file.
//
// The file belongs to the server, so the parsing here is deliberately as
// literal as Boost's INI parser: ';' and '#' open a comment at the start of a
// line only, and a value runs to the end of its line.
func loadConfig(configPath string) (Config, error) {
	f, err := os.Open(configPath)
	if err != nil {
		return Config{}, fmt.Errorf("cannot open config %q: %w", configPath, err)
	}
	defer f.Close()

	const outputPrefix = "output."
	const viewerSection = "web_viewer"

	type section struct {
		name        string
		jsonlFile   string
		maxFiles    int
		maxFilesSet bool
	}

	var (
		sections    []section
		current     *section
		scanner     = bufio.NewScanner(f)
		inOutputSec bool
		inViewerSec bool
		viewerHost  string
		viewerPort  string
	)

	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())

		// Skip blank lines and comments.
		if line == "" || line[0] == ';' || line[0] == '#' {
			continue
		}

		// Section header: [output.main], [server], [web_viewer], ...
		if line[0] == '[' {
			end := strings.Index(line, "]")
			if end < 0 {
				continue
			}
			name := strings.TrimSpace(line[1:end])
			current = nil
			inOutputSec = false
			inViewerSec = false
			switch {
			case strings.HasPrefix(name, outputPrefix):
				sinkName := name[len(outputPrefix):]
				sections = append(sections, section{name: sinkName})
				current = &sections[len(sections)-1]
				inOutputSec = true
			case name == viewerSection:
				inViewerSec = true
			}
			continue
		}

		// Key = value
		if !inOutputSec && !inViewerSec {
			continue
		}
		eq := strings.Index(line, "=")
		if eq < 0 {
			continue
		}
		key := strings.TrimSpace(line[:eq])
		// A value runs to the end of the line. ';' and '#' only start a comment
		// at the start of a line, which is what Boost's INI parser does in the
		// server and what Python's configparser does in the cli-viewer — and the
		// server is what defines this file format. Stripping them here used to
		// make "jsonl_file = hash#name.jsonl" open "hash" while the server wrote
		// "hash#name.jsonl", and a file that is not there reads as an idle sink
		// rather than as an error.
		val := strings.TrimSpace(line[eq+1:])

		if inViewerSec {
			switch key {
			case "host":
				viewerHost = val
			case "port":
				viewerPort = val
			}
			continue
		}

		if current == nil {
			continue
		}
		switch key {
		case "jsonl_file":
			current.jsonlFile = val
		case "max_files":
			if n, err := strconv.Atoi(val); err == nil && n >= 0 {
				current.maxFiles = n
				current.maxFilesSet = true
			}
		}
	}
	if err := scanner.Err(); err != nil {
		return Config{}, fmt.Errorf("error reading config: %w", err)
	}

	const defaultMaxFiles = 10
	// unlimitedMaxFiles is used when max_files = 0 (unlimited rotation) is set in
	// the server config. The viewer probes this many generations; files that do not
	// exist are simply skipped, so a large value is safe and effectively unlimited.
	const unlimitedMaxFiles = 1000

	var sinks []Sink
	for _, s := range sections {
		if s.jsonlFile != "" {
			mf := defaultMaxFiles
			if s.maxFilesSet {
				if s.maxFiles == 0 {
					mf = unlimitedMaxFiles
				} else {
					mf = s.maxFiles
				}
			}
			// The path is used exactly as configured. minilog's own config loader
			// rejects a relative text_file / jsonl_file, so a path that reaches
			// here is absolute; resolving one here would only reintroduce the
			// disagreement about what it is relative to that rejection removed.
			sinks = append(sinks, Sink{Name: s.name, Path: s.jsonlFile, MaxFiles: mf})
		}
	}
	if len(sinks) == 0 {
		return Config{}, fmt.Errorf("no output sections with a jsonl_file found in %q", configPath)
	}

	addr, err := listenAddr(viewerHost, viewerPort)
	if err != nil {
		return Config{}, err
	}
	return Config{Sinks: sinks, Addr: addr}, nil
}

// listenAddr assembles the [web_viewer] host and port into an address for
// net.Listen.
//
// An absent or empty host means every interface and must produce ":9514", not
// "0.0.0.0:9514": the latter is IPv4 only, and the --addr default this replaced
// listened on both families. net.JoinHostPort("", port) gives exactly that, and
// brackets an IPv6 literal host on the way.
//
// The host is not validated. Whether it names an interface this machine has is
// a question only the bind can answer, and serve() reports a failed bind by
// name; checking here would only reject values net.Listen accepts, such as a
// hostname. The port is validated, because "80a" would otherwise reach
// net.Listen as an address whose error says nothing about which key is wrong.
func listenAddr(host, port string) (string, error) {
	if port == "" {
		port = defaultPort
	}
	n, err := strconv.Atoi(port)
	if err != nil || n < 0 || n > 65535 {
		return "", fmt.Errorf("[web_viewer] port: %q is not a port number (0-65535)", port)
	}
	return net.JoinHostPort(host, port), nil
}
