# minilog

> **Looking for the Python version?** See the [python-version](https://github.com/SafirSDK/minilog/tree/python-version) tag.

![minilog logo](artwork/minilog-logo.png)

[![Build & Test](https://github.com/SafirSDK/minilog/actions/workflows/build.yml/badge.svg?branch=master)](https://github.com/SafirSDK/minilog/actions/workflows/build.yml)
[![codecov](https://codecov.io/gh/SafirSDK/minilog/branch/master/graph/badge.svg)](https://codecov.io/gh/SafirSDK/minilog)

A small UDP syslog server that understands RFC 3164 and RFC 5424. Receives datagrams, routes them to text and/or JSONL log files with rotation, and can forward to another syslog endpoint. Ships with a browser-based log viewer and a CLI tail tool for reading logs. Runs as a Windows service or a Linux process (systemd-friendly); the Windows installer sets up both the server and the web viewer by default.

## Features

- UDP reception
- Parses RFC 3164, RFC 5424, and unrecognised ("UNKNOWN") datagrams
- Multiple named output sections, each independently filtered by facility
- Per-output text file (raw payload) and/or JSONL file (structured)
- Log rotation by file size with configurable retention count
- UDP forwarding with per-facility filtering and message truncation
- Windows service installation/removal via CLI flags
- Web viewer — browser UI with paging, filtering, search across rotated log files (installed by default on Windows)
- CLI viewer — Python `tail -f` style tool with colour output and filtering
- Single external dependency: Boost (server only; viewers are standalone)

## Limitations

- **UDP only** — no TCP, TLS, or RELP; message delivery is best-effort
- **IPv4 only** — the server binds a UDP v4 socket; IPv6 is not supported
- **No multicast or promiscuous capture** — minilog receives datagrams addressed to the host it runs on; it does not join multicast groups or sniff traffic addressed elsewhere (neither is part of the syslog RFCs)
- **RFC 5424 structured data is not parsed** — it is just passed along to the output files
- **Rotated files are not compressed** — generation files are plain text/JSONL; no gzip
- **Web viewer has no authentication or TLS** — anyone who can reach the listen port can read all exposed logs; bind to localhost or place behind a reverse proxy on untrusted networks

## Standards conformance

minilog implements [RFC 5426](https://www.rfc-editor.org/rfc/rfc5426) (Transmission of Syslog
Messages over UDP) in the **receiver** role:

| Requirement | minilog |
|-------------|---------|
| §3.1 — one message per datagram, complete or truncated | One parse per datagram; payloads matching neither RFC format are retained verbatim as `proto=UNKNOWN` rather than discarded |
| §3.2 — MUST accept 480 octets (IPv4); SHOULD accept 2048 | Accepts up to 65507 octets — the full UDP payload limit |
| §3.3 — MUST accept on port 514, MAY be configurable | Default 514, configurable via `[server] udp_port` |
| §3.4 — source IP SHOULD NOT identify the originator | `src` (sender IP) is recorded separately from `hostname` (the in-message identifier) |
| §3.6 — MUST NOT disable UDP checksum checks | Kernel default, untouched |

Message formats: [RFC 5424](https://www.rfc-editor.org/rfc/rfc5424) and legacy
[RFC 3164](https://www.rfc-editor.org/rfc/rfc3164). RFC 5424 structured data is preserved verbatim
as a prefix of the message rather than parsed into separate fields.

When forwarding (`[forwarding]`), minilog acts as a syslog sender. Messages longer than
`max_message_size` are truncated and marked with `... [TRUNCATED: N bytes]`; RFC 5426 §3.1 permits
truncated messages, and §3.2 RECOMMENDS that senders keep datagrams below the path MTU.

Multicast and broadcast group reception are not addressed by any of the syslog RFCs and are not
supported. Transports other than UDP (TCP, TLS/RFC 5425, RELP) are out of scope.

## Requirements

| Platform | Toolchain | Boost |
|----------|-----------|-------|
| Linux | GCC 12+ or Clang 16+, CMake 3.25+, Ninja | system package (`libboost-all-dev`) |
| Windows | MSVC 2022+, CMake 3.25+, Ninja, Conan 2 | managed by Conan |

## Building

### Linux

```
sudo apt-get install ninja-build libboost-all-dev
cmake --preset linux-release
cmake --build --preset linux-release
```

The binary is at `build/linux-release/minilog`.

To run the tests:

```
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug --output-on-failure
```

Other available presets: `linux-coverage`, `linux-asan`, `linux-tsan`, `linux-fuzz`.

### Windows

Install [Conan 2](https://conan.io/) and MSVC 2022, then from a Visual Studio developer prompt:

```
conan install . --output-folder=build/windows-release --build=missing -s build_type=Release
cmake --preset windows-release
cmake --build --preset windows-release
```

The binary is at `build\windows-release\Release\minilog.exe`.

To build the installer (requires [Inno Setup](https://jrsoftware.org/isinfo.php)):

```
cmake --build --preset windows-release --target package
```

### Running the installer from another installer or a script

Pass `/VERYSILENT` to suppress the wizard and install with defaults:

```
minilog-1.0.0-setup.exe /VERYSILENT
```

For further command-line flags (component selection, install directory override, etc.) see the
[Inno Setup documentation](https://jrsoftware.org/ishelp/index.php?topic=setupcmdline).

## Development

### Test suite overview

All tests are driven by CTest. Run them with:

```
ctest --preset linux-debug --output-on-failure
```

The suite contains:

| Target | What it covers |
|--------|---------------|
| `test_config` | INI config parsing, defaults, validation |
| `test_wait_until` | The stop-and-wait timeout logic behind `--stop`/`--uninstall` |
| `test_parser` | RFC 3164, RFC 5424, and UNKNOWN datagram parsing |
| `test_output` | File writing, rotation, facility filtering |
| `test_forwarder` | UDP forwarding, truncation, facility filtering |
| `test_integration` | Multi-output routing end-to-end |
| `test_stress` | Concurrent senders, file rotation under load (soak) |
| `test_binary` | Black-box test of the real binary (Python, via CTest) |

### Sanitizer builds

```
cmake --preset linux-asan && cmake --build --preset linux-asan
ctest --preset linux-asan --output-on-failure
```

Replace `asan` with `tsan` for the ThreadSanitizer build.

### Coverage

```
cmake --preset linux-coverage && cmake --build --preset linux-coverage
ctest --preset linux-coverage
gcovr -r . --html-details build/linux-coverage/coverage.html
```

### Fuzz testing

```
cmake --preset linux-fuzz && cmake --build --preset linux-fuzz
build/linux-fuzz/bin/fuzz_parser -max_total_time=60
```

### Extended soak (ASan + UBSan / TSan, ~30 min each)

The `linux-asan-extended` and `linux-tsan-extended` presets run `test_stress` for 900 seconds per case. Used in CI nightly; run locally when making changes to threading or I/O paths:

```
cmake --preset linux-asan-extended && cmake --build --preset linux-asan-extended
ctest --preset linux-asan-extended --output-on-failure
```

## Usage

```
minilog <config-path>
minilog --help
```

On Windows only:

```
minilog --install <config-path>    # register as a Windows service (does not start it)
minilog --stop                     # stop the service, wait for the process to exit
minilog --uninstall                # stop and remove the Windows service
```

The config path is stored in the service registry entry so the same path is used when the service
starts automatically on boot. It is made absolute first: the SCM starts services with the working
directory set to `C:\Windows\System32`, so a relative path given to `--install` would resolve
somewhere else entirely at boot. The registration records the executable's own location as the
OS reports it, so it is correct whether minilog was invoked by full path or found through `PATH`.
A config file that cannot be read is refused at install time, rather than registering a service
that fails at every boot.

`--install` against a service that is already registered updates it rather than failing: the
executable path and the config path are rewritten, and the Event Log source is repointed at the
current executable, but the start type and the service account are left as they are — an
administrator who set the service to manual start, or bound it to a specific account with
`sc config`, keeps that across an upgrade. Nothing is started or stopped; restart the service to
run the new registration. `--uninstall` against a service that is not registered succeeds, since
that is the state it is asking for.

`--stop` and `--uninstall` wait until the service *process* has exited, not merely until the SCM
reports `SERVICE_STOPPED` — a service reports itself stopped before it has returned from `main`,
and a running executable cannot be overwritten or deleted. `--timeout SECONDS` (default 30) bounds
the wait; running out is an error rather than a silent proceed. Stopping a service that is already
stopped, or not registered at all, succeeds.

`--stop` is the primitive to use before copying new binaries over an installation. `sc stop` and
PowerShell's `WaitForStatus('Stopped')` wait on SCM state only, which is not the same thing:

```
minilog --stop                     # both services, before anything is copied
minilog-web-viewer --stop
                                   # copy the new binaries over the old ones
minilog --install <config-path>
minilog-web-viewer --install --config <config-path>
sc start minilog
sc start minilog-web-viewer
```

`--install` also configures the service's recovery actions: the SCM restarts it 5 seconds after
a failure, twice, and then leaves it stopped; the failure counter resets after 300 seconds with
no failures. A service that fails at startup — bad config, unbindable port — therefore stops
after two attempts rather than looping, while one that crashes after running healthily for
longer than the reset period is always retried.

The service reports `SERVICE_RUNNING` only after the config has loaded, the sinks have opened and
the UDP socket has bound. A run that fails is reported to the SCM as a service-specific error with
a non-zero exit code, rather than as a clean stop.

Note that `sc start minilog` returns as soon as the service reports `SERVICE_START_PENDING`, so
its exit code does not say whether startup then succeeded. Use `net start minilog`, which waits
for the outcome, or check `sc query minilog` afterwards — a failed start shows
`WIN32_EXIT_CODE : 1066` with a non-zero `SERVICE_EXIT_CODE`. The reason is in the Event Log.

## Configuration

minilog reads a single INI file passed on the command line. There is no config reload; restart the process to pick up changes.

Errors (bad config, bind failure, write failure) are reported to the Windows Event Log on Windows, and to the system syslog on Linux — plus stderr in both cases.

**Sink failures are isolated and final.** If a sink cannot write, rotate or reopen its files — a
denied directory, a full disk, a network path that has gone away — that sink is taken out of
service and every later message routed to it is dropped. The failure is reported once, the other
sinks and the forwarder keep running, and the process stays up. A closed sink is not reopened
automatically, so restart minilog once the underlying storage problem is fixed.

See [`minilog.conf.example`](minilog.conf.example) for a fully commented example.

### `[server]`

| Key | Default | Description |
|-----|---------|-------------|
| `host` | `0.0.0.0` | IP address to bind |
| `udp_port` | `514` | UDP port (0–65535; 0 = OS-assigned) |
| `workers` | `4` | Number of I/O worker threads |

### `[output.<name>]`

Any number of named output sections. At least one of `text_file` or `jsonl_file` must be set,
and every configured file must belong to exactly one section — naming the same path twice,
whether as both keys of one section or across two sections, is a config error.

| Key | Default | Description |
|-----|---------|-------------|
| `text_file` | — | Raw UDP payload bytes + `\n`, one line per message |
| `jsonl_file` | — | One JSON object per line (see [JSONL format](#jsonl-format)) |
| `max_size` | `0` (unlimited) | Rotate when either file exceeds this. Units: `B`, `KB`, `MB`, `GB` |
| `max_files` | `10` | Rotated files to keep. `0` = unlimited |
| `facility` | `*` | Comma-separated facility names to accept. `*` = all |
| `include_malformed` | `true` | Write unrecognised (UNKNOWN) datagrams |

Rotated filenames insert a generation number before the extension:
`syslog.log` → `syslog.1.log`, `syslog.2.log`, …

### `[forwarding]`

| Key | Default | Description |
|-----|---------|-------------|
| `enabled` | `false` | Enable UDP forwarding |
| `host` | — | Destination IP address (names are not resolved) |
| `port` | `514` | Destination UDP port |
| `facility` | `*` | Facilities to forward |
| `max_message_size` | `2048` | Truncate messages longer than this (bytes); appends `... [TRUNCATED: N bytes]` |

### Facility names

`kern`, `user`, `mail`, `daemon`, `auth`, `syslog`, `lpr`, `news`, `uucp`, `clock`, `authpriv`, `ftp`, `ntp`, `audit`, `alert`, `local0`–`local7`.

Aliases: `kernel`=`kern`, `security`=`auth`, `system`=`daemon`, `cron`=`clock`, `logaudit`=`audit`, `logalert`=`alert`.

## Output formats

### Text file

Raw UDP payload bytes written verbatim, followed by a single `\n`. No decoding or reformatting.

### JSONL format

One UTF-8 JSON object per line:

```json
{"rcv":"2026-03-12T14:30:22Z","src":"192.168.1.50","proto":"RFC3164","facility":"daemon","severity":"NOTICE","hostname":"mymachine","app":"su","pid":"123","msgid":null,"msg_time":"Mar 12 14:30:22","message":"text here"}
```

| Field | Type | Description |
|-------|------|-------------|
| `rcv` | string | ISO 8601 UTC receive time (when minilog received the datagram) |
| `src` | string | Source IP address |
| `proto` | string | `"RFC3164"`, `"RFC5424"`, or `"UNKNOWN"` |
| `facility` | string\|null | Facility name (e.g. `"daemon"`, `"auth"`, `"local0"`) |
| `severity` | string\|null | Severity name (e.g. `"INFO"`, `"ERROR"`, `"DEBUG"`) |
| `hostname` | string\|null | Syslog hostname field |
| `app` | string\|null | Application name |
| `pid` | string\|null | Process ID |
| `msgid` | string\|null | RFC 5424 MSGID field |
| `msg_time` | string\|null | Timestamp from the syslog message itself, verbatim and unnormalised. RFC 3164 example: `"Mar 12 14:30:22"` (no year, no timezone). RFC 5424 example: `"2026-03-12T14:30:22.000Z"`. `null` if the message carried no timestamp. |
| `message` | string | Message text. For RFC 5424, structured data is kept as a prefix of this field. |

For `UNKNOWN` messages, only `rcv`, `src`, and `message` are populated; all other fields are `null`.

## Docker

Build the image:

```
docker build -t minilog .
```

Run with docker-compose (mounts config from `./conf/minilog.conf`, writes logs to `./logs/`):

```
docker compose up
```

Log file paths in the config must match the container's volume mount. With the default `docker-compose.yml` the log directory is `/var/log/minilog/`, so use paths like `/var/log/minilog/syslog.log`.

The Docker image contains only the syslog server. The web-viewer and cli-viewer are not included — run them on the host against the mounted log volume if needed.

## Linux deployment (systemd)

No PID file is needed; systemd tracks the process directly. Example unit file:

```ini
# /etc/systemd/system/minilog.service
[Unit]
Description=minilog syslog server
After=network.target

[Service]
ExecStart=/usr/local/bin/minilog /etc/minilog/minilog.conf
Restart=on-failure
User=minilog

[Install]
WantedBy=multi-user.target
```

Enable and start:

```
systemctl daemon-reload
systemctl enable --now minilog
```

Note: binding to UDP port 514 requires either `CAP_NET_BIND_SERVICE` or running as root. To avoid running as root, bind to a high port (e.g. 5514) and redirect with a firewall rule.

## Sending a test message

Using netcat:

```
echo "<14>Mar 15 12:00:00 myhost app[99]: hello" | nc -u -w1 127.0.0.1 514
```

Using Python:

```python
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.sendto(b"<14>Mar 15 12:00:00 myhost app[99]: hello", ("127.0.0.1", 514))
```

## Viewers

Two optional viewer tools ship alongside the server. Both read minilog's JSONL output files
directly — no special server-side support required.

### cli-viewer

`src/cli-viewer/minilog-cli-viewer.py` — Python 3 script. Works on Linux and Windows. Behaves
like `tail -f`: shows the last N lines on startup and then follows new entries in real time,
surviving log rotation transparently.

**Config discovery** (in order):

1. `./minilog.conf` (current directory)
2. `/etc/minilog/minilog.conf` (Linux) or `%ProgramData%\minilog\minilog.conf` (Windows)
3. Same directory as the script

The viewer also looks for `minilog-cli-viewer.conf` next to `minilog.conf` (or `./`) for display
and filter settings. See [`src/cli-viewer/minilog-cli-viewer.conf.example`](src/cli-viewer/minilog-cli-viewer.conf.example).

**Usage:**

```
python3 minilog-cli-viewer.py [options]
```

| Option | Default | Description |
|--------|---------|-------------|
| `--output-section NAME` | `main` | Read `[output.NAME]` from `minilog.conf` |
| `--lines N` / `-n N` | `10` | Lines to show on startup; `0` = follow-only |
| `--show-all` | — | Print all existing entries and exit (no follow) |
| `--include PATTERN` | — | Show only messages containing PATTERN (repeatable, additive with config file) |
| `--exclude PATTERN` | — | Hide messages containing PATTERN (repeatable, additive with config file; exclude wins) |
| `--no-color` | — | Disable ANSI colour output |
| `--verbose` / `-v` | — | Print config discovery and filter info to stderr |

**`minilog-cli-viewer.conf` settings:**

| Section | Key | Default | Description |
|---------|-----|---------|-------------|
| `[viewer]` | `columns` | `rcv, facility, severity, hostname, app, pid, message` | Columns to display (comma-separated). Available: `rcv src proto facility severity hostname app pid msgid message` |
| `[viewer]` | `timestamp_format` | `short` | `iso` (full ISO8601), `short` (`MM-DD HH:MM:SS`), or `time` (`HH:MM:SS`) |
| `[viewer]` | `use_colors` | `true` | ANSI colour coding; auto-disabled when stdout is not a TTY |
| `[filters]` | `exclude` | — | One pattern per line; combined with `--exclude` CLI flags |
| `[filters]` | `include` | — | One pattern per line; combined with `--include` CLI flags |

### web-viewer

`src/web-viewer/` — Go 1.25 HTTP server with an embedded single-page app. Reads JSONL files
(including the full rotation chain) and exposes them via a REST API; the browser UI handles
paging, filtering, and search without a database.

**Build:**

```
go build -o minilog-web-viewer ./src/web-viewer
```

On Windows, cross-compile with `GOOS=windows GOARCH=amd64` or build natively with Go for Windows.

**Usage:**

```
minilog-web-viewer [options]
```

| Option | Default | Description |
|--------|---------|-------------|
| `--config PATH` | `<exe dir>/minilog.conf` | Path to `minilog.conf` |
| `--addr ADDR` | `:9514` | HTTP listen address |
| `--install` | — | Register as a Windows service (Windows only) |
| `--stop` | — | Stop the Windows service and wait for its process to exit (Windows only) |
| `--uninstall` | — | Remove the Windows service (Windows only) |
| `--timeout SECONDS` | `30` | How long `--stop` and `--uninstall` wait for the process to exit |

The server reads all `[output.*]` sections that have `jsonl_file` configured and exposes each as
a named **sink**. Open `http://localhost:9514` in a browser to access the UI.

**Security note:** The web viewer does not implement any authentication or access control.
Anyone who can reach the listen address can read all exposed log data. Bind to `127.0.0.1:9514`
(via `--addr 127.0.0.1:9514`) to restrict access to the local machine, or place the viewer
behind a reverse proxy that provides authentication. Do not expose it on an untrusted network
without additional protection.

**Windows service:** `--install` registers the binary as an auto-start service named
`minilog-web-viewer`. Pass `--config` and `--addr` at install time; those values are baked into
the service entry. `--uninstall` stops and removes it, and `--stop` stops it without removing it —
both wait for the process to exit, as described for the server above. `--install` over an existing
registration updates it, preserving the start type and account, and `--uninstall` succeeds when
there is nothing registered — the same as the server.

`--install` also registers a Windows Event Log source named `minilog-web-viewer` (removed again
by `--uninstall`) and configures the same recovery actions as the server: two restarts 5 seconds
apart, then stopped, with the failure counter resetting after 300 seconds. A service process has
no console, so the Event Log is where startup failures — an unreadable `minilog.conf`, a listen
address that cannot be bound — are recorded. The viewer reports `SERVICE_RUNNING` only once the
listen address is bound, and reports a failed run to the SCM as a service-specific error; the same
`net start` / `sc query` caveat as the server applies. Interactive runs continue to log to stderr.

**Browser UI features:**

- Sink selector (one tab per `[output.*]` section)
- Infinite scroll — loads older pages as you scroll up; live-tail polling for new entries
- Column visibility toggle (Msg Time, Rcv Time, Source, Hostname, App, PID, Severity, Proto, Msg ID, Message)
- Severity and facility filter dropdowns
- Include / exclude text pattern filters
- Full-chain search (searches across all rotated generations, returns total match count)

**REST API:**

| Endpoint | Description |
|----------|-------------|
| `GET /sinks` | JSON array of `{name}` objects, one per configured output sink |
| `GET /lines?sink=NAME&[tail=true\|offset=N&dir=forward\|backward]&count=N&sev=…&fac=…&inc=…&exc=…` | Page of log lines with offsets |
| `GET /search?sink=NAME&q=TEXT&limit=N&sev=…&fac=…&inc=…&exc=…` | Full-chain search |

Filter parameters `sev` and `fac` accept comma-separated name strings (e.g. `sev=info,warning`, `fac=auth,daemon`).

## License

MIT — see the [LICENSE](LICENSE) file for details.
