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

Both Windows executables embed `artwork/minilog.ico`. The server uses `src/server/minilog.rc`
(compiled by MSVC). The Go web-viewer uses a pre-generated `src/web-viewer/rsrc_windows_amd64.syso`.
If the icon changes, regenerate the `.syso`:
```
cd src/web-viewer
go-winres simply --icon ../../artwork/minilog.ico --arch amd64
```
Install `go-winres` with `go install github.com/tc-hib/go-winres@latest` if needed.

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

### Sink failure and recovery
A filesystem error closes that one sink (`LogFile::failSink`) and drops every message routed to it;
the process and the other sinks are unaffected. A closed sink retries `openFiles()` on its own
`steady_timer` every `SinkRecovery::kRetryInterval` (30 s, deliberately not configurable) and is
re-reported once a minute while it stays closed. `SinkRecovery` (`output/sink_recovery.hpp`) holds
the reporting policy and nothing else, so it is unit-testable with an injected clock — the same
split as `ReceiveBackoff` and `AdmissionControl`. `LogFile::close()` sets a shutdown flag as well as
cancelling the timer: a pending retry keeps `io_context::run()` from returning, and `cancel()` alone
does not stop a handler that was already queued.

A failure inside `openAtStartup()` is the one that is **not** retried or rate-limited: `main()` turns
it into `EXIT_FAILURE` without ever running the `io_context`, so there is nothing alive to retry on
and the message must not promise one.

### Preflight (`--check`)
`preflight.hpp/.cpp` validates a config and the host without starting anything. `preflight()`
returns a `PreflightReport` (requirements + findings) and does the looking; `printPreflight()` does
the printing and returns the exit code, so every check is testable without capturing output. It goes
to stdout only — never `osLogError`, which would write to the Windows Event Log from a validation
run. Directory writability is tested with a probe file that is deleted again, never by opening the
configured log file (that would leave an empty `syslog.log` behind). `ServiceState` /
`queryServiceState()` in `platform/service.hpp` exist for this one decision: a failed bind while the
minilog service is running is a warning, not an error. It is passed into `preflight()` rather than
queried inside it, so the classification is testable on a host with no SCM. Only a *service* is
excused: on Linux (`NotApplicable`) a taken port stays an error, because nothing there can attribute
the socket to minilog — the message says that rather than guessing.

### RFC5424 structured data
Kept verbatim as a prefix of `message` — **not** parsed into a separate JSONL field.

## JSONL record format
UTF-8, one JSON object per line. Invalid UTF-8 bytes are replaced with U+FFFD before serialisation.
Fields: `rcv` (ISO8601 UTC), `src` (sender IP, no port), `proto` (`"RFC3164"`/`"RFC5424"`/`"UNKNOWN"`),
`facility`, `severity`, `hostname`, `app`, `pid`, `msgid`, `msg_time`, `message`. Absent optionals → `null`.
`facility` and `severity` are string names (e.g. `"daemon"`, `"INFO"`), not numeric codes.
`msg_time` is the raw timestamp string from the syslog message itself (verbatim, not normalised).
Malformed messages (`proto="UNKNOWN"`): only `rcv`, `src`, `message` populated.
**Field order is part of the format** and `message` is last: the web-viewer's severity/facility
filters scan the raw line for `"severity":` rather than parsing it, which is exact only because
of that ordering and because those values are table-driven (see `matchStringField` in `reader.go`).

## Viewers

### cli-viewer (`src/cli-viewer/`)
- Language: Python 3, no extra dependencies.
- Entry point: `minilog-cli-viewer.py`
- Behaviour: `tail -f` style — shows last N lines on startup (default 10), then follows new lines.
  Detects log rotation via inode change (POSIX) or file-size regression (Windows) and re-opens.
- Config discovery: looks for `minilog.conf` in `./`, platform default dir, then script dir;
  looks for `minilog-cli-viewer.conf` next to `minilog.conf` or `./`. `--config` and
  `--viewer-config` override either search and error if the path is missing (no fall-back).
- Key classes/functions: `ViewerConfig`, `tail_file()`, `format_message()`, `should_display()`,
  `escape_control_chars()` (C0/DEL escaping applied to every displayed field),
  `_file_id()`, `_open_shared()` (Windows-aware FILE_SHARE_DELETE open).
- Tests: `tests/cli-viewer/test_cli_viewer.py` (invoked via CTest).

### web-viewer (`src/web-viewer/`)
- Language: Go 1.25; single external dep: `golang.org/x/sys` (Windows service and Event Log
  support only), vendored under `src/web-viewer/vendor/`.
- Entry point: `main.go`; build with `go build ./src/web-viewer`.
- Serves an embedded SPA (`assets/`) over HTTP (default `:9514`).
- Reads `minilog.conf` to discover all `[output.*]` sections with `jsonl_file`; each becomes a
  named **sink** available in the browser's sink selector. The listen address comes from the
  viewer's own `[web_viewer]` section of the same file — there is no `--addr` flag, and the
  installer reads the port back out of the installed config to build its shortcut URLs.
- Key packages/files:
  - `config.go` — INI parser, `Sink` / `Config` structs, `loadConfig()`, `listenAddr()`
  - `reader.go` — `FileChain` (logical byte-offset abstraction over rotation chain),
    `ReadForward()`, `ReadBackward()`, `Search()`, `Filter` struct
  - `handlers.go` — HTTP routes: `GET /sinks`, `GET /lines`, `GET /search`
  - `service_windows.go` — Windows NT service install/uninstall/run via `golang.org/x/sys/windows/svc`,
    including recovery actions (two restarts, 5 s apart, 300 s reset period)
  - `service_other.go` — no-op stubs for non-Windows
  - `os_log_windows.go` / `os_log_other.go` — `osLogError`/`osLogInfo`; on Windows these also write
    to the `minilog-web-viewer` Event Log source registered by `--install`
- `FileChain` is rebuilt per request (snapshots the filesystem); supports forward paging,
  backward paging (for infinite-scroll upward), and full-chain search across all rotated generations.
- A chain file need not end on a line boundary — an interrupted write leaves a partial record, and
  rotation then moves that file into the middle of the chain. `ReadForward` charges `len(line)+1`
  for the newline the scanner strips, so it clamps its cursor to the file's snapshotted size;
  without that the next page starts one byte into the following generation. `ReadBackward` and
  `Search` walk from the `'\n'` bytes that are really there and need no equivalent. The active file
  is the case this does not make whole: a record half written when the request snapshotted the file
  is returned truncated, and its remainder becomes the first line of a later page.
- Two ceilings bound one `/lines` request, neither configurable and neither reported in the
  response: `maxLines` (5000, `handlers.go`) on lines returned, and `maxResponseBytes` (8 MB,
  `reader.go`) on their total size. Both leave the paging cursor just past what was returned, so a
  client continues from there; a line is never truncated to fit. Note that `maxResponseBytes` bounds
  the JSONL collected off disk, not the response body — `encoding/json` expands `<`, `>` and `&` to
  `\u00NN`, which boost::json does not escape when writing the record, so records dense in them
  reach ~6× that in body bytes.
- `/search` returns an offset per match and no record text, so neither ceiling bounds its size and
  it has no paging cursor. Clients jump to a match by asking `/lines` for the window around the
  offset. `total_matches` counts the whole chain even when `limit` truncated the offsets.
- Concurrent requests are deliberately not capped: the viewer is sized for an operator or a handful,
  `app.js` issues one read at a time per tab, and a few hundred MB of transient memory is the
  accepted aggregate ceiling. Settled — see the `maxResponseBytes` comment for the reasoning and
  for what would change it. Do not re-raise it as an open memory bound.
- Filter params on `/lines` and `/search`: `sev` (severity names), `fac` (facility names),
  `inc` (include substrings), `exc` (exclude substrings).
- Tests: `*_test.go` files in `src/web-viewer/`; run with `go test ./src/web-viewer/`.

## CI flakes to watch — not to chase

Noted as they turn up. A flake seen once is noise; the same one twice is a defect, and this list
exists to tell the difference across context resets rather than re-diagnosing it each time. Remove
an entry once its cause is found and fixed, or once it has gone a few releases without recurring.

- **`test_binary.py::test_inflight_messages_complete_before_exit`, Windows.** Failed once as
  `17 != 20` (run 35359400495, 2026-09-18, on the #35 commit, which touches nothing but the
  installer and its test). Three of twenty datagrams sent in a tight loop never reached the log
  before the shutdown signal; a re-run passed. This is the territory of #10 (admission control —
  what happens to datagrams that arrive faster than they are processed), so if it recurs, record it
  here and treat it as evidence about that path rather than as a test to loosen. The test allows
  0.3 s between the last send and the signal, which is the first thing to look at if the admission
  path turns out not to explain it.

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
   Re-check the **Standards conformance** section if the receive path, parser, or forwarding
   truncation changed — the RFC 5426 table makes claims that must stay true (buffer size,
   default port, `src` vs `hostname` separation).

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
   The tag push triggers the Windows build and uploads both `minilog-<version>-setup.exe` and
   `minilog-<version>-win64.zip` to the GitHub Release. The zip's file list lives in three places
   that must agree: `cmake/package_zip.cmake`, `tests/installer/test_zip_install.py`
   (`EXPECTED_FILES`) and the table in the README's "Windows deployment without the installer".
