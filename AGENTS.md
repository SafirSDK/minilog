# minilog — Claude Code context

## Project
Project name is "minilog", in all lower case.
Minimal but production-worthy C++20 syslog server (UDP receiver, RFC3164 + RFC5424 + malformed
messages, INI config, multiple output sinks with rotation, facility-based routing, UDP forwarding,
Windows service + Inno Setup installer). Intended primarily for Windows deployment; developed on Linux.

**WSL note:** If the host OS is Windows, development is done inside WSL (Ubuntu). All shell
commands (git, cmake, ctest, go, python, etc.) must be run via `wsl bash -c "..."` rather than
directly in PowerShell/CMD.

- Branch: `master`
- Build: CMake + Boost (system package on Linux, Conan on Windows); Boost is the only external dep
- Test framework: Boost.Test + Python binary tests (`tests/binary/test_binary.py`)
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
`facility`, `severity`, `hostname`, `app`, `pid`, `msgid`, `msg_time`, `message`. Absent optionals → `null`.
`facility` and `severity` are string names (e.g. `"daemon"`, `"INFO"`), not numeric codes.
`msg_time` is the raw timestamp string from the syslog message itself (verbatim, not normalised).
Malformed messages (`proto="UNKNOWN"`): only `rcv`, `src`, `message` populated.

## Viewers

### cli-viewer (`src/cli-viewer/`)
- Language: Python 3, no extra dependencies.
- Entry point: `minilog-cli-viewer.py`
- Behaviour: `tail -f` style — shows last N lines on startup (default 10), then follows new lines.
  Detects log rotation via inode change (POSIX) or file-size regression (Windows) and re-opens.
- Config discovery: looks for `minilog.conf` in `./`, platform default dir, then script dir;
  looks for `minilog-cli-viewer.conf` next to `minilog.conf` or `./`.
- Key classes/functions: `ViewerConfig`, `tail_file()`, `format_message()`, `should_display()`,
  `_file_id()`, `_open_shared()` (Windows-aware FILE_SHARE_DELETE open).
- Tests: `tests/cli-viewer/test_cli_viewer.py` (invoked via CTest).

### web-viewer (`src/web-viewer/`)
- Language: Go 1.25; single external dep: `golang.org/x/sys` (Windows service support only).
- Entry point: `main.go`; build with `go build ./src/web-viewer`.
- Serves an embedded SPA (`assets/`) over HTTP (default `:8080`).
- Reads `minilog.conf` to discover all `[output.*]` sections with `jsonl_file`; each becomes a
  named **sink** available in the browser's sink selector.
- Key packages/files:
  - `config.go` — INI parser, `Sink` struct, `loadSinks()`
  - `reader.go` — `FileChain` (logical byte-offset abstraction over rotation chain),
    `ReadForward()`, `ReadBackward()`, `Search()`, `Filter` struct
  - `handlers.go` — HTTP routes: `GET /sinks`, `GET /lines`, `GET /search`
  - `service_windows.go` — Windows NT service install/uninstall/run via `golang.org/x/sys/windows/svc`
  - `service_other.go` — no-op stubs for non-Windows
- `FileChain` is rebuilt per request (snapshots the filesystem); supports forward paging,
  backward paging (for infinite-scroll upward), and full-chain search across all rotated generations.
- Filter params on `/lines` and `/search`: `sev` (severity names), `fac` (facility names),
  `inc` (include substrings), `exc` (exclude substrings).
- Tests: `*_test.go` files in `src/web-viewer/`; run with `go test ./src/web-viewer/`.

## Release checklist

Before tagging a release, verify all of the following:

1. **Version numbers** — all three must match:
   - `CMakeLists.txt`: `project(minilog VERSION x.y.z ...)`
   - `installer/setup.iss`: `#define AppVersion "x.y.z"` (the fallback default)
   - `CHANGES.md`: `## vx.y.z` entry at the top

2. **Changelog** — `CHANGES.md` has a dated entry for the new version with all user-visible
   changes documented under `### New`, `### Changed`, `### Fixed` as appropriate.

3. **CI green** — all GitHub Actions jobs pass on the `develop` branch (or the release branch):
   - clang-format, header check, ruff lint
   - Linux GCC, Clang ASan+UBSan, Clang TSan
   - Linux coverage (C++ and Go)
   - Windows MSVC (build, tests, installer)
   - Docker build
   - libFuzzer

4. **README** — verify any new features, config options, or CLI flags are documented.

5. **AGENTS.md** — update if architecture, conventions, or component layout changed.

6. **Merge to master** — verify, tag, then merge:
   ```
   # Verify develop is a fast-forward of master (no divergence)
   git fetch origin
   git merge-base --is-ancestor origin/master develop

   # Tag on develop so the tag points to the tested commit
   git checkout develop
   git tag -a v1.2.0 -m "v1.2.0"

   # Fast-forward master to develop and push everything
   git checkout master && git merge --ff-only develop
   git push origin master develop --tags
   ```
   The tag push triggers the Windows installer build and GitHub Release upload.
