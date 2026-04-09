# Changelog

## v1.2.0 — 2026-04-09

### New

- **minilog-web-viewer**: a Go HTTP server with an embedded single-page app for browsing minilog
  JSONL logs in a browser. Supports multi-sink selection, severity/facility/text filtering,
  full-chain search across rotated files, infinite scroll, and live tail polling. Runs as a
  Windows service or standalone process. Bundled in the Windows installer as an optional component.

### Changed

- **Repository restructure**: server sources moved to `src/server/`, cli-viewer to
  `src/cli-viewer/`, tests to `tests/server/`, `tests/cli-viewer/`, `tests/binary/`.
- **JSONL severity/facility fields** are now string names (e.g. `"daemon"`, `"INFO"`) rather than
  integer codes, matching the server's actual output. The web-viewer filters on these strings.
- **`udp_port = 0`** is now accepted in the config file (OS-assigned ephemeral port).
- **Go coverage** is now uploaded to Codecov alongside the C++ coverage, with separate flags.

### Fixed

- **Receive strand serialised parsing**: parse+dispatch work is now posted to the `io_context`
  directly, enabling parallel parsing across the worker thread pool.
- **Rotation gap detection**: the server now probes all slots up to `max_files` instead of
  stopping at the first missing generation, preventing orphaned files after manual deletion.
- **Forwarder socket** is now created on its strand for consistency with the strand-per-sink
  pattern.
- **LogFile destructor** no longer calls `closeFiles()` directly, preventing a potential data race
  with queued strand work.
- **`parseSize` error messages** now include the output section name for easier debugging.
- **cli-viewer tests** replaced `select()`-based synchronisation with a thread-based accumulator,
  fixing failures on Windows.
- **Go INI parser** now strips `#` inline comments, matching the C++ parser behaviour.

## v1.1.0 — 2026-04-02

### New

- **minilog-cli-viewer**: a standalone Python 3.10+ script for real-time viewing of minilog JSONL
  output. Reads `minilog.conf` automatically to locate the log file. Features include configurable
  column display, include/exclude message filters, ANSI colour output by facility and severity,
  three timestamp formats, `--lines`/`--show-all` modes (similar to `tail -f`), and automatic
  re-open after log rotation. Configured via an optional `minilog-cli-viewer.conf` file.
  See `minilog-cli-viewer.conf.example` for all options.

### Fixed

- **Windows Event Viewer messages** (#2): the service now registers its own message table resource
  so that Event Viewer resolves log entries against the minilog executable. Previously every entry
  showed "The operation completed successfully" instead of the actual message text.
- **Default config contained unused `encoding` field** (#1): the `encoding` key in
  `[output.main]` was not a recognised option and has been removed from the installed default
  `minilog.conf`.

## v1.0.0 — 2026-03-16

This is the first release of minilog, a small, robust UDP syslog server. It receives RFC 3164 and RFC 5424 datagrams, routes them to one or more output sinks based on facility, and writes plain-text and/or structured JSONL log files with automatic rotation. Messages can also be forwarded to another syslog endpoint.

Releases contain an installable native Windows service, and can be built for Linux or Docker  for other use cases. It is configured via a single INI file. 

See [README.md](README.md) for details on features, configuration and development.
