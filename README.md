# minilog

> **Looking for the Python version?** See the [python-version](https://github.com/SafirSDK/minilog/tree/python-version) tag.

![minilog logo](artwork/minilog-logo.png)

[![Build & Test](https://github.com/SafirSDK/minilog/actions/workflows/build.yml/badge.svg?branch=master)](https://github.com/SafirSDK/minilog/actions/workflows/build.yml)
[![codecov](https://codecov.io/gh/SafirSDK/minilog/branch/develop/graph/badge.svg)](https://codecov.io/gh/SafirSDK/minilog)

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
- CLI sender — `minilog-send`, a small executable for putting a line into the log from a script or a shell
- Single external dependency: Boost (server and sender only; viewers are standalone)

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
`max_message_size` are truncated to that size, the end replaced by `[TRUNCATED: N bytes]` with N the
original length; RFC 5426 §3.1 permits truncated messages, and §3.2 RECOMMENDS that senders keep
datagrams below the path MTU.

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

The binaries are in `build/linux-release/bin/`.

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

The binaries are in `build\windows-release\bin\`.

To build the installer (requires [Inno Setup](https://jrsoftware.org/isinfo.php)):

```
cmake --build --preset windows-release --target package
```

To build the zip archive for [installing without the installer](#windows-deployment-without-the-installer)
(needs nothing beyond the build itself):

```
cmake --build --preset windows-release --target package-zip
```

Both land in `build\windows-release`, and a tagged CI build attaches both to the GitHub release.
A prerelease tag such as `v1.4.0-beta2` puts its suffix into the file names
(`minilog-1.4.0-beta2-setup.exe`) and into the web viewer's `/version`; locally, configure with
`-DMINILOG_VERSION_SUFFIX=-beta2` for the same effect.

### Running the installer from another installer or a script

Pass `/VERYSILENT` to suppress the wizard and install with defaults:

```
minilog-1.4.0-setup.exe /VERYSILENT
```

For further command-line flags (component selection, install directory override, etc.) see the
[Inno Setup documentation](https://jrsoftware.org/ishelp/index.php?topic=setupcmdline).

A deployment that wants to place the files itself, or that cannot run a third-party installer,
uses the zip archive instead — see [Windows deployment without the
installer](#windows-deployment-without-the-installer).

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
| `test_preflight` | What `--check` reports about a config and the machine under it |
| `test_parser` | RFC 3164, RFC 5424, and UNKNOWN datagram parsing |
| `test_output` | File writing, rotation, facility filtering |
| `test_forwarder` | UDP forwarding, truncation, facility filtering |
| `test_integration` | Multi-output routing end-to-end |
| `test_stress` | Concurrent senders, file rotation under load (soak) |
| `test_send` | `minilog-send`: argument parsing, both wire formats, timestamps, and a round trip through the parser |
| `test_binary` | Black-box test of the real binary (Python, via CTest) |
| `test_send_binary` | The built `minilog-send` against a running `minilog`: JSONL fields, stdin mode, exit codes (Python, via CTest) |

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
minilog --check <config-path>      # validate the config and this machine, then exit
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

### Validating a deployment — `--check`

`minilog --check <config-path>` loads the config, checks the machine against it, prints everything
it found and exits. It starts no server, writes to no log file and creates nothing that outlives
the run: the probe it writes into each log directory is deleted again, and an existing log file is
opened for appending without a byte being added.

```
minilog --check /etc/minilog/minilog.conf
```

It reports:

- **config errors** — the same validation the server does at startup. A config that will not load
  ends the run, since there is nothing left to check the machine against.
- **a log directory that does not exist**, distinguished from **one that exists but is not
  writable**. minilog never creates directories, so these are the two halves of provisioning and
  they have nothing in common: one needs the directory created, the other needs its ACL changed.
- **an existing log file that cannot be appended to** — a directory with the right permissions is
  not enough once someone has tightened the permissions on yesterday's log.
- **the UDP listen port**, by binding it with the same socket options the server uses and letting
  go again.
- **an unresolvable forwarding destination**, when `[forwarding] enabled = true`. The server
  deliberately does not fail to start over this; it retries the lookup in the background, so the
  only symptom at runtime is that nothing is forwarded.
- **`max_size = 0`**, as a warning: rotation is off, which is a valid choice and worth stating.

Every check runs even after one has failed, so one run lists every problem — a preflight that
stopped at the first fault would force the fix-rerun-fix-rerun cycle it exists to prevent. The exit
code is non-zero if any finding is an error; warnings alone exit zero.

Output goes to stdout only, never to the Event Log or syslog: a validation run must not leave
entries behind, least of all before `--install` has registered the event source that renders them.

On Windows with the service already running, the bind test cannot succeed —
`SO_EXCLUSIVEADDRUSE` means the running minilog holds the port exclusively. `--check` asks the SCM
and reports that as a warning naming the service, rather than as a bind failure; that is the state
of a healthy machine, which is where `--check` is most often run. The SCM query is read-only and
needs no elevation.

What it cannot tell you is whether **remote senders can reach the port**. That takes a datagram
from a real sender; nothing running locally can prove a firewall rule exists. The report ends with
a list of what the configuration requires from the machine — listen endpoints and directories
needing write access — to hand to whoever provisions those rules and ACLs.

## Configuration

minilog reads a single INI file passed on the command line. There is no config reload; restart the process to pick up changes.

Errors (bad config, bind failure, write failure) are reported to the Windows Event Log on Windows, and to the system syslog on Linux — plus stderr in both cases.

**Sink failures are isolated, and a closed sink retries.** If a sink cannot write, rotate or
reopen its files — a denied directory, a full disk, a network path that has gone away — that sink
is taken out of service: the failure is reported, every message routed to it is dropped, and the
other sinks and the forwarder keep running with the process up.

Every 30 seconds it tries to open its files again, and reports the outage with its duration when
they open. The interval is not configurable. A sink is selected by facility, so the retry is on a
timer rather than on the next message to reach that sink — the sink whose silence is least likely
to be noticed is exactly the quiet one no message would wake. **Messages that arrive while a sink
is closed are gone**; what the retry restores is the sink, not the gap.

A sink still closed is re-reported once a minute, naming how many attempts to reopen it have
failed, so an outage that lasts is visible for as long as it lasts and not only at the moment it
began. A fault that never clears therefore costs one log line a minute and one open attempt every
30 seconds, indefinitely — which is the intended cost of never having to restart minilog to
recover a sink.

Reopening does not repair a rotation that failed part way through: the sink appends to whatever is
on disk and takes its rotation accounting from the file sizes it finds, so a generation may be
missing or a file short. The next rotation proceeds from there.

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
| `exclude_facility` | — | Comma-separated facility names to leave out of whatever `facility` accepts. `facility = *` with `exclude_facility = local3` is "everything except local3". `*` is not allowed here |
| `include_malformed` | `true` | Write unrecognised (UNKNOWN) datagrams. `true`, `false`, `1` or `0` |

A datagram that parsed as neither RFC has no facility, and only a sink whose `facility` is `*`
receives it. `exclude_facility` cannot keep such a datagram out, since it has no facility to be
named by; that is what `include_malformed` is for. This is also why "all but local3" is spelled
with `exclude_facility` rather than by listing the other 23 names: an explicit list, however
long, is not the wildcard, and would drop those datagrams too. Naming a facility in
`exclude_facility` that `facility` does not accept anyway is harmless and changes nothing.

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
| `exclude_facility` | — | Facilities not to forward, taken out of what `facility` accepts; same rules as in `[output.*]` |
| `max_message_size` | `2048` | Truncate messages longer than this (bytes) to this size, ending in `[TRUNCATED: N bytes]`, N the original length. `0` = no limit; no unit suffix |

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

`kern`, `user`, `mail`, `daemon`, `auth`, `syslog`, `lpr`, `news`, `uucp`, `clock`, `authpriv`, `ftp`, `ntp`, `audit`, `alert`, `clock2`, `local0`–`local7`.

Aliases: `kernel`=`kern`, `security`=`auth`, `system`=`daemon`, `cron`=`clock`, `logaudit`=`audit`, `logalert`=`alert`.

These are also the names the JSONL `facility` field carries and the names `minilog-send --facility`
accepts, so a name read from a log can be written straight back into a config or a command line.

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

**This is not JSON escaping.** The JSONL sink writes ESC as `\u001b`, TAB as `\t`, and escapes
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

Run with docker-compose (mounts config from `./conf/minilog.conf`, writes logs to `./logs/`). The
`conf` directory is not in the repository; start from the example, whose log paths already suit
the container:

```
mkdir conf && cp minilog.conf.example conf/minilog.conf
docker compose up
```

Log file paths in the config must match the container's volume mount. With the default `docker-compose.yml` the log directory is `/var/log/minilog/`, so use paths like `/var/log/minilog/syslog.log`.

The Docker image contains only the syslog server. The web-viewer and cli-viewer are not included — run them on the host against the mounted log volume if needed.

## Windows deployment without the installer

Every release ships `minilog-<version>-win64.zip` beside the installer. It holds the same files
the installer lays down, and nothing that needs an installer to work: the executables are
statically linked, take every path they use from the command line or the config, and register
themselves as services. Copy the files where the deployment says, write a config, and run the
steps below. This is how minilog is meant to be rolled out as one component among others,
into directories that are not its own.

The archive contains one directory, `minilog-<version>\`:

| File | What it is |
|---|---|
| `minilog.exe` | the syslog server |
| `minilog-web-viewer.exe` | the web viewer |
| `minilog-send.exe` | the command-line sender; see [minilog-send](#minilog-send) |
| `minilog.pdb` | debug symbols for `minilog.exe`; optional, but a crash dump is only readable with the `.pdb` of the exact build, so keep it beside the executable |
| `minilog.conf` | the default configuration the installer ships; edit it, do not use it as is |
| `minilog-cli-viewer.py` | the CLI viewer, a Python 3 script with no dependencies |
| `minilog-cli-viewer.conf` | display preferences for the CLI viewer; optional |
| `LICENSE`, `README.md`, `CHANGES.md` | this documentation |

### Constraints to know before starting

- **One minilog per machine.** The service names `minilog` and `minilog-web-viewer` are fixed.
  A second copy cannot be registered alongside a first, and running `--install` from a new
  location repoints the existing registration at the new executable — which also means that a
  machine with the installer's minilog on it must have that uninstalled first, or the installer's
  later uninstall will remove the hand-placed service.
- **Both `--install` calls need an elevated prompt.** They write the service registration and an
  Event Log source under `HKLM`, whatever account the service later runs as.
- **The services run as LocalSystem and start automatically.** `--install` does not take an
  account or a start type. Change either afterwards with `sc config`; a later `--install`
  (an upgrade) leaves both as it finds them.
- **minilog creates no directories.** The log directory is provisioned by whoever deploys it,
  with an ACL that lets the service account write there.
- **Paths in the config must be absolute, and environment variables are not expanded.** A config
  generated per machine has to contain the expanded values.

### Installing

1. **Place the executables.** Any directory; the three need not share one. Nothing is read
   relative to the executable except the web viewer's default config path, and that is
   overridden below. `minilog-send.exe` reads nothing at all; put it wherever scripts will find
   it.

2. **Write the config.** Start from the shipped `minilog.conf` and put it wherever the deployment
   keeps configuration. Set `text_file` and `jsonl_file` in each `[output.*]` section to absolute
   paths in the log directory of your choosing, and `[web_viewer] host` / `port` to where the
   viewer should listen — an empty `host` is every interface. See [Configuration](#configuration)
   for the rest. Both services read this one file.

3. **Create the log directory** named by those paths. Grant the service account write access if
   it is somewhere LocalSystem cannot already write.

4. **Open the firewall** for inbound UDP on the syslog port, and for TCP on the viewer's port if
   it is to be reached from other machines. No local check can verify this, so it is listed here
   rather than by `--check`:

   ```
   netsh advfirewall firewall add rule name="minilog syslog" dir=in action=allow protocol=UDP localport=514
   netsh advfirewall firewall add rule name="minilog web viewer" dir=in action=allow protocol=TCP localport=9514
   ```

5. **Validate**, before anything is registered:

   ```
   D:\deploy\bin\minilog.exe --check D:\deploy\etc\minilog.conf
   ```

   It reports every problem in one run and exits non-zero if any is an error — a missing log
   directory, an unwritable one, a taken port, an unresolvable forwarding host. Fix and re-run
   until it exits zero. The `--check` section under Usage above lists everything it looks at.

6. **Register both services**, from an elevated prompt, giving the config path in full:

   ```
   D:\deploy\bin\minilog.exe --install D:\deploy\etc\minilog.conf
   D:\deploy\bin\minilog-web-viewer.exe --install --config D:\deploy\etc\minilog.conf
   ```

   Each records its own location as the OS reports it and the config path made absolute, sets
   the recovery actions (two restarts 5 s apart, reset after 300 s), and registers its Event Log
   source. Neither starts the service. A config the server cannot read is refused here rather
   than at the next boot.

7. **Start them** with `net start`, which waits for the outcome where `sc start` does not:

   ```
   net start minilog
   net start minilog-web-viewer
   ```

   A failed start says so on the console, and the reason is in the Application event log under
   the source `minilog` or `minilog-web-viewer`. Then send a message, confirm it lands in the
   log file, and open `http://<host>:9514/` in a browser:

   ```
   D:\deploy\bin\minilog-send.exe --port 514 deployment check
   ```

8. **The CLI viewer**, if wanted, needs a Python 3 interpreter and a pointer to the config, since
   its own search looks only in the current directory, `%ProgramData%\minilog` and beside the
   script:

   ```
   python D:\deploy\bin\minilog-cli-viewer.py --config D:\deploy\etc\minilog.conf
   ```

   A shortcut whose "Start in" field is the config directory does the same without the flag. The
   installer also puts this script on the system `PATH` and creates Start Menu and desktop
   shortcuts to the viewer URL; a manual deployment does either as it sees fit.

### Command-line options

Both executables take the same service verbs. Everything else the services need comes from the
config file, so there is nothing to pass at start time and nothing else to keep in step.

| `minilog.exe` | `minilog-web-viewer.exe` | Effect |
|---|---|---|
| `--check <config>` | — | validate the config and this machine, then exit; see above |
| `--install <config>` | `--install --config <config>` | register as a service, or update an existing registration; does not start it |
| `--stop` | `--stop` | stop the service and wait until its *process* has exited |
| `--uninstall` | `--uninstall` | stop as above, then remove the service and its Event Log source; succeeds if nothing is registered |
| `--timeout <seconds>` | `--timeout <seconds>` | how long `--stop` and `--uninstall` wait for the process; default 30, running out is an error |
| `<config>` | `--config <config>` | run in the foreground with this config; what the SCM runs, and useful for a first try at a console |

The web viewer's `--config` defaults to `minilog.conf` beside its own executable; the server has
no default and always takes the path as its positional argument.

### Upgrading

```
D:\deploy\bin\minilog.exe --stop
D:\deploy\bin\minilog-web-viewer.exe --stop
                                             copy the new executables over the old ones
D:\deploy\bin\minilog.exe --install D:\deploy\etc\minilog.conf
D:\deploy\bin\minilog-web-viewer.exe --install --config D:\deploy\etc\minilog.conf
net start minilog
net start minilog-web-viewer
```

`--stop` returns only once the process is gone, which is what lets the copy succeed; `sc stop`
and PowerShell's `WaitForStatus('Stopped')` return earlier than that. Re-running `--install` is
harmless when nothing moved and required when something did — it refreshes the executable path,
the Event Log message file and the recovery actions, and leaves the start type and account as
they are. Read the **Upgrading from** notes at the top of `CHANGES.md` for the release first:
some releases change what a config must contain.

### Removing

```
D:\deploy\bin\minilog-web-viewer.exe --uninstall
D:\deploy\bin\minilog.exe --uninstall
```

Then delete the files. The config and the logs are yours and are never touched.

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

## minilog-send

`minilog-send` builds a syslog datagram from its command line and sends it over UDP. It is for
scripts — a PowerShell or batch file that wants a line in the log, where Windows has no
`logger(1)` — and for proving a fresh deployment works end to end: `--check` shows the config and
the machine are right, and one message arriving in the viewer shows the whole path is.

The installer puts it in the `tools` directory, which is on the system `PATH`; the zip archive
ships it beside the other executables. On Linux it is built alongside `minilog` and installed to
the same `bin`.

```
minilog-send backup finished
minilog-send -s error -a deploy --msgid STEP3 release 1.4 failed on web01
minilog-send --host collector.example --port 5514 -f local3 hello from a script
minilog-send --rfc3164 -a legacy for an old collector
some-command 2>&1 | minilog-send -s warning -a some-command
```

The words on the command line are joined with single spaces into one message; put `--` before a
message that starts with a dash (a message made only of empty words, as from an unset shell
variable, is an error rather than a switch to stdin). With no words, stdin is read to its end and
then every non-empty line is sent as a message of its own, with the same header fields and a fresh
timestamp each — that is what makes it usable after a command in a pipeline. Empty lines are
skipped. Because nothing is sent until stdin closes, it is not a sink for `tail -f`: the lines would
wait for an end that never comes.

| Flag | Default | Field |
|---|---|---|
| `--host <host>` | `127.0.0.1` | where to send; a name or an IP address |
| `--port <port>` | `514` | UDP port |
| `-f`, `--facility <name\|0-23>` | `user` | the names under [Facility names](#facility-names), or a number |
| `-s`, `--severity <name\|0-7>` | `info` | `emergency`, `alert`, `critical`, `error`, `warning`, `notice`, `info`, `debug`, or a number; `emerg`, `panic`, `crit`, `err`, `warn`, `informational` are accepted too |
| `-a`, `--app <name>` | `minilog-send` | APP-NAME (RFC 5424) or the tag (RFC 3164) |
| `--hostname <name>` | this machine's name | HOSTNAME |
| `--pid <id>` | this process's pid | PROCID |
| `--msgid <id>` | none | MSGID; RFC 5424 only, an error with `--rfc3164` |
| `--rfc3164` | off | send `<PRI>Mmm dd hh:mm:ss HOST APP[PID]: MSG` instead of RFC 5424 |
| `-h`, `--help`, `--version` | | |

Names are case-insensitive. A `--host` name that resolves to several addresses is sent to the first
one the resolver returns — on a dual-stack machine `localhost` is usually `::1` — so if minilog
listens on IPv4 only, give the address rather than the name. The default format is RFC 5424 with a
local timestamp carrying its UTC offset (`2026-09-23T14:07:31.123456+02:00`), no structured data,
and `-` for a field not given. Header fields must be single words of printable ASCII, since that is
how the receiver takes the header apart, and no longer than the RFCs allow (RFC 5424: hostname 255,
app 48, pid 128, msgid 32 characters; RFC 3164: the `app[pid]` tag 32, and no `[`, `]` or `:` in app
or pid) — minilog itself would not mind, but another collector may. The message itself may contain
anything, and minilog escapes or replaces what it must. A message that would not fit in one UDP
datagram (65 507 bytes with its header) is refused, not truncated. On Windows the executable runs in
the UTF-8 code page, so a non-ASCII word on the command line is sent as UTF-8, as it is everywhere
else.

Exit status is 0 when every datagram left this machine, 1 when the host could not be resolved, a
send failed or a stdin line was too long for a datagram (the line that failed and how many were sent
before it are on stderr, and nothing after it is sent), and 2 for a bad command line, which includes
a command-line message too long for a datagram and a stdin with no non-empty line. Nothing is
written to stdout on success. **UDP gives no delivery receipt**: exit 0 means the message was sent,
not that anything received it. It does not read `minilog.conf` — the destination is what the flags
say, and it never writes to the Windows Event Log.

### Without minilog-send

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

The viewer also looks for `minilog-cli-viewer.conf` in `./`, then next to `minilog.conf`, for
display and filter settings. See
[`src/cli-viewer/minilog-cli-viewer.conf.example`](src/cli-viewer/minilog-cli-viewer.conf.example).

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
cd src/web-viewer
go build -o minilog-web-viewer .
```

`src/web-viewer/` is its own Go module, so the build must run from that directory.

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

Nothing limits how many requests are served at once, because the viewer is built for an operator
or a handful of them and the UI issues one read at a time per tab. A handful of tabs all paging a
sink of very large records simultaneously is a few hundred megabytes of transient memory, which is
the accepted ceiling. Putting the viewer somewhere that fans out requests — reachable by crawlers,
or serving many more than a handful of people — is outside what it is sized for.

Long lines are returned whole: a page ends between records, never inside one. That holds even for a
chain whose files do not all end in a newline — a rotated generation left unterminated by a process
killed mid-write is read as a record ending at the file's end, so the next page still starts on the
following record. The exception is the file being appended to right now: a record that was only
half written when the request took its snapshot is returned as far as it had got, and the remainder
turns up as the first line of a later page.

Neither ceiling is reported in the response. A page cut short by either advances `next_offset` to
just past the last line returned when paging forward, and sets `first_offset` to the start of the
oldest line returned when paging back, so a client continues from there and eventually sees
everything.

`/search` carries an offset per match and no record text — a client jumps to a match by asking
`/lines` for the window around its offset — so its response is small whatever the records behind
it are, and the byte ceiling does not apply to it.

Filter parameters `sev` and `fac` accept comma-separated name strings (e.g. `sev=info,warning`, `fac=auth,daemon`).

## License

MIT — see the [LICENSE](LICENSE) file for details.
