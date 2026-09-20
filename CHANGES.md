# Changelog

## Unreleased

### Fixed

- **The web viewer now opens the file minilog actually writes when the path contains `;` or `#`.**
  Its config parser stripped everything after the first `;` or `#` in a value, which no other
  reader of `minilog.conf` does: Boost's INI parser in the server keeps the whole value, and the
  cli-viewer's `configparser` is built without `inline_comment_prefixes`. With
  `jsonl_file = hash#name.jsonl` the server created and wrote `hash#name.jsonl` while the viewer
  opened `hash` — and a sink file that is not there looks exactly like a sink that has had no
  traffic yet, so the viewer showed an empty pane and no error. `#` is a legal filename character
  on NTFS and ext4 alike. Values now run to the end of the line in the viewer as well; `;` and `#`
  still start a comment at the beginning of a line. One consequence worth knowing: a trailing
  `max_files = 10 ; ten generations` is not a number to either end, so both fall back to the
  default of 10 rather than disagreeing about how deep to rotate.

- **The web viewer no longer holds half-open connections open forever.** `http.Server` was built
  with only `Addr` and `Handler`, and Go applies no timeouts by default, so a client that
  connected and then stopped talking was never hung up on. Fifty connections each carrying half a
  request header — about ten lines of script, no authentication and no traffic volume needed —
  were all still open after 20 seconds, each costing a goroutine, a file descriptor and a read
  buffer until the process ran out of descriptors and stopped accepting anything. On Windows the
  viewer is an auto-start LocalSystem service, so once wedged it stays down until somebody
  notices. The server now sets `ReadHeaderTimeout` (10 s), `ReadTimeout` (30 s) and `IdleTimeout`
  (120 s); the same 50-connection test now leaves none of them open. `WriteTimeout` is
  deliberately left unset, because a full-chain `/search` can legitimately take longer than any
  value worth setting and a truncated response is indistinguishable from a complete one.

- **One web-viewer URL can no longer exhaust the host's memory.** `count` on `/lines` and `limit`
  on `/search` were taken from the query string with no upper bound, and the read path
  materialises every matching line, copies it into a `[]string` and lets the JSON encoder buffer
  the whole response before sending a byte — several times the chain size in RSS. Against a 73 MB
  sink, `count=1000000000` returned an 89 MB body and took the process from 9 MB to 377 MB peak;
  at the documented defaults (`max_size = 100MB` x `max_files = 10`) that is roughly 6 GB for a
  single GET. Both are now clamped to 5000 lines: the same request returns 1.9 MB and peaks at
  21 MB. No attacker is needed for the old behaviour — a bookmarked URL, a typo or a crawler
  would do it — and because the viewer usually shares a host with the collector, the process
  killed for memory could be the syslog server. The browser UI never asks for more than 200
  lines, so no legitimate client is affected, and there is deliberately no setting for it.

- **The cli-viewer no longer lets a syslog sender drive the operator's terminal.** ESC survived
  the whole pipeline — the server wrote it as a JSON escape, so the file stayed well-formed, and
  `json.loads` handed it back as a real ESC byte, which `print()` passed to the terminal. Anyone
  able to send a datagram could clear the screen, move the cursor back over entries already
  printed, recolour a benign line as critical, change the window title, or write the clipboard.
  C0 control characters and DEL are now escaped in **every** displayed field, not just `message`:
  `hostname`, `app`, `msgid` and `pid` are parsed straight out of the datagram and are equally
  attacker-controlled. C1 (`U+0080`-`U+009F`) is escaped alongside C0, because `U+009B` and
  `U+009D` are the 8-bit CSI and OSC and reach the same sequences without an ESC byte at all. An
  embedded newline in a message now renders on one line instead of printing as a second,
  forged-looking entry. TAB and everything above C1 are untouched, so UTF-8 still displays, and
  the viewer's own colour codes are unaffected — those come from a table, never from the record.

- **A datagram can no longer forge a second entry in the text sink.** The text sink wrote the
  payload byte for byte, so an embedded newline ended the record and started another one that the
  sender had written in full — including its own PRI, so the forged line could claim a facility and
  severity the datagram never had, and nothing reading the file afterwards could tell it from a
  genuine entry. C0 control characters and DEL are now escaped before the write: `\n`, `\r`, `\\`
  for a literal backslash, `\xNN` for the rest, and `\uNNNN` for C1 (`U+0080`-`U+009F`), which
  holds the 8-bit forms of CSI and OSC and so drives a terminal paging the file without an ESC
  byte appearing anywhere in the datagram. C1 is matched as a decoded codepoint rather than a raw
  byte, so the continuation bytes of ordinary text are untouched; TAB stays literal and everything
  else above `0x7F` is written byte for byte, leaving UTF-8 unaffected. This is deliberately not
  JSON's escaping — the JSONL sink writes ESC as `\u001B` and escapes TAB and the double quote —
  but it is the same dialect the cli-viewer displays, so a line on screen reads the way a line in
  the file does. The JSONL sink, the web viewer and the forwarding path already handled the
  original problem correctly and are unchanged.

- **A UDP flood no longer grows minilog until the OS kills it.** The receive path had no admission
  control: each datagram was copied and posted to the io_context, the worker that picked it up
  posted a copy to every matching sink, and nothing anywhere asked how much was already queued. The
  sink is deliberately the narrow end of that pipe — it flushes after every line so `tail` sees
  entries at once — so a sustained flood from one unauthenticated source took the process from
  5 MB to 1.13 GB and still climbing, at the shipped `workers = 4`. The setting inverted the
  behaviour: `workers = 1` stayed flat at 5.8 MB because the thread accepting datagrams was the
  thread writing them, so `workers` was really "do you want a memory limit or not". minilog now
  charges each datagram against `[server] max_queue_bytes` (default 16 MB) before the first copy
  and drops it if the budget is full, holding the charge until the last queued copy of the message
  is written — bounding only the io_context queue would have moved the growth to the sink strands.
  The same flood now settles at 58 MB with one output section and does not climb. Dropped counts
  are reported to syslog or the Windows Event Log, batched to at most one notice every ten seconds
  so the notice cannot become the flood. Dropping is the right answer rather than a compromise —
  UDP syslog has no delivery guarantee and the kernel is already dropping silently when its own
  socket buffer fills — so there is deliberately no setting that removes the limit.
- **An RFC 3164 message without a tag no longer has its text moved into `app`.** The tag was taken
  to end at the first colon anywhere in the message, so any colon in ordinary text ended it: an
  `IP:port`, a URL scheme, a clock time. `<14>… myhost user logged in from 10.0.0.1:22 ok` was
  stored with `app` = `user logged in from 10.0.0.1` and `message` = `22 ok`, in the JSONL and text
  sinks both. The tag now has to end before the first space, which is what RFC 3164 means by a
  single-token TAG, so real tags (`sshd:`, `sshd[123]:`, `%BGP-5-ADJCHANGE:`) are unaffected and a
  colon in the body is left where it is.
- **A log file named twice in the config is now rejected instead of corrupting both outputs.**
  `text_file` and `jsonl_file` could be given the same path, in one section or across two, and the
  result was accepted and then wrong three ways over: raw text lines and JSON records were
  interleaved in the one file, so every JSONL reader silently skipped half of it; rotation shifted
  the generations once per writer, consuming two of `max_files` per rotation and leaving gaps in
  the numbering; and each writer sized the file against its own counter, so it grew to about twice
  `max_size` before either tripped. Two sections on one path additionally rotated and wrote it from
  two threads at once. Config load now refuses it, naming the section and the path — or both
  sections. Paths are compared as written, which is what catches the same path typed twice.
- **The web viewer's live tail no longer skips messages during a burst.** Each poll advanced its
  cursor to the end of the chain rather than past the lines it had just been given, and a response
  carries at most 200 lines. More than 200 matching lines arriving between two polls — they are
  500 ms apart — and everything past the first 200 was stepped over: still on disk, still findable
  by search, but never displayed until the view was reloaded. The cursor now advances by the
  `next_offset` the server has always returned for the purpose, so a burst drains one batch per
  poll. The visible consequence is that the live view falls behind the file and catches up rather
  than jumping to the end, which puts a ceiling of roughly 400 lines per second on what the live
  view can show; search, scrolling and the log itself are unaffected. Rotation is now detected
  against the previous response's end of chain, because the cursor no longer tracks it.
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

- **`text_file` and `jsonl_file` must now be absolute paths.** A relative path resolved against
  whatever working directory the reading process happened to have, and the three programs that
  read `minilog.conf` each had a different one: the server used its own CWD, the cli-viewer used
  its own, and the web-viewer resolved against the config file's directory. One configuration
  therefore named up to three different files. Under the Windows SCM the server's CWD is
  `C:\Windows\System32`, so a relative path aimed at a system directory and left a dead sink
  behind when the open failed — with nothing to say why. A relative path is now a config error
  naming the section and the key, and the message says that environment variables are not
  expanded, because `%ProgramData%\minilog\logs` is the next thing people try. UNC paths
  (`\\server\share\logs`) remain valid on Windows. Both viewers now use the configured value
  exactly as written, so there is no longer a resolution rule to keep three implementations
  agreeing on. The shipped `installer/minilog.conf` and `minilog.conf.example` already used
  absolute paths; a hand-written config with relative ones has to be corrected.

- **An RFC 3164 message without a tag now has `app` unset instead of its first word.** This goes
  with the parsing fix above. When no `tag:` is found the first word used to be taken as the app
  name and removed from the message, so `<14>… myhost Connection reset by peer` was stored as
  `app` = `Connection`, `message` = `reset by peer` — with no colon anywhere in it. Now `app` is
  `null` and the message is whole. Both changes alter how existing inputs parse: stored JSONL
  written before and after this release will differ in `app` and `message` for any message that
  was not properly tagged, and the web viewer's app filter and the CLI viewer's patterns will see
  a smaller, bounded set of app values. One case is knowingly left alone — a message opening with
  a bare clock time (`10:30:45 disk is full`) has a colon before any space and is still read as a
  tag.
- **Output files are opened at startup, not on the first message.** An unwritable or missing log
  directory is now a startup failure naming the path. Previously minilog started, reported itself
  running — to the SCM as well — and the sink then died on the first message, with no non-zero
  exit code and no recovery action. The side effect is that log files now appear as soon as
  minilog starts, rather than when the first message arrives.

### New

- **`[server] max_queue_bytes`** bounds the received-but-unwritten log held in memory; see the
  entry under Fixed. Takes the same units as `max_size`, so `16MB` and `16777216` both work, and
  defaults to 16 MB. `0` is rejected rather than meaning "unlimited".
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
