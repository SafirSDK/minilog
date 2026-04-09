// Copyright (c) 2026 Saab AB (https://github.com/SafirSDK/minilog)
// SPDX-License-Identifier: MIT

package main

import (
	"bufio"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// Sink represents a single minilog [output.<name>] section that has a jsonl_file configured.
type Sink struct {
	Name     string
	Path     string
	MaxFiles int // maximum number of rotated generations (default 10)
}

// loadSinks parses a minilog INI config file and returns all output sections
// that have a jsonl_file configured.
func loadSinks(configPath string) ([]Sink, error) {
	f, err := os.Open(configPath)
	if err != nil {
		return nil, fmt.Errorf("cannot open config %q: %w", configPath, err)
	}
	defer f.Close()

	const outputPrefix = "output."

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
	)

	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())

		// Skip blank lines and comments.
		if line == "" || line[0] == ';' || line[0] == '#' {
			continue
		}

		// Section header: [output.main], [server], [forwarding], ...
		if line[0] == '[' {
			end := strings.Index(line, "]")
			if end < 0 {
				continue
			}
			name := strings.TrimSpace(line[1:end])
			if strings.HasPrefix(name, outputPrefix) {
				sinkName := name[len(outputPrefix):]
				sections = append(sections, section{name: sinkName})
				current = &sections[len(sections)-1]
				inOutputSec = true
			} else {
				current = nil
				inOutputSec = false
			}
			continue
		}

		// Key = value
		if !inOutputSec || current == nil {
			continue
		}
		eq := strings.Index(line, "=")
		if eq < 0 {
			continue
		}
		key := strings.TrimSpace(line[:eq])
		val := strings.TrimSpace(line[eq+1:])
		// Strip inline comments (; or #).
		if sc := strings.Index(val, ";"); sc >= 0 {
			val = strings.TrimSpace(val[:sc])
		}
		if hc := strings.Index(val, "#"); hc >= 0 {
			val = strings.TrimSpace(val[:hc])
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
		return nil, fmt.Errorf("error reading config: %w", err)
	}

	const defaultMaxFiles = 10
	// unlimitedMaxFiles is used when max_files = 0 (unlimited rotation) is set in
	// the server config. The viewer probes this many generations; files that do not
	// exist are simply skipped, so a large value is safe and effectively unlimited.
	const unlimitedMaxFiles = 1000

	configDir := filepath.Dir(configPath)
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
			path := s.jsonlFile
			if !filepath.IsAbs(path) {
				path = filepath.Join(configDir, path)
			}
			sinks = append(sinks, Sink{Name: s.name, Path: path, MaxFiles: mf})
		}
	}
	if len(sinks) == 0 {
		return nil, fmt.Errorf("no output sections with a jsonl_file found in %q", configPath)
	}
	return sinks, nil
}
