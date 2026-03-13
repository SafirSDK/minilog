# Plan: C++ Syslog Server Rewrite

## Context

Rewriting the Python `syslog-server.py` as a production-grade C++20 application. The Python version is a single-file dev tool; the C++ version is a Windows service with log rotation, facility-based routing, multiple output formats, UDP forwarding, and an Inno Setup installer. Must also build and test on Linux for developer convenience.

---

## Requirements Summary

- UDP reception, RFC3164 + RFC5424 + malformed messages handled
- INI file configuration
- Multiple `[output.X]` sections, each with:
  - `text_file` (raw UDP payload + newline) and/or `jsonl_file` (parsed JSON)
  - `facility` filter (comma-separated facility names, `*` = all)
  - `include_malformed` flag
  - `max_size` (e.g. `100MB`) — rotate both files when either exceeds this
  - `max_files` (0 = unlimited)
- Rotated filenames use numeric suffix before extension: `syslog.1.log`, `syslog.2.log`, `syslog.1.jsonl`, `syslog.2.jsonl`
- `[forwarding]` section: UDP-only, facility filter, `max_message_size` (truncate with `... [TRUNCATED: N bytes]`)
- Server errors → Windows Event Log (Windows) or syslog (Linux), + stderr
- No stdout output, no config reload (restart to reconfigure)
- Max UDP payload buffer: 65507 bytes hardcoded (65535 − 20 IP header − 8 UDP header)
- Windows service support + Inno Setup installer
- GitHub Actions CI: Linux + Windows runners

---

## Technology Choices

| Concern | Choice | Rationale |
|---|---|---|
| Build system | CMake + Conan | Conan handles Boost cleanly |
| Presets | `CMakePresets.json` | Easy Linux/Windows switching |
| Networking | Boost.Asio | Mature, async, cross-platform |
| INI parsing | Boost.PropertyTree | Already a dependency |
| JSON output | Boost.JSON | Already a dependency |
| Testing | Boost.Test | Already a dependency |
| Internal logging | Direct (WinAPI / POSIX) | Avoid extra dep for small task |

Single external dependency: **Boost** (managed via Conan).

---

## Project Structure

```
minilog/
├── CMakeLists.txt
├── CMakePresets.json
├── conanfile.txt
├── src/
│   ├── main.cpp                    # Entry point, service wiring
│   ├── config/
│   │   ├── config.hpp/.cpp         # INI parsing → Config structs
│   ├── parser/
│   │   ├── syslog_message.hpp      # SyslogMessage struct
│   │   └── syslog_parser.hpp/.cpp  # RFC3164 + RFC5424 parsing
│   ├── server/
│   │   └── udp_server.hpp/.cpp     # Boost.Asio UDP receiver
│   ├── output/
│   │   ├── output_manager.hpp/.cpp # Route messages to output sinks
│   │   └── log_file.hpp/.cpp       # File writing + rotation
│   ├── forwarder/
│   │   └── forwarder.hpp/.cpp      # UDP forwarding with truncation
│   └── platform/
│       ├── os_log.hpp              # OS logging interface
│       ├── os_log_win.cpp          # Windows Event Log
│       ├── os_log_linux.cpp        # Linux syslog()
│       ├── service_win.cpp         # Windows SERVICE_MAIN etc.
│       └── service_linux.cpp       # Signal handling, PID file
├── tests/
│   ├── CMakeLists.txt
│   ├── test_parser.cpp             # RFC3164/RFC5424 parsing unit tests
│   ├── test_config.cpp             # INI loading unit tests
│   ├── test_output.cpp             # Rotation, file writing tests
│   ├── test_forwarder.cpp          # Truncation logic tests
│   └── test_integration.cpp        # UDP send → file receive end-to-end
├── conanfile.txt                       # Boost dependency declaration
└── installer/
    └── setup.iss                       # Inno Setup script
```

---

## Configuration File (final spec)

```ini
[server]
host = 0.0.0.0
udp_port = 514
encoding = utf-8
workers = 4

[output.main]
text_file = C:\logs\syslog.log
jsonl_file = C:\logs\syslog.jsonl
max_size = 100MB        ; supports B, KB, MB, GB
max_files = 10          ; 0 = unlimited
facility = *
include_malformed = true

[output.auth]
text_file = C:\logs\auth.log
max_size = 50MB
max_files = 5
facility = auth,authpriv
include_malformed = false

[forwarding]
enabled = false
host = 10.0.0.5
port = 514
protocol = udp
facility = *
max_message_size = 2048
```

---

## JSONL Record Format

One JSON object per line, UTF-8:
```json
{"rcv":"2026-03-12T14:30:22Z","src":"192.168.1.50","proto":"RFC3164","facility":"daemon","severity":"NOTICE","hostname":"mymachine","app":"su","pid":"123","msgid":null,"message":"text here"}
```

- `rcv`: ISO8601 UTC receive time
- `src`: source IP (no port — port is ephemeral, not useful)
- `proto`: `"RFC3164"`, `"RFC5424"`, or `"UNKNOWN"`
- Null JSON fields (`null`) for absent optional values
- Malformed messages: `proto="UNKNOWN"`, only `rcv`, `src`, `message` (raw bytes as string) populated

### Open Question: RFC5424 Structured Data

Currently structured data is kept verbatim as a prefix of `message` (matches Python reference). Options to discuss:

1. **Status quo** — structured data stays in `message` as raw text (e.g. `[exampleSDID@32473 iut="3" eventSource="Application"] actual message`)
2. **Strip SD, expose as `message`** — parse out SD block, set `message` to the human-readable MSG portion only
3. **Parse SD into JSONL** — add a `structured_data` field (object or array) to JSONL records, strip from `message`

Considerations: option 3 is the most useful for log analysis tools but adds significant parser complexity and a new JSONL schema field. Option 2 is a clean middle ground. Option 1 is simplest but pollutes the `message` field with structured data syntax.

**Decision needed before implementing JSONL output (step 6).**

---

## Text File Format

Raw UDP payload bytes written as-is, with a single `\n` appended. One message per line. No other modification.

---

## Architecture

### Threading Model

N threads all call `io_context::run()` (N = `workers` from config). All work lives inside the io_context — no separate queues or thread pools.

```
[io_context]  — N threads calling run()
     |
     | async_receive_from  (re-arms itself immediately on each receive)
     v
[receive handler]  — runs on whichever thread picks it up
   1. copy buffer (buffer is immediately re-armed for next receive)
   2. post processing task back to io_context
     |
     v
[process task]  — runs on any available thread
   - decode bytes (encoding)
   - parse syslog message
   - dispatch write to each matching output sink's strand
   - dispatch to forwarder's strand
     |
     v
[LogFile strand]  — one strand per output sink, serializes file I/O
[Forwarder strand] — serializes UDP sends
```

- `asio::strand` per output sink replaces mutexes — Asio-native synchronization
- No hand-written work queue needed
- Forwarder dispatches through its own strand (UDP is fire-and-forget; no retry)

### Output Routing

For each received (and decoded) message:
1. For each `[output.X]` in config:
   - Check facility match (or `*`)
   - Check `include_malformed` if `proto == UNKNOWN`
   - If match: write raw line to `text_file` (if configured), write JSON to `jsonl_file` (if configured)
2. If forwarding enabled: check facility, truncate if needed, send UDP

### Log Rotation

Triggered before each write if either `text_file` or `jsonl_file` exceeds `max_size`:
1. Close both files
2. Shift existing rotated files up by one: `.2` → `.3`, `.1` → `.2`, current → `.1`
3. If `max_files > 0`: stat all rotated files for this base name, delete oldest beyond limit
4. Open fresh files

---

## Windows Service

- `main()` detects if running interactively (console) or as service
- Service: calls `StartServiceCtrlDispatcher`
- Console: runs directly (useful for testing on Windows)
- Install/uninstall via CLI flags: `--install`, `--uninstall`
- Service name and display name configurable in `setup.iss`

---

## Inno Setup Installer

- Packages the `.exe` and a default `syslog-server.conf`
- Installs to `{pf}\MiniLog`
- Registers and starts Windows service on install
- Stops and removes service on uninstall
- Open source (MIT) compatible
- PDB file (`minilog.pdb`) packaged as an optional "Debug Symbols" component (not installed by default)

---

## GitHub Actions CI

```yaml
# .github/workflows/build.yml
# Four jobs:
# 1. linux:          ubuntu-latest, GCC, CMake, Boost.Test tests
# 2. linux-san:      ubuntu-latest, Clang, ASan+UBSan build + tests, TSan build + stress tests
# 3. linux-coverage: ubuntu-latest, GCC --coverage, lcov summary + Codecov upload
# 4. windows:        windows-latest, MSVC + Ninja, CMake, Boost.Test tests + Inno Setup packaging
```

Conan with `conanfile.py`, binary caching enabled for fast CI.

### Coverage job detail
- New `linux-coverage` CMake preset: GCC with `--coverage -fno-inline`, `CMAKE_BUILD_TYPE=Debug`
- After `ctest`: run `lcov --capture`, strip system headers and `tests/` from results, print summary with `lcov --list`
- Upload `coverage.info` to Codecov via `codecov/codecov-action@v4`
- Requires `CODECOV_TOKEN` secret set in repo settings (Settings → Secrets → Actions)

---

## Docker

Multi-stage `Dockerfile` at repo root:
- **Build stage**: `debian:bookworm` + GCC + CMake + Ninja + libboost-all-dev; builds with `linux-release` preset
- **Runtime stage**: `debian:bookworm-slim`; copies only the `minilog` binary; runs as a non-root user
- Exposes UDP port 514; config file mounted via volume at `/etc/minilog/syslog-server.conf`
- CI (`build.yml`) adds a `docker` job: `docker build .` to verify the image builds on every push (no push to registry)
- `docker-compose.yml` for local development: mounts `./conf/syslog-server.conf` and `./logs/` volume

---

## Test Plan

### Parser unit tests (`test_parser`)
Ported from Python plus significantly extended:
- RFC3164: basic, no PID, no message body, very long hostname/app/message
- RFC3164: PRI boundary values (0, 191=max valid, 192=invalid)
- RFC3164: timestamp variants (single-digit day with leading space)
- RFC5424: basic, nil values (`-`), structured data present but ignored, BOM prefix
- RFC5424: version != 1 (should fall through to UNKNOWN or RFC3164)
- RFC5424: multiple structured data elements
- Both formats: embedded `\r\n` in message portion
- Both formats: null bytes (`\0`) in message
- Both formats: non-UTF-8 byte sequences
- Both formats: message exactly at and just over 65507 bytes
- UNKNOWN: empty datagram, random binary, plain text with no PRI
- Real-world corpus: Linux kernel, OpenSSH, Cisco IOS, Juniper, Windows Event Log forwarded via syslog

### Config unit tests (`test_config`)
- Valid config with all fields, minimal config (defaults), missing file (expect defaults or clean error)
- Invalid port (0, 65536, negative, non-numeric)
- Invalid max_size (no unit, unknown unit, negative, zero)
- All 24 valid facility names; unknown facility name; mixed case
- Output section with neither text_file nor jsonl_file (should warn/error)
- Duplicate output section names
- max_files = 0 (unlimited), max_files = 1
- Config file with Windows CRLF line endings
- File paths with spaces and special characters

### Output unit tests (`test_output`)
- Write single line to text_file, verify exact bytes (payload + `\n`, nothing else)
- Write single record to jsonl_file, verify valid JSON with all expected fields, nulls for absent optionals
- Rotation: verify both files rotate together when text_file exceeds max_size
- Rotation: verify both files rotate together when jsonl_file exceeds max_size
- Rotation numbering: `.1` → `.2` shift on second rotation, `.N` on Nth
- max_files = 3: after 4 rotations verify only 3 rotated files exist (oldest deleted)
- max_files = 0: after many rotations verify nothing deleted
- Facility routing: message with `facility=auth` reaches `output.auth` and `output.main` (facility=*) but not `output.mail`
- include_malformed = false: UNKNOWN messages not written
- include_malformed = true: UNKNOWN messages written
- Write to non-existent directory (expect clean error, not crash)

### Forwarder unit tests (`test_forwarder`)
- Message below max_message_size: forwarded unchanged
- Message at exactly max_message_size: forwarded unchanged
- Message one byte over: truncated, contains `[TRUNCATED: N bytes]` indicator
- Message far over limit: truncated correctly
- Facility filter: matching facility forwarded, non-matching dropped

### Integration tests (`test_integration`)
- Full pipeline: send RFC3164 → verify text_file has raw line, jsonl_file has correct fields
- Full pipeline: send RFC5424 → same verification
- Both RFC3164 and RFC5424 messages in same session
- Malformed message: reaches include_malformed=true output, absent from include_malformed=false output
- Verify JSONL `rcv` field is a valid ISO8601 UTC timestamp
- Verify JSONL `src` field is the sender IP
- Graceful shutdown: messages in flight are completed before exit
- **Forward-to-self**: spin up two server instances on different ephemeral ports; instance A has forwarding pointed at instance B; send messages to A; verify both A and B output files contain the messages (with correct content on each side)

### Stress & robustness tests
- **Flood**: 10,000 messages as fast as possible from a single sender — verify line count in output files matches, no corruption
- **Concurrent senders**: 8 threads sending simultaneously — verify no torn/interleaved lines in output files
- **Max-size datagrams**: 65507-byte UDP payload — no crash, no buffer overrun
- **Adversarial input**: empty datagram, 1-byte datagrams, all-zero bytes, all-255 bytes, random binary — server must not crash
- **Rotation under flood**: force rotation by setting tiny max_size, hammer with messages — verify no corruption and correct file count
- **Forwarding unreachable**: forwarding enabled, target host down — server keeps running, no hang, errors logged

### Sanitizer & fuzzer builds (Linux/Clang only)
- **ASan + UBSan build**: run full unit + integration test suite under AddressSanitizer and UndefinedBehaviourSanitizer — catches buffer overflows, use-after-free, UB
- **TSan build**: run concurrent/stress tests under ThreadSanitizer — catches data races
- **libFuzzer target**: fuzz the parser entry point with random + semi-valid syslog input corpus — target: no crashes, no sanitizer findings

---

## Implementation Order

1. CMake + Conan skeleton (builds empty binary on Linux + Windows)
2. GitHub Actions CI (build-only, no tests yet — catches Windows compile errors from day one)
3. Config parser + unit tests
4. Syslog parser + unit tests (port Python test cases to Boost.Test) + libFuzzer target (Linux/Clang)
5. Add file headers to all source files (copyright/licence block + brief description)
6. LogFile (write + rotation) + unit tests
7. OutputManager (routing) + unit tests
8. UDP server (Boost.Asio) + integration tests
9. Add CI check that every .cpp/.hpp has the correct file header
10. Forwarder + unit tests
11. Platform layer (OS log, Windows service, Linux signal handling)
12. main.cpp wiring
13. Inno Setup installer
14. Dockerfile + docker-compose.yml + Docker CI job
15. Stress & robustness tests:
    - Flood: 10 000 messages as fast as possible from a single sender — verify line count matches, no corruption
    - Concurrent senders: 8 threads sending simultaneously — no torn/interleaved lines
    - Max-size datagrams: 65507-byte UDP payload — no crash, no buffer overrun
    - Adversarial input: empty datagram, 1-byte, all-zero, all-0xFF, random binary — server must not crash
    - Rotation under flood: tiny max_size + high message rate — no corruption, correct file count
    - Forwarding unreachable: target host down — server keeps running, no hang
16. libFuzzer target for the syslog parser (Linux/Clang only):
    - Fuzz entry point over `parseSyslog()` with random + semi-valid corpus
    - Goal: no crashes, no sanitizer findings
    - Add `linux-fuzz` CMake preset and a CI job that runs the fuzzer for a fixed duration
17. Remove Python implementation (`syslog-server.py`, `syslog-server.conf`, `tests/test_syslog_server.py`)
18. Update README and write documentation:
    - Rewrite README.md for the C++ version (build instructions, configuration reference, output format, Windows service install/uninstall)
    - Write `syslog-server.conf.example` with all options documented and commented
    - Add `BUILDING.md`: how to build on Linux (nix-shell + CMake) and Windows (Conan + MSVC + CMake)
    - Add `CHANGELOG.md` with initial release notes
