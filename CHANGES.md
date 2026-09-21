# Changelog

## Unreleased

### Changed

- **A config value that cannot be read is a startup error.** Making an unrecognised *key* a hard
  error left the other half of the same guarantee undone: `boost::property_tree`'s
  `get<T>(path, default)` returns the default on a failed translation as well as on an absent key,
  so `max_files = abc` was silently 10, `include_malformed = yess` silently `true`,
  `[forwarding] enabled = yess` silently off and `max_message_size = -1` silently 4294967295 —
  while `max_sise = 100MB`, a typo one character away, already failed the start naming the section
  and the key. A misspelled key and a misspelled value are the same operator mistake with the same
  consequence: a running server doing something other than what the file says. All four now fail
  the start, naming the section, the key and the value, as `[server] udp_port`,
  `[server] workers` and `[forwarding] port` already did — those three name their section now too,
  instead of only the key.

  `include_malformed` and `[forwarding] enabled` accept `true`, `false`, `1` and `0`, which is
  exactly what property_tree's own translator took; `yes`, `on` and `True` are errors rather than
  guesses, and the accepted set is documented in the README and `minilog.conf.example`.

  The known cost, accepted: `max_files = 10 ; ten generations` is a startup failure rather than a
  silent fall-back to the default. Values run to the end of their line — that is what makes a `#`
  or `;` legal in a log path — so somebody who wrote that expecting the comment to be stripped is
  better told than quietly given a different rotation depth. The web viewer rejects the same line
  for the same reason, rather than taking the default and disagreeing about how deep the chain
  goes with the component that owns the file.

### Fixed

- **Resolving the forwarding destination no longer holds up startup.** `Forwarder`'s constructor
  called `getaddrinfo` synchronously, before the UDP socket binds and before the Windows service
  reports itself running — all inside a single 10-second `SERVICE_START_PENDING` wait hint whose
  budget was written for "config load + sink open + socket bind". An unresponsive resolver is
  exactly when that call blocks longest, tens of seconds on a glibc host walking its `resolv.conf`
  attempts, and it is also the case the background retry exists for: the motivating scenario was
  the one that could have the SCM treat the start as hung. On Linux there is no SCM to complain,
  but the lookup still sat in front of the bind, so a collector could be unable to receive for the
  whole of it. The lookup is started by the constructor and completes on the io_context now, and
  everything the startup path does after it proceeds immediately. Messages arriving before it
  finishes are dropped, counted, and reported when it succeeds, the same way the retry path has
  always handled them. Stopping minilog while a lookup is in flight still waits for that lookup to
  return: Asio runs `getaddrinfo` on a thread of its own and `resolver::cancel()` only reaches
  operations still queued, which is now said where it used to be claimed otherwise. The Windows
  service reports a 30-second `SERVICE_STOP_PENDING` wait hint to cover that wait; it used to report
  none, which tells the SCM to expect the stop to be immediate. 30 s is what `--stop` waits by
  default, so both ends give up at the same point.

  An IP literal destination does not go through the resolver at all: the constructor parses it with
  `boost::asio::ip::make_address` and forwarding is live before the socket binds. Sending literals
  through the asynchronous lookup along with names was tried first, because one path for both reads
  better, but parsing a literal is not a lookup — no syscall, no resolver thread, nothing that can
  block — so it was paying the cost of the fix without having the problem the fix is for. Measured,
  because the window sounded too small to matter: on an idle host it never opened in 240 starts, but
  with the machine loaded about one start in fourteen lost the whole first burst to a destination
  that was reachable the entire time. That is what a collector restarting alongside minilog looks
  like. Nothing was silent about it — the local sinks kept every message and the log said how many
  were not forwarded — but the split now follows what can block rather than how the destination was
  spelled. A name still only has its lookup started, which is the part that needed to move.

- **`max_size = 0` now works as documented.** `config.hpp`, the README and `minilog.conf.example`
  all describe `0` as "no rotation", and `rotateIfNeeded` implements exactly that — but the parser
  rejected it, so the documented way to disable rotation was the one value that would not load.
  `max_queue_bytes` still rejects `0`: there is deliberately no unlimited setting for the receive
  queue.

- **A `max_size` too large for 64 bits is rejected instead of meaning "never rotate".**
  `17179869184GB` is 2^64 bytes, which wrapped to `0` — and `0` means no rotation, so a config
  asking for an enormous threshold silently asked for none at all and the sink grew until the disk
  filled. An out-of-range value also reports the section and key it came from; `std::stoull` throws
  `std::out_of_range`, a `std::logic_error`, which slipped past the handler that adds them, so the
  operator used to get `failed to load config: stoull` and nothing else.

- **`max_files` is bounded at 1000.** Every generation costs a filesystem existence check — on each
  rotation in the server, and on each HTTP request in the web viewer as it builds its file chain —
  so `max_files = 2000000000` was two billion stat calls to answer one GET. Both the server and the
  viewer reject anything higher: the viewer clamped at first, which would have had it show a chain
  depth no running minilog ever writes and say nothing about a config minilog will not start on.
  1000 is the number the viewer already used for `max_files = 0`, so both ends agree on how deep a
  chain can be.

- **The web viewer escapes the severity badge's class attribute.** The badge text was escaped and
  the `class` interpolation next to it was not, so a severity containing a double quote would close
  the attribute and turn the rest into attributes of its own. minilog cannot produce one — severity
  comes from a table of eight fixed names — but the viewer renders whichever `jsonl_file` the config
  points at, `sevLabel` passes any string through, and this was the only unescaped interpolation in
  a file whose whole job is rendering untrusted text.

- **A facility value with a quote or bracket no longer breaks the filter chips.** `addFacilityChip`
  interpolated the value into a `querySelector`, where such a value throws an uncaught
  `SyntaxError` — after which chip updates stopped for the rest of the session. The comparison is
  done in JavaScript instead.

- **The cli-viewer closes the file handle it is actually holding.** The follow loop re-opens the
  file on rotation and rebinds it, but the enclosing `with` block still held the original, so on
  exit it closed an already-closed handle and left the live one to the garbage collector. It uses
  an `ExitStack` now, which tracks whichever generation is open.

- **A `workers` typo is a config error instead of a crash.** Only the lower bound was checked, so
  `workers = 1000000` passed validation and `runServer` then looped spawning threads until
  creation failed; the `std::system_error` escaped through `runServer` and `main` to
  `std::terminate`. A core dump for a misplaced digit — and on Windows nothing in the Event Log,
  because `osLogError` was never reached. The value is now capped at 256, and the spawn loop
  reports a genuine resource failure and carries on with the workers it got rather than
  terminating.

- **A persistent receive error no longer spins a core.** Any error other than `operation_aborted`
  or `bad_descriptor` was logged and the socket re-armed immediately. For a transient error that
  is right; for a persistent one it was a tight loop at 100% of a core writing one log line per
  iteration into the host's system log — which on a syslog collector is frequently relayed back
  into minilog, so the loop fed itself. The re-arm now waits, from 50 ms doubling to a second, and
  further errors are counted rather than logged, with a summary at most once a minute that says
  whether they were all the same error. The delay is monotonic and only a successful receive
  resets it — a socket failing in two ways alternately is still a failing socket. An earlier
  draft restarted the backoff whenever the error message changed, which two errors alternating
  defeated completely: 100 such errors produced 100 log lines and never left the 50 ms delay,
  where 100 identical ones produced one line. A second draft reported the first error after every
  successful receive immediately, which a socket failing on every other receive defeated the same
  way — 100 error/success pairs produced 100 error lines and 100 recovery lines, and a success
  resets the delay, so it never left 50 ms either. The reporting interval therefore spans
  successes, and only a streak that was reported gets a "receiving again after N consecutive
  receive error(s)" line, counted across a mixed streak. A summary covering occurrences with a
  successful receive among them says "failing intermittently" rather than "still failing", which
  would claim nothing had been received since the last line. The delay still resets on a success: a
  socket that just delivered a datagram should re-arm at once.

- **The cli-viewer no longer exits on a record it did not expect.** `record.get("message", "")`
  defaults only when the key is *absent*, so `"message": null` produced `None`, which reached
  `.lower()` and ended the session with `AttributeError` — and the per-line handler caught
  `JSONDecodeError` only, so it came straight back out. The same held for a numeric `facility`
  reaching the colour table. minilog does not write such records, but the viewer reads whichever
  file the config names and a rotated file can be cut mid-write. Fields are coerced on read and an
  unshowable line is skipped instead of the session; printing stays outside the handler, so a
  `BrokenPipeError` from `| head` still ends the run rather than being skipped over line by line.

- **The web viewer now opens the file minilog actually writes when the path contains `;` or `#`.**
  Its config parser stripped everything after the first `;` or `#` in a value, which no other
  reader of `minilog.conf` does: Boost's INI parser in the server keeps the whole value, and the
  cli-viewer's `configparser` is built without `inline_comment_prefixes`. With
  `jsonl_file = hash#name.jsonl` the server created and wrote `hash#name.jsonl` while the viewer
  opened `hash` — and a sink file that is not there looks exactly like a sink that has had no
  traffic yet, so the viewer showed an empty pane and no error. `#` is a legal filename character
  on NTFS and ext4 alike. Values now run to the end of the line in the viewer as well; `;` and `#`
  still start a comment at the beginning of a line. One consequence worth knowing: a trailing
  `max_files = 10 ; ten generations` is not a number to either end, and both now reject it — see
  the entry below.

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

- **The README no longer claims IPv4-only.** It said "IPv6 is not supported" while
  `make_address` accepted an IPv6 literal and `udp::endpoint` then bound an IPv6 socket — a
  documented limitation the code did not implement, sitting directly above the Standards
  conformance section. The documentation now says what is true: one socket, one address family at
  a time, no dual-stack listener, and an IPv6 `host` binds an IPv6 socket but is exercised only by
  a loopback smoke test, so treat it as unsupported. Behaviour is unchanged, and the smoke test is
  new — it pins the path as working without claiming more than that. `minilog.conf.example` says
  the same on `[server] host`.

- **CI pins third-party actions to commit SHAs.** `ilammy/msvc-dev-cmd`, `softprops/action-gh-release`
  and `codecov/codecov-action` were referenced by tag, which is a mutable pointer — the code those
  jobs run could change with no change in the repository, and they hold `GITHUB_TOKEN` (release
  upload) and `CODECOV_TOKEN` (coverage upload). Each is now a full SHA with the version in a
  trailing comment, and the workflow says how to move one. `actions/*` are GitHub's own and stay on
  tags.

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

- **An unrecognised config key is now an error.** `loadConfig` read the keys it knew and ignored
  everything else, so `max_sise = 100MB` left the size at its default, `enabeld = true` left
  forwarding off and `faciltiy = auth` left the filter at the wildcard — a running server doing
  something other than what the file said, with nothing anywhere to say so. An unknown key in
  `[server]`, `[output.*]`, `[forwarding]` or `[web_viewer]` now fails the load, naming the
  section, the key and the valid ones. Sections minilog does not know are still ignored, so the
  file can carry another tool's settings; `[web_viewer]` is validated despite belonging to the web
  viewer, because minilog is the only component that validates this file at all.
  **Upgrading from v1.0.0 needs one edit:** that release's shipped `installer/minilog.conf`
  carried `encoding = utf-8` in `[server]`, a key removed in v1.1.0, and the installer writes the
  config `onlyifdoesntexist` — so an upgrade keeps the administrator's file verbatim and minilog
  refuses to start until that line is deleted. Configs from v1.1.0 onward are unaffected.

- **Security and cache headers on the web viewer.** Every response now carries a
  `Content-Security-Policy` (`default-src 'none'`, `script-src`/`style-src`/`connect-src` at
  `'self'`, `img-src 'self' data:`, `frame-ancestors 'none'`), `X-Content-Type-Options: nosniff`,
  `Referrer-Policy: no-referrer` and `Cache-Control: no-store`. The UI renders text a syslog
  sender chose, so the escaping in `app.js` is the control and the policy is the backstop for what
  it misses; the assets are all local and free of inline script, style and event handlers, which a
  test now checks so that a later edit cannot quietly make the policy wrong. `data:` is allowed
  for images because the search icon is an inline SVG. `no-store` also stops an upgraded viewer
  serving the previous version's `app.js` from a browser cache — the asset URLs carry no version.

- **`[forwarding] host` accepts a hostname.** It took an IP literal only —
  `boost::asio::ip::make_address` does not resolve names — while the shipped example config
  described the field as "hostname or IP address". Forwarding to a collector by name is the normal
  deployment shape; hard-coding its address on every syslog host is what people are trying to
  avoid. The destination is now resolved once, when minilog starts, and the address found is used
  for the lifetime of the process: re-resolving per message would put a name lookup on the hot
  path, and a collector that moves is rare enough to be worth a restart. A name that does not
  resolve at startup is **not** a startup failure — minilog runs with forwarding off, reports it
  once, and retries in the background with a growing delay (1 s, doubling to a minute), reporting
  how many messages were dropped when it finally succeeds. A Windows `AUTO_START` service is
  routinely running before DNS is, and losing the collector over an unreachable forwarding
  destination would be worse than losing forwarding. Values that cannot be a host at all —
  brackets, a space, a scheme, or a port appended — are still config errors at startup, because as
  names they would never resolve and would be retried silently forever; `10.0.0.999` is among
  them, since a hostname cannot have an all-numeric top-level label.

- **IPv6 forwarding destinations now work.** The forwarding socket was opened as
  `udp::v4()` regardless of the destination, so an IPv6 host passed config validation and then had
  nothing to send through. It is opened from the resolved endpoint's protocol instead. Receiving
  is unchanged and still IPv4.

- **`--config` and `--viewer-config` for the cli-viewer.** It was the only component that could
  not be told where its configuration lives: `minilog.exe` takes a path as an argument and
  `minilog-web-viewer` has `--config`, but the cli-viewer had a fixed search order and nothing
  else. That blocks deployments which put binaries and configuration inside an existing
  application tree rather than the platform directories, leaving "launch it from the config
  directory" or "keep a second copy of minilog.conf beside the script" — which then drifts. Both
  flags override the search outright, and a path that does not exist is an error rather than a
  quiet fall-back, because a typo in a deployment script would otherwise read some other
  configuration's logs and say nothing. Giving neither flag behaves exactly as before. The
  not-found message now lists the paths actually searched, including
  `%ProgramData%\minilog\minilog.conf` — it used to name `C:\Program Files\minilog\minilog.conf`,
  which is not one of them — and says that `--config` exists. Searching the current directory
  first is deliberate and now documented as such: a shortcut's "Start in" field selects which
  configuration the viewer picks up.

- **`[web_viewer] host` and `port`, replacing the web viewer's `--addr` flag.** The listen address
  was the one deployment fact that did not live in `minilog.conf` — it was a command-line flag,
  frozen into the Windows service registration at install time and supplied by the installer, so
  changing the port meant re-registering the service and editing `minilog.conf` did nothing. It is
  now read from the viewer's own section of the same file, and `--addr` is gone; the service entry
  is just `--config <path>`. Defaults are unchanged: an absent section still means every interface
  on port 9514. An absent or empty `host` means every interface on **both** IPv4 and IPv6 —
  writing `0.0.0.0` there would be IPv4 only, which is why the default is empty rather than an
  address. minilog itself ignores the section, so no server change was needed. The installer no
  longer takes a `WebViewerAddr` define: it reads the port back out of the config it has just
  installed to build the Start Menu and desktop shortcut URLs, which is what makes them right on
  an upgrade, where the config on disk is the administrator's with whatever port they chose. A
  shortcut still cannot follow an edit made after the install, and the shipped config says so
  where the edit happens.

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
