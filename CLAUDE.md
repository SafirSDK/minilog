# minilog — Claude Code context

## Project
Project name is "minilog", in all lower case.
Minimal but production-worthy C++20 syslog server (UDP receiver, RFC3164 + RFC5424 + malformed
messages, INI config, multiple output sinks with rotation, facility-based routing, UDP forwarding,
Windows service + Inno Setup installer). Intended primarily for Windows deployment; developed on Linux.

- Branch: `cpp-rewrite` (merges to `master` — see checklist below)
- Build: CMake + Boost (system package on Linux, Conan on Windows); Boost is the only external dep
- Test framework: Boost.Test + Python binary tests (`tests/test_binary.py`)
- Build presets: `linux-debug`, `linux-release`, `linux-docker`, `linux-coverage`, `linux-asan`,
  `linux-tsan`, `linux-asan-extended`, `linux-tsan-extended`, `linux-fuzz`, `windows-release`

## Technology choices
Boost throughout: Asio (networking), PropertyTree (INI parsing), JSON (JSONL output), Test (tests).
Direct WinAPI/POSIX for OS logging. Inno Setup 6 for the Windows installer.

## Conventions
Formatting enforced by `.clang-format` (Allman braces, 100-col limit, include grouping — read it).
- Naming: `camelCase` functions/vars/params; `m_camelCase` private members; `PascalCase` types;
  plain `camelCase` for public struct fields (e.g. `appName`, `maxSize`)
- File headers: MIT licence block at the top of every `.cpp`/`.hpp`; interior lines use ` *`
  (space before asterisk) to satisfy clang-format

## Architecture

### Threading model
N threads call `io_context::run()`. No hand-written queues — one `asio::strand` per output sink
serialises file I/O; one strand for the forwarder serialises UDP sends.

```
receive handler  →  copy buffer, re-arm immediately  →  post processing task
processing task  →  parse  →  post write to each matching sink strand
                          →  post to forwarder strand
```

### Log rotation
Triggered before each write if **either** `text_file` or `jsonl_file` exceeds `max_size`.
Both files always rotate together.

### RFC5424 structured data
Kept verbatim as a prefix of `message` — **not** parsed into a separate JSONL field.

## JSONL record format
UTF-8, one JSON object per line. Invalid UTF-8 bytes are replaced with U+FFFD before serialisation.
Fields: `rcv` (ISO8601 UTC), `src` (sender IP, no port), `proto` (`"RFC3164"`/`"RFC5424"`/`"UNKNOWN"`),
`facility`, `severity`, `hostname`, `app`, `pid`, `msgid`, `message`. Absent optionals → `null`.
Malformed messages (`proto="UNKNOWN"`): only `rcv`, `src`, `message` populated.

## Merge checklist (cpp-rewrite → master)
- Update `codecov.yml` branch from `cpp-rewrite` to `master`
- Update README badge URL from `branch/cpp-rewrite` to `branch/master`

