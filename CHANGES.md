# Changelog

## v1.0.0 — 2026-03-16

minilog is a small, production-worthy UDP syslog server. It receives RFC 3164 and RFC 5424 datagrams, routes them to one or more output sinks based on facility, and writes plain-text and/or structured JSONL log files with automatic rotation. Messages can also be forwarded to another syslog endpoint.

It runs as a native Windows service or as a plain Linux process, and is configured via a single INI file. See [README.md](README.md) for details.
