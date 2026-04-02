# Changelog

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
