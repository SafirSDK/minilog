# minilog

[![Build & Test](https://github.com/SafirSDK/minilog/actions/workflows/build.yml/badge.svg?branch=cpp-rewrite)](https://github.com/SafirSDK/minilog/actions/workflows/build.yml)
[![codecov](https://codecov.io/gh/SafirSDK/minilog/branch/cpp-rewrite/graph/badge.svg)](https://codecov.io/gh/SafirSDK/minilog)

A small UDP syslog server that understands RFC 3164 and RFC 5424. Receives datagrams, routes them to text and/or JSONL log files with rotation, and can forward to another syslog endpoint. Runs as a Windows service or a Linux process (systemd-friendly).

## Features

- UDP reception (no TCP/TLS)
- Parses RFC 3164, RFC 5424, and unrecognised ("UNKNOWN") datagrams
- Multiple named output sections, each independently filtered by facility
- Per-output text file (raw payload) and/or JSONL file (structured)
- Log rotation by file size with configurable retention count
- UDP forwarding with per-facility filtering and message truncation
- Windows service installation/removal via CLI flags
- Single external dependency: Boost

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

## Usage

```
minilog <config-path>
minilog --help
```

On Windows only:

```
minilog --install <config-path>    # register and start as a Windows service
minilog --uninstall                # stop and remove the Windows service
```

The config path is stored in the service registry entry so the same path is used when the service starts automatically on boot.

## Configuration

minilog reads a single INI file passed on the command line. There is no config reload; restart the process to pick up changes.

See [`syslog-server.conf.example`](syslog-server.conf.example) for a fully commented example.

### `[server]`

| Key | Default | Description |
|-----|---------|-------------|
| `host` | `0.0.0.0` | IP address to bind |
| `udp_port` | `514` | UDP port (1–65535) |
| `encoding` | `utf-8` | Byte encoding for incoming datagrams |
| `workers` | `4` | Number of I/O worker threads |

### `[output.<name>]`

Any number of named output sections. At least one of `text_file` or `jsonl_file` must be set.

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
| `host` | — | Destination hostname or IP |
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
{"rcv":"2026-03-12T14:30:22Z","src":"192.168.1.50","proto":"RFC3164","facility":"daemon","severity":"NOTICE","hostname":"mymachine","app":"su","pid":"123","msgid":null,"message":"text here"}
```

| Field | Type | Description |
|-------|------|-------------|
| `rcv` | string | ISO 8601 UTC receive time |
| `src` | string | Source IP address |
| `proto` | string | `"RFC3164"`, `"RFC5424"`, or `"UNKNOWN"` |
| `facility` | string\|null | Facility name |
| `severity` | string\|null | Severity name |
| `hostname` | string\|null | Syslog hostname field |
| `app` | string\|null | Application name |
| `pid` | string\|null | Process ID |
| `msgid` | string\|null | RFC 5424 MSGID field |
| `message` | string | Message text. For RFC 5424, structured data is kept as a prefix of this field. |

For `UNKNOWN` messages, only `rcv`, `src`, and `message` are populated; all other fields are `null`.

## Docker

Build the image:

```
docker build -t minilog .
```

Run with docker-compose (mounts config from `./conf/syslog-server.conf`, writes logs to `./logs/`):

```
docker compose up
```

Log file paths in the config must match the container's volume mount. With the default `docker-compose.yml` the log directory is `/var/log/minilog/`, so use paths like `/var/log/minilog/syslog.log`.

## Linux deployment (systemd)

No PID file is needed; systemd tracks the process directly. Example unit file:

```ini
# /etc/systemd/system/minilog.service
[Unit]
Description=minilog syslog server
After=network.target

[Service]
ExecStart=/usr/local/bin/minilog /etc/minilog/syslog-server.conf
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

## License

MIT — see the [LICENSE](LICENSE) file for details.
