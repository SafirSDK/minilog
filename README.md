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
- **IPv6 is untested** — minilog binds a single socket to `[server] host`, so it serves one address
  family at a time; there is no dual-stack listener and nothing sets `v6_only(false)`, which on
  Windows an IPv6 socket defaults to anyway. An IPv6 `host` does bind an IPv6 socket and does
  receive, but that path is exercised only by a loopback smoke test and has never been validated
  against real IPv6 senders — treat it as unsupported. Forwarding is the same: the destination
  socket is opened from the resolved endpoint, so an IPv6 collector works as far as the same
  loopback test goes and no further
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

## Behaviour under flood

Nothing between the socket and the disk applies back pressure, and the sink flushes after every
line so that `tail` sees entries immediately. A sender faster than the disk would therefore grow
minilog's memory until the OS killed it. Instead, minilog holds at most `[server]
max_queue_bytes` (default 16 MB) of received-but-unwritten log and drops anything beyond that,
reporting the count to syslog or the Windows Event Log at most once every ten seconds. Resident
memory settles at a few times that figure, because a queued message is held once per `[output.*]`
section it routes to.

Dropping is the correct outcome rather than a compromise: UDP syslog has no delivery guarantee,
the sender never learns either way, and the kernel is already dropping silently once its own
socket buffer fills. The difference is that minilog now drops at a limit you chose, says how much
it dropped, and stays running. Raise `max_queue_bytes` to buffer more; there is deliberately no
setting that removes the limit.

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

**An unrecognised key is a config error, and so is a value that cannot be read.** minilog rejects
a key it does not know in `[server]`, `[output.*]`, `[forwarding]` or `[web_viewer]`, naming the
section, the key and the valid ones. `max_sise = 100MB` used to leave the size at its default and
`enabeld = true` used to leave forwarding off, with a running server doing something other than
what the file said. A misspelled *value* is the same mistake with the same consequence, so it
fails the same way: `max_files = abc`, `include_malformed = yess` and `max_message_size = 2k` are
startup errors naming the section, the key and the value. The booleans — `include_malformed` and
`[forwarding] enabled` — accept `true`, `false`, `1` and `0`, and nothing else; `yes`, `on` and
`True` are errors rather than guesses. Sections minilog does not know are ignored, so another
tool can keep its own settings in this file.

**Comments start a line; values run to the end of one.** `;` and `#` introduce a comment only as
the first character of a line. After a `=` they are ordinary characters, so
`jsonl_file = C:\logs\build#3.jsonl` names a file with a `#` in it, and
`max_files = 10 ; ten generations` is not the number 10 — it is a config error, in the server and
in the web viewer alike. All three programs that read this file — minilog, the cli-viewer and the
web-viewer — read it that way.

See [`minilog.conf.example`](minilog.conf.example) for a fully commented example.

### `[server]`

| Key | Default | Description |
|-----|---------|-------------|
| `host` | `0.0.0.0` | IP address to bind. An IPv6 literal binds an IPv6 socket — one family at a time, and untested against real senders; see [Limitations](#limitations) |
| `udp_port` | `514` | UDP port (0–65535; 0 = OS-assigned) |
| `workers` | `4` | Number of I/O worker threads (1–256) |
| `max_queue_bytes` | `16MB` | Received-but-unwritten log held in memory before further datagrams are dropped. Same units as `max_size`; a plain number is bytes. No "unlimited" setting — see [Behaviour under flood](#behaviour-under-flood) |

### `[output.<name>]`

Any number of named output sections. At least one of `text_file` or `jsonl_file` must be set,
and every configured file must belong to exactly one section — naming the same path twice,
whether as both keys of one section or across two sections, is a config error.

Both paths must be **absolute**. A relative path would resolve against the working directory of
whichever process read it, and minilog, the cli-viewer and the web-viewer each have a different
one — a Windows service inherits `C:\Windows\System32`. Environment variables are not expanded,
so `%ProgramData%\minilog\logs\syslog.log` is rejected rather than treated as a path. On Windows
a path needs a drive or a UNC share (`C:\logs\syslog.log`, `\\server\share\logs\syslog.log`);
`\logs\syslog.log` is relative to the current drive and is rejected.

| Key | Default | Description |
|-----|---------|-------------|
| `text_file` | — | Absolute path. Raw UDP payload bytes + `\n`, one line per message ([control characters escaped](#text-file)) |
| `jsonl_file` | — | Absolute path. One JSON object per line (see [JSONL format](#jsonl-format)) |
| `max_size` | `0` (no rotation) | Rotate when either file exceeds this. Units: `B`, `KB`, `MB`, `GB`. `0` disables rotation; a value that overflows 64 bits is a config error rather than silently becoming `0` |
| `max_files` | `10` | Rotated generations to keep, 0–1000. `0` = keep them all, up to that limit. Each generation costs a filesystem check on every rotation and, in the web viewer, on every request |
| `facility` | `*` | Comma-separated facility names to accept. `*` = all |
| `include_malformed` | `true` | Write unrecognised (UNKNOWN) datagrams. `true`, `false`, `1` or `0` |

Rotated filenames insert a generation number before the extension:
`syslog.log` → `syslog.1.log`, `syslog.2.log`, …

### `[web_viewer]`

Read by `minilog-web-viewer` only; minilog itself ignores the section. See
[web-viewer](#web-viewer) for what it changes.

| Key | Default | Description |
|-----|---------|-------------|
| `host` | — (every interface) | Address to bind the HTTP listener to. Empty means all interfaces on both IPv4 and IPv6 — `0.0.0.0` would be IPv4 only |
| `port` | `9514` | TCP port to listen on |

### `[forwarding]`

| Key | Default | Description |
|-----|---------|-------------|
| `enabled` | `false` | Enable UDP forwarding. `true`, `false`, `1` or `0` |
| `host` | — | Destination hostname or IP address (IPv4 or IPv6). Resolved once at startup; see below |
| `port` | `514` | Destination UDP port |
| `facility` | `*` | Facilities to forward |
| `max_message_size` | `2048` | Truncate messages longer than this (bytes); appends `... [TRUNCATED: N bytes]`. `0` = no limit; no unit suffix |

The destination is resolved once, when minilog starts, and the address found is used for the
lifetime of the process — re-resolving per message would put a name lookup on the hot path, and a
collector that moves is rare enough to be worth a restart. The lookup does not hold up startup:
it runs in the background while the UDP socket binds and the service reports itself running, so a
slow resolver delays forwarding and nothing else. Messages arriving in the moment before it
finishes are not forwarded, and are counted and reported when it does. A name that does **not**
resolve at startup is not a startup failure: minilog runs with forwarding off, reports it once,
and retries in the background with a growing delay until it succeeds, reporting how many messages
were dropped in the meantime. That is deliberate — a Windows `AUTO_START` service is routinely running
before DNS is, and losing the collector over an unreachable forwarding destination would be worse
than losing forwarding. A value that cannot be a host at all — brackets, a space, a scheme, or a
port appended (`syslog.example.com:514`, `10.0.0.5:514`) — is still a config error at startup,
because as a name it would never resolve and would be retried silently forever.

### Facility names

`kern`, `user`, `mail`, `daemon`, `auth`, `syslog`, `lpr`, `news`, `uucp`, `clock`, `authpriv`, `ftp`, `ntp`, `audit`, `alert`, `local0`–`local7`.

Aliases: `kernel`=`kern`, `security`=`auth`, `system`=`daemon`, `cron`=`clock`, `logaudit`=`audit`, `logalert`=`alert`.

## Output formats

### Text file

Raw UDP payload bytes written verbatim, followed by a single `\n`. No decoding or reformatting,
except that control characters are escaped so that one datagram is always exactly one line:

| Input | Written as |
|-------|------------|
| LF (`0x0A`) | `\n` |
| CR (`0x0D`) | `\r` |
| backslash (`0x5C`) | `\\` |
| any other C0 (`0x00`–`0x1F`) and DEL (`0x7F`) | `\xNN`, always two uppercase hex digits |
| C1 (`U+0080`–`U+009F`) | `\uNNNN`, always four uppercase hex digits |
| TAB (`0x09`) | itself — a tab cannot start a new line |

The two forms say what they encode: `\xNN` is one byte, `\uNNNN` is one codepoint. Everything
else above `0x7F` is written byte for byte, so UTF-8 text is never mangled — C1 is matched as a
decoded codepoint (the two-byte sequence `C2 80`–`C2 9F`), never as a raw byte, so the
continuation bytes of ordinary text are untouched. C1 is escaped because `U+009B` and `U+009D`
are the 8-bit forms of CSI and OSC: without it a sender can drive a terminal that pages or
`cat`s the file, with no ESC byte anywhere in the datagram.

Without this a sender could embed a newline in a datagram and write a second, entirely
fabricated entry — PRI included, so it would appear to come from a facility the datagram never
had — that nothing reading the file afterwards could distinguish from a genuine one. Doubling
the backslash keeps the transform reversible: `\n` in the file is always an escaped newline,
and a literal backslash-n in the message is always written `\\n`.

**This is not JSON escaping.** The JSONL sink writes ESC as `\\u001B`, TAB as `\t`, and escapes
the double quote; the text sink does none of those. The dialect above is the one the
[cli-viewer](#cli-viewer) displays, so a line on screen reads the way a line in the file does —
decode text-sink lines with these rules, not with a JSON string unescaper.

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
| `app` | string\|null | Application name. RFC 5424: the APP-NAME field. RFC 3164: the `tag:` at the start of the message — `null` when the message carries no tag |
| `pid` | string\|null | Process ID |
| `msgid` | string\|null | RFC 5424 MSGID field |
| `msg_time` | string\|null | Timestamp from the syslog message itself, verbatim and unnormalised. RFC 3164 example: `"Mar 12 14:30:22"` (no year, no timezone). RFC 5424 example: `"2026-03-12T14:30:22.000Z"`. `null` if the message carried no timestamp. |
| `message` | string | Message text. For RFC 5424, structured data is kept as a prefix of this field. |

For `UNKNOWN` messages, only `rcv`, `src`, and `message` are populated; all other fields are `null`.

**Field order is part of the format.** The fields appear in the order above, with `message` last,
and `facility` and `severity` are always names from minilog's own tables. The web viewer's
severity and facility filters rely on both: they locate `"severity":` by scanning the raw line
rather than parsing it, because the filter runs over every line of a rotation chain that can be
gigabytes, on every request. For files minilog wrote this is exact — the first `"severity":` in a
line is the real field, since `message` comes last, and a table-driven value cannot contain a
quote. Point the viewer at a JSONL file written by something else and neither holds: a `"severity":
"error"` inside a message body would be matched instead, quietly returning lines that do not match
the filter and hiding lines that do.

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

**The current directory is searched first on purpose.** A Windows shortcut's "Start in" field, or
a `cd` in a launcher script, then decides which configuration the viewer picks up — so several
config directories can be kept and switched between with different shortcuts. `--verbose` prints
which files were found when it is not obvious.

`--config` and `--viewer-config` name either file directly and skip the search entirely. A path
that does not exist is an error, not a fall-back to the search order: a typo in a deployment
script would otherwise read some other configuration's logs without saying so.

**Usage:**

```
python3 minilog-cli-viewer.py [options]
```

| Option | Default | Description |
|--------|---------|-------------|
| `--config PATH` | search order | Path to `minilog.conf`; error if it does not exist |
| `--viewer-config PATH` | search order | Path to `minilog-cli-viewer.conf`; error if it does not exist |
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

**Control characters on screen.** Every displayed field comes from the datagram, so every
displayed field is escaped before it is printed, using the same dialect as the
[text sink](#text-file): `\n`, `\r`, `\xNN` for the rest of C0 and DEL, and `\uNNNN` for C1
(`U+0080`–`U+009F`). Without this, ESC would reach the terminal and be obeyed — clearing the
screen, moving the cursor back over entries already printed, recolouring a benign line as
critical — and an embedded newline would let one record print as two. C1 is escaped for the
same reason: `U+009B` and `U+009D` are the 8-bit CSI and OSC, so a sender reaches those same
sequences without an ESC byte at all.

TAB prints as itself, and nothing above C1 is touched, so UTF-8 messages display normally.
Unlike the [text sink](#text-file), a literal backslash is **not** doubled: nothing decodes
what is on screen, and doubling it would obscure every Windows path in a message. The viewer's
own colour codes are unaffected — they are chosen from a table, never taken from the record.

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
| `--install` | — | Register as a Windows service (Windows only) |
| `--stop` | — | Stop the Windows service and wait for its process to exit (Windows only) |
| `--uninstall` | — | Remove the Windows service (Windows only) |
| `--timeout SECONDS` | `30` | How long `--stop` and `--uninstall` wait for the process to exit |

The server reads all `[output.*]` sections that have `jsonl_file` configured and exposes each as
a named **sink**. Open `http://localhost:9514` in a browser to access the UI.

**Listen address:** taken from `[web_viewer]` in `minilog.conf`, alongside every other deployment
fact. There is no command-line flag for it, so changing the port is an edit and a restart rather
than a re-registration of the Windows service.

```ini
[web_viewer]
; An empty host means every interface, on both IPv4 and IPv6.
; 127.0.0.1 restricts the viewer to the local machine.
host =
port = 9514
```

**Security note:** The web viewer does not implement any authentication or access control.
Anyone who can reach the listen address can read all exposed log data. Set
`[web_viewer] host = 127.0.0.1` to restrict access to the local machine, or place the viewer
behind a reverse proxy that provides authentication. Do not expose it on an untrusted network
without additional protection.

**Response headers.** Every response carries a `Content-Security-Policy` of `default-src 'none'`
with `script-src`, `style-src` and `connect-src` at `'self'`, `img-src 'self' data:` (the search
icon is an inline SVG data URI), and `frame-ancestors 'none'`; plus
`X-Content-Type-Options: nosniff`, `Referrer-Policy: no-referrer` and `Cache-Control: no-store`.
The UI's job is rendering text a syslog sender chose, so the escaping in `app.js` is the control
and the CSP is the backstop for what it misses. The strict policy is possible because every asset
is local and none of them uses an inline script, style or event handler — which a test checks, so
that a later edit cannot quietly make the policy wrong. `no-store` also keeps an upgraded viewer
from serving the previous version's `app.js` out of a browser cache, since the asset URLs carry no
version.

**Connection timeouts:** the HTTP server closes a connection whose request headers are not
complete within 10 seconds, whose request is not complete within 30 seconds, or that sits idle
between keep-alive requests for 120 seconds. None of these are configurable. There is
deliberately no write timeout: a full-chain `/search` can legitimately take a long time to
produce, and cutting it off would hand the client a truncated response indistinguishable from a
complete one.

**Windows service:** `--install` registers the binary as an auto-start service named
`minilog-web-viewer`. Pass `--config` at install time; that path is baked into the service entry,
and the listen address then follows the config file it points at. The installer's Start Menu and
desktop shortcuts are built from `[web_viewer] port` in the installed config, so they are correct
on an upgrade as well as a fresh install — but nothing can keep them in step with an edit made
afterwards. `--uninstall` stops and removes it, and `--stop` stops it without removing it —
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

`count` and `limit` default to 200 and are clamped to **5000**; a larger value is silently
reduced rather than rejected. The clamp bounds how many lines one request returns. It is not
configurable, and the browser UI never asks for more than 200, so it is not a limit any normal
client meets. On `/search` it bounds the results returned, not `total_matches`, which still
counts every match in the chain.

A second ceiling bounds the **bytes** on `/lines`: a request stops collecting once the lines
gathered reach **8 MB**, and returns what it has. The line clamp alone is not a memory bound,
because a syslog sender chooses how long a line is — a 65507-byte datagram of control bytes
becomes roughly 400 KB of JSONL, so 5000 of those would be about 2 GB built in memory for one
GET. This is not a limit the browser UI meets either; at 200 records a page it is reached only
by a sink of records averaging over 40 KB.

That 8 MB is the log text collected off disk, not the size of the response. The two differ by
more than framing: the JSON encoder rewrites `<`, `>` and `&` as six-byte escapes, which the
records themselves do not use, so a sink of records dense in those — an application logging XML,
say — yields a body closer to **48 MB**, and a comparable buffer while it is built. That is the
documented worst case for one request.

Long lines are returned whole: a page ends between records, never inside one, except that a
chain whose non-final file does not end in a newline can still start a page one byte late (see
[issue #41](https://github.com/SafirSDK/minilog/issues/41)). Neither ceiling is reported in the
response. A page cut short by either advances `next_offset` to just past the last line returned
when paging forward, and sets `first_offset` to the start of the oldest line returned when paging
back, so a client continues from there and eventually sees everything.

`/search` carries an offset per match and no record text — a client jumps to a match by asking
`/lines` for the window around its offset — so its response is small whatever the records behind
it are, and the byte ceiling does not apply to it.

Filter parameters `sev` and `fac` accept comma-separated name strings (e.g. `sev=info,warning`, `fac=auth,daemon`).

## License

MIT — see the [LICENSE](LICENSE) file for details.
