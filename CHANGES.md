# Changelog

## Unreleased

### Fixed

- **The web viewer's tail view no longer loses log entries longer than 64 KB.** `ReadBackward`
  reads the rotation chain backwards in 64 KB chunks and treated the first byte of every chunk as
  the start of a line, so a record spanning a chunk boundary reached the browser as one fragment
  per chunk — none of them valid JSON, all of them silently discarded by the renderer. One byte of
  real content was deleted at each boundary as well. A single datagram is enough to produce such a
  record: the maximum is 65507 bytes, and JSON escaping expands a control character sixfold. The
  backward reader now carries the leading fragment of a line into the next chunk and emits the line
  only once the newline preceding it has been found, so long entries appear in the tail view and in
  upward scrolling just as search and forward paging already showed them. A line over 1 MB — the
  ceiling the forward reader has always had — is skipped rather than buffered.
- **The installer now removes its system `PATH` entry on uninstall.** The installer appends
  `{app}\tools` to the machine-wide `PATH`, and nothing ever took it out again: Inno does not
  revert a `{olddata}`-style append on its own, so every uninstall left a `PATH` entry pointing at
  a directory that no longer exists. It also accumulated — the duplicate guard only suppresses a
  second entry while the first is still present with the same `{app}` value, so install, uninstall,
  install elsewhere left two. The uninstaller now reads `PATH` at uninstall time, removes only its
  own entry (tolerating case and a trailing backslash), and writes the result back only if it
  changed, leaving every other entry byte for byte in order.
- **`--uninstall` now waits for the service to stop before deleting it.** Both implementations
  requested the stop and deleted immediately — the C++ one with no wait at all, the Go one with a
  flat 500 ms sleep. `ControlService` is asynchronous, and deleting a service that is still running
  only *marks* it for deletion: the registration lingers and the next `--install` fails with
  `ERROR_SERVICE_MARKED_FOR_DELETE`. Both now wait for `SERVICE_STOPPED` and then for the process
  itself to exit, and report a timeout instead of deleting anyway. This has not been seen in
  practice because services stop quickly under light load, which is what made it worth fixing: it
  would have shown up first on the busiest machine in an estate.
- **`--install` no longer registers a service that cannot start.** The command line written into
  the service entry was built from `argv[0]` prepended with the working directory — which is not
  where the executable is when it was found through `PATH` — and from the config path exactly as
  typed, so `minilog --install minilog.conf` stored a relative path that the SCM resolves against
  `System32` at boot. Both were accepted by `CreateService`, so `--install` reported success and
  the failure surfaced only at the next start. The image path now comes from the OS, the config
  path is made absolute, and a config file that cannot be read is refused instead.
- **An invalid `host` is now a config error, not a crash or a silent exit.** Neither `[server]
  host` nor `[forwarding] host` was validated, and both are passed to an address parser that does
  not resolve names. A hostname or typo in `[forwarding] host` aborted the process with `SIGABRT`;
  the same in `[server] host` exited non-zero with nothing on stderr and nothing in the Event Log.
  `loadConfig` now rejects both, naming the key and the value, and startup failures from the
  server socket are reported by the caller so they can no longer be swallowed.
  `minilog.conf.example` said "hostname or IP address" and now says IP address. An address with a
  port appended (`10.0.0.5:514`) is rejected too — the Windows address parser accepted it and
  silently discarded the port.
- **A filesystem error no longer aborts the whole server.** Six `std::filesystem` calls on the
  write and rotation paths used the throwing overloads. An exception from any of them escaped the
  sink's strand handler and then `io_context::run()` on a worker thread, where it became
  `std::terminate` — an unreadable log directory took down every sink, including those whose own
  storage was healthy. All filesystem calls on that path now use the `error_code` overloads, a
  failure takes only the affected sink out of service, and handlers plus each `run()` thread have
  a catch-all so that no exception can terminate the process. A sink closed this way stays closed;
  restart minilog once the storage problem is fixed. A caught handler exception is reported as a
  failed run rather than a clean stop — surviving is not the same as being healthy, and on Windows
  it is what lets the service's recovery actions fire.
- **Windows services now report failure to the SCM.** Both the server and the web viewer used to
  report every stop as a clean one with exit code 0, so a service that died on an invalid config
  or an unbindable port was indistinguishable from one stopped on purpose — and no recovery action
  could ever have fired. Each service now stays in `SERVICE_START_PENDING` until startup has
  actually succeeded, and reports a service-specific error with a non-zero exit code when it
  fails, visible in `sc query`. (`sc start` returns before startup resolves, so it still reports
  success; `net start` waits for the outcome.)

### Changed

- **Output files are opened at startup, not on the first message.** An unwritable or missing log
  directory is now a startup failure naming the path. Previously minilog started, reported itself
  running — to the SCM as well — and the sink then died on the first message, with no non-zero
  exit code and no recovery action. The side effect is that log files now appear as soon as
  minilog starts, rather than when the first message arrives.

### New

- **`--install` and `--uninstall` are now idempotent.** `--install` used to fail against a service
  that already existed (`ERROR_SERVICE_EXISTS`, or an explicit check in the web viewer) and
  `--uninstall` used to fail when there was none, which is why the installer ran `--uninstall` on
  both executables with its errors deliberately swallowed. `--install` now updates an existing
  registration — binary path, arguments, display name, description, recovery actions and Event Log
  source — while leaving the start type and the service account alone, so an administrator who
  bound the service to a specific account or set it to manual start keeps that across an upgrade.
  It reports "installed" or "updated" and exits 0 either way; it never starts or stops anything.
  `--uninstall` against an absent service exits 0. The installer now stops the services with
  `--stop` before copying files instead of deregistering them, so a hand-tuned registration
  survives an upgrade.
- **`--stop` on both executables.** `minilog --stop` and `minilog-web-viewer --stop` stop the
  service and wait until its process has genuinely exited, with `--timeout SECONDS` (default 30)
  bounding the wait. This is what an upgrade needs between stopping the old build and copying the
  new one: `sc stop` and PowerShell's `WaitForStatus('Stopped')` wait on SCM state, and a service
  reports itself stopped before its process has released the executable file. Stopping a service
  that is already stopped, or not registered, succeeds. The README documents the upgrade sequence.
- **Windows service recovery actions.** `--install` now configures both services to be restarted
  by the SCM 5 seconds after a failure, twice, before being left stopped, with the failure
  counter resetting after 300 seconds without a failure. Previously a service that died stayed
  dead until someone noticed.
- **Event Log source for the web viewer.** `--install` registers a `minilog-web-viewer` Event Log
  source and `--uninstall` removes it. A web viewer running as a service has no console, so its
  startup failures previously left no trace at all; they are now written to the Windows Event
  Log. Interactive runs still log to stderr.

## v1.3.0 — 2026-04-28

### New

- **Web viewer — message detail panel**: clicking a log row expands an inline detail panel
  showing all fields. Columns are resizable by dragging header edges.
- **Web viewer — Clear Screen button**: hides all current messages so only new incoming entries
  are shown. Implemented via a server-side `since` parameter on `/lines`.
- **Web viewer — toolbar improvements**: renamed Clear/Reset buttons with descriptive tooltips,
  repositioned Clear Screen to a centered toolbar zone, and gave it a distinct visual style.
- **Installer shortcuts**: the Windows installer now creates Start Menu and Desktop shortcuts for
  the web viewer (optional component, selected by default).
- **Application icon**: both Windows executables (server and web viewer) now embed the minilog
  icon.
- **Pre-release support**: tagged pre-releases (e.g. `v1.3.0-rc1`) are automatically published as
  pre-release GitHub Releases.

### Changed

- **Web viewer default port**: changed from 8080 to 9514 to avoid conflicts with commonly used
  development ports. The port 9514 is unregistered with IANA and alludes to the syslog port (514).
- **README**: the summary, features, and limitations sections now mention the bundled viewers,
  default Windows installation, and the lack of authentication/TLS on the web viewer.
- **Installer tests**: added web viewer smoke tests — verifies the binary is installed, the
  service is registered and running, and the HTTP `/sinks` endpoint responds after installation.

## v1.2.0 — 2026-04-09

### New

- **minilog-web-viewer**: a Go HTTP server with an embedded single-page app for browsing minilog
  JSONL logs in a browser. Supports multi-sink selection, severity/facility/text filtering,
  full-chain search across rotated files, infinite scroll, and live tail polling. Runs as a
  Windows service or standalone process. Bundled in the Windows installer as an optional component.

### Changed

- **Repository restructure**: server sources moved to `src/server/`, cli-viewer to
  `src/cli-viewer/`, tests to `tests/server/`, `tests/cli-viewer/`, `tests/binary/`.
- **JSONL severity/facility fields** are now string names (e.g. `"daemon"`, `"INFO"`) rather than
  integer codes, matching the server's actual output. The web-viewer filters on these strings.
- **`udp_port = 0`** is now accepted in the config file (OS-assigned ephemeral port).
- **Go coverage** is now uploaded to Codecov alongside the C++ coverage, with separate flags.

### Fixed

- **Receive strand serialised parsing**: parse+dispatch work is now posted to the `io_context`
  directly, enabling parallel parsing across the worker thread pool.
- **Rotation gap detection**: the server now probes all slots up to `max_files` instead of
  stopping at the first missing generation, preventing orphaned files after manual deletion.
- **Forwarder socket** is now created on its strand for consistency with the strand-per-sink
  pattern.
- **LogFile destructor** no longer calls `closeFiles()` directly, preventing a potential data race
  with queued strand work.
- **`parseSize` error messages** now include the output section name for easier debugging.
- **cli-viewer tests** replaced `select()`-based synchronisation with a thread-based accumulator,
  fixing failures on Windows.
- **Go INI parser** now strips `#` inline comments, matching the C++ parser behaviour.

## v1.1.0 — 2026-04-02

### New

- **minilog-cli-viewer**: a standalone Python 3.10+ script for real-time viewing of minilog JSONL
  output. Reads `minilog.conf` automatically to locate the log file. Features include configurable
  column display, include/exclude message filters, ANSI colour output by facility and severity,
  three timestamp formats, `--lines`/`--show-all` modes (similar to `tail -f`), and automatic
  re-open after log rotation. Configured via an optional `minilog-cli-viewer.conf` file.
  See `minilog-cli-viewer.conf.example` for all options.

### Fixed

- **Windows Event Viewer messages** (#2): the service now registers its own message table resource
  so that Event Viewer resolves log entries against the minilog executable. Previously every entry
  showed "The operation completed successfully" instead of the actual message text.
- **Default config contained unused `encoding` field** (#1): the `encoding` key in
  `[output.main]` was not a recognised option and has been removed from the installed default
  `minilog.conf`.

## v1.0.0 — 2026-03-16

This is the first release of minilog, a small, robust UDP syslog server. It receives RFC 3164 and RFC 5424 datagrams, routes them to one or more output sinks based on facility, and writes plain-text and/or structured JSONL log files with automatic rotation. Messages can also be forwarded to another syslog endpoint.

Releases contain an installable native Windows service, and can be built for Linux or Docker  for other use cases. It is configured via a single INI file. 

See [README.md](README.md) for details on features, configuration and development.
