# minilog

A small, dependency-free, UDP-only syslog listener that understands RFC3164 and RFC5424 message formats (transport RFC5426). It supports regex-based exclusion filters and can write to stdout and/or a file. Intended for development and testing; not production-hardened.

## Features

- UDP server (no TCP/TLS).
- Parses RFC3164 and RFC5424 (ignores structured data but preserves it in the message text).
- Regex-based exclusion filters (case-insensitive by default; toggleable).
- Output to stdout and/or a log file with simple, single-line formatted records.
- Worker thread pool for processing incoming packets.
- Optional INI configuration file living next to the script.

## Requirements

- Python 3.8+ (standard library only).

## Installation

No install required; clone and run directly. For running tests you may want pytest:
```
pip install pytest
```

## Quick start

- Listen on the default port (514) on all interfaces:
  ```
  python syslog-server.py
  ```
- Specify host/port and add exclusion patterns:
  ```
  python syslog-server.py --host 0.0.0.0 --port 5514 -e "healthcheck" -e "^DEBUG"
  ```
- Case-sensitive exclusion matching and write to a file only:
  ```
  python syslog-server.py --case-sensitive --no-stdout -o logs.txt
  ```
- Show effective options before starting:
  ```
  python syslog-server.py --verbose
  ```

On some systems, binding to port 514 may require elevated privileges.

## Send a test message

- Using netcat (UDP):
  ```
  echo "<14>Oct 11 22:14:15 myhost app[99]: hello" | nc -u -w1 127.0.0.1 5514
  ```
- Using Python:
  ```
  python - <<'PY'
  import socket
  s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
  s.sendto(b"<14>Oct 11 22:14:15 myhost app[99]: hello", ("127.0.0.1", 5514))
  PY
  ```

## Command-line options

- `--host` Host/IP to bind (default: value from config or 0.0.0.0).
- `--port` UDP port (default: value from config or 514).
- `-e, --exclude` Regex pattern to exclude (can be repeated). Added to patterns from config.
- `--case-sensitive` Enable case-sensitive matching for excludes (default: case-insensitive).
- `-o, --output` Write formatted output to this file (in addition to stdout unless `--no-stdout`).
- `--no-stdout` Do not write to stdout.
- `--encoding` Decode incoming bytes with this encoding (default: utf-8).
- `--workers` Number of worker threads for processing packets (default: 4 or value from config).
- `--verbose` Print effective options before starting.

## Configuration file

Place a `syslog-server.conf` file next to `syslog-server.py`. All options are under the `[server]` section. CLI options override config values.

Example:

```
[server]
host = 127.0.0.1
port = 5514
encoding = latin-1
output = test.log
no_stdout = true
case_sensitive = true
workers = 8
exclude = foo, bar
exclude = ^ignore-this$
```

Notes:
- `exclude` supports multiple lines and/or comma-separated lists. Duplicates are removed while preserving order.
- Excludes from the config are additive with `-e/--exclude` on the CLI.

## Output format

Each line is emitted as a single line (newlines are replaced with spaces). A typical line contains:

```
<utc-received-iso> <protocol> <facility.severity or pri=N> <hostname?> <app[procid]?> <msgid?> ts=<timestamp?> src=<ip:port> -- <message?>
```

Examples:
- RFC3164: `2024-10-07T12:34:56Z RFC3164 user.INFO myhost app[99] ts=Oct 11 22:14:15 src=127.0.0.1:56324 -- hello`
- RFC5424: `2024-10-07T12:34:56Z RFC5424 auth.CRITICAL mymachine su ID47 ts=2003-10-11T22:14:15.003Z src=127.0.0.1:56324 -- [exampleSDID@32473 iut="3"] 'su root' failed ...`

Notes:
- For RFC5424, structured data is not parsed but preserved in the message text.
- When facility/severity are not available, `pri=N` is shown instead.
- `src=` is always included.
- Maximum UDP packet size accepted is 65507 bytes.

## Limitations

- UDP only (no TCP/TLS).
- Not hardened for production use.
- RFC5424 structured data is not parsed; it is kept as part of the message text.
- On some platforms, binding to privileged ports (e.g., 514) requires elevated privileges.

## Development

- Lint/format as you prefer; the project uses only the standard library.
- Worker threads are managed via `concurrent.futures.ThreadPoolExecutor`.

## Testing

Run the test suite from the repo root:

```
pytest -q
```

## License

MIT — see the LICENSE file for details.
