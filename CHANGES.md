# Changelog

## v1.0.0 — 2026-03-16

This is the first release of minilog, a small, robust UDP syslog server. It receives RFC 3164 and RFC 5424 datagrams, routes them to one or more output sinks based on facility, and writes plain-text and/or structured JSONL log files with automatic rotation. Messages can also be forwarded to another syslog endpoint.

Releases contain an installable native Windows service, and can be built for Linux or Docker  for other use cases. It is configured via a single INI file. 

See [README.md](README.md) for details on features, configuration and development.
