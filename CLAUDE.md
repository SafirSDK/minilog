# minilog — Claude Code context

## Project
Minimal but production-worthy C++20 syslog server (UDP receiver, RFC3164 + RFC5424 + malformed
messages, INI config, multiple output sinks with rotation, facility-based routing, UDP forwarding,
Windows service + Inno Setup installer). Intended primarily for Windows deployment; developed and
tested on Linux.

- Branch: `cpp-rewrite` (merges to `master` when ready)
- Build: CMake + Boost (system package on Linux, Conan on Windows)
- Test framework: Boost.Test + Python binary tests (`tests/test_binary.py`)
- Build presets: `linux-debug`, `linux-release`, `linux-docker`, `linux-coverage`, `linux-asan`,
  `linux-tsan`, `linux-asan-extended`, `linux-tsan-extended`, `linux-fuzz`, `windows-release`

## Technology choices

| Concern | Choice | Rationale |
|---|---|---|
| Networking | Boost.Asio | Mature, async, cross-platform |
| INI parsing | Boost.PropertyTree | Already a dependency |
| JSON output | Boost.JSON | Already a dependency |
| Testing | Boost.Test | Already a dependency |
| OS logging | Direct WinAPI / POSIX | Avoid extra dep for small task |
| Windows packaging | Inno Setup 6 | Free, scriptable, service support |

Single external dependency: **Boost** (via Conan on Windows, system package on Linux).

## Conventions
- Naming: `camelCase` for functions, local vars, parameters; `m_camelCase` for private members;
  `PascalCase` for types; plain `camelCase` for public struct fields (e.g. `appName`, `maxSize`)
- Brace style: Allman (enforced via `.clang-format`)
- File headers: MIT licence block at top of every `.cpp`/`.hpp`; interior lines use ` *` (space
  before asterisk) to satisfy clang-format

## Architecture

### Threading model
N threads (from config `workers`) all call `io_context::run()`. No separate queues or thread pools.

```
async_receive_from → receive handler (any thread)
  1. copy buffer, re-arm immediately
  2. post processing task back to io_context

processing task (any thread)
  - parse syslog message
  - post write to each matching sink's strand
  - post to forwarder's strand

LogFile strand   — one per output sink, serialises file I/O
Forwarder strand — serialises UDP sends
```

`asio::strand` per sink replaces mutexes — no hand-written work queues.

### Log rotation
Rotation is triggered **before** each write if **either** `text_file` or `jsonl_file` has exceeded
`max_size`. Both files always rotate together.

### RFC5424 structured data
Structured data is kept verbatim as a prefix of the `message` field (matches the original Python
reference). It is **not** parsed into a separate JSONL field.

## JSONL record format
One JSON object per line, UTF-8. All string fields are sanitised to valid UTF-8 before serialisation
(invalid byte sequences → U+FFFD).

```json
{"rcv":"2026-03-12T14:30:22Z","src":"192.168.1.50","proto":"RFC3164",
 "facility":"daemon","severity":"notice","hostname":"host","app":"su",
 "pid":"123","msgid":null,"message":"text here"}
```

- `rcv` — ISO8601 UTC receive timestamp
- `src` — sender IP (no port; ephemeral and not useful)
- `proto` — `"RFC3164"`, `"RFC5424"`, or `"UNKNOWN"`
- Absent optional fields → JSON `null`
- Malformed (`proto="UNKNOWN"`) — only `rcv`, `src`, `message` (raw bytes as string) populated

## ioc.poll() pattern in tests
After `lf.write(msg)` or `om.dispatch(msg)`, always call `ioc.restart()` before `ioc.poll()` —
Boost.Asio marks the io_context stopped after `poll()` empties, so `restart()` is required for
subsequent calls to work.

## Soak test notes
OOM under ASan: soak senders flooded at loopback speed; io_context work queue grew unbounded because
ASan slows consumer (file I/O) far more than producer. Not a leak — fix is 100 µs sleep between
sends in both soak loops (~10k msg/s per thread). Present in `tests/test_stress.cpp`.

## Merge checklist (cpp-rewrite → master)
- Update `codecov.yml` branch from `cpp-rewrite` to `master`
- Update README badge URL from `branch/cpp-rewrite` to `branch/master`
