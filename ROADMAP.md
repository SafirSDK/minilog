# Roadmap — v1.4.0

Working order for the issues to be closed before the v1.4.0 release. Current
version is 1.3.0.

Batches 1–5 covered the original sweep (#10–#35). Batches 6–8 cover what is
left, including three issues (#38–#40) that came out of reviewing batch 5 rather
than from the original sweep. Completed batches stay in the file: their
rationale is what a context reset needs, and their being finished is not
something this file tracks.

**This file is temporary.** Delete it as part of preparing the v1.4.0 release —
git history keeps the record. It exists only to survive context resets while the
work is in progress.

**It records order and rationale, never status.** Progress lives in GitHub:
closing an issue is the only act that records it. `gh issue list --state open`
says what is left; this file says what order to take it in and why. Nothing here
needs ticking off or keeping in sync.

## How the batches were chosen

Ordered by dependency first, then clustered by the files each issue touches, so
each area of the codebase is opened once rather than revisited per issue.
Correctness before ergonomics within a cluster.

Clear context **per batch, not per issue** — the clustering exists so that later
issues in a batch reuse the files already loaded by earlier ones.

---

## Batch 1 — stop minilog lying about failure

**#26 → #13 → #17**

All three are "minilog fails and you cannot tell". Do **#26 first**: until the
SCM and Event Log are told the truth, every Windows failure in every later batch
is invisible and you would be debugging batch 2 blind.

#13 is labelled security but belongs here — "any filesystem error aborts the
whole server" is a normal-operations crash, and tight directory permissions are
exactly the target environment.

## Batch 2 — finish the Windows service and installer surface

**#33 → #25 → #21 → #35**, and settle **#22**

All in `service_win.cpp`, `main.cpp`, `service_windows.go` and
`tests/installer/test_installer.py`.

- #33 before #25: a service registered with wrong paths confuses every
  stop/start test written afterwards.
- #21 after #25, as decided on the issues — the idempotency work depends on the
  wait-for-stop fix.
- #22 is parked; the target environment's conventions already match what minilog
  registers. Decide whether to keep it parked or close it while in these files.

## Batch 3 — data integrity

**#27 → #28 → #30 → #29 → #10**

Losing or corrupting log lines is the worst failure class for a log collector.

#27 and #28 are both "the viewer silently does not show you things" and share
test infrastructure. #10 sits here rather than with the security batch because
it is really a policy question about behaviour under load — the same
conversation as #28.

## Batch 4 — remote-triggerable security

**#11, #12, #14, #15** — mutually independent; any order.

This is the one batch where parallel subagents would pay off, since the issues
do not touch each other and are largely mechanical.

## Batch 5 — config rationalisation and ergonomics

**#32 → #31 → #19 → #20 → #18 → #34 → #16 → #24**

#32's decision is already made (see below), so this batch starts by
implementing it. #31 shrank as a result: with relative paths gone, the viewers
drop their path-resolution logic entirely and #31 covers only the inline-comment
stripping in `config.go`.

#24 is a docs-only single-file change — good filler whenever something short is
wanted.

## Batch 6 — close the gaps batch 5 left

**#38 → #39 → #40**

All three came out of reviewing batch 5, and all three are corrections to code
that batch had just landed. They share the server and little else: #38 is in
`forwarder.cpp` and `main.cpp`, #39 in `receive_backoff.hpp`, #40 in
`config.cpp`. They belong together anyway — doing them while that code is still
in mind is worth more here than file locality is.

Ordered by severity, since nothing depends on anything else:

- **#38** can have the SCM declare a start hung. The forwarder resolves DNS
  synchronously before the UDP bind and before the service reports running, all
  inside one 10 s wait hint — and an unresponsive resolver, the case #18's retry
  design exists for, is exactly when `getaddrinfo` blocks longest. Suggested fix
  and the rejected alternative are on the issue.
- **#39** is a log loop, but needs an unusual trigger: two receive errors
  alternating reset the backoff on every call, so the socket re-arms every 50 ms
  and a line goes to the host's syslog each time — which a collector frequently
  relays back into itself.
- **#40** is the smallest, and also has to land before #23: `--check` reports
  config problems, and #40 changes which values are problems.

#40 carries a documentation tail — #31's `CHANGES.md` entry describes the
inline-comment fall-back as intended behaviour, and rejecting unparseable values
replaces it.

## Batch 7 — #37, the last web-viewer issue

#12 clamped `count`/`limit` to 5000 lines, which bounds how many lines a request
returns but not how many bytes: a sender who can reach the UDP port controls
line size, so a fed sink still reaches the multi-gigabyte response #12 set out
to remove.

Its own batch because it touches `reader.go` and `handlers.go` and nothing else
remaining goes near them — a C++ batch and a Go batch reuse nothing. Fold it
into batch 6 if fewer context clears are worth more than that; it is independent
of everything.

**Moved ahead of #36**, reversing the earlier ordering. That put #37 last partly
because "the byte budget wants the same judgement about what a client sees when
a page ends early that #36's recovery question raises about partial state" — on
re-reading both, that link is thin: a HTTP page cut short on a byte budget and a
sink closed by a filesystem error are not the same question. The other reason
given — it needs the collector fed first, so it is a step further out than #12,
which needed only a URL — is a priority argument rather than a dependency, and
#37 is now competing with three batch-5 corrections rather than with the rest of
the security work.

#12 parked the streaming refactor deliberately; start from that decision, not
from scratch.

## Batch 8 — running but not collecting

**#36 → #23**

The one pair left with a real dependency, and the only batch that starts with a
conversation rather than with code.

#36 decides whether a sink closed by a filesystem error ever reopens. #13 chose
"stays closed until restart" deliberately and it is the right default, but
"restart to recover" was accepted as a cost rather than decided on its merits.
Its stated preconditions have now landed: #10 settled behaviour under load, and
#31/#32 settled how much config surface is acceptable — which is what decides
whether a retry interval can be configurable.

#23 follows because its primary motivation *is* the dead sink — a log directory
with the wrong ACLs leaves the server running and silently discarding everything
routed to that sink. If #36 lands a retry, that case weakens, so #23's scope
depends on #36's answer.

**#23 may be closed rather than built.** The issue lists three things that
survive #26: the dead-sink case, validating a deployment before anything is
installed, and read-only diagnosis of a live system. #26 has landed, #36 may
finish the first, and the issue itself says closing is a reasonable outcome if
the remainder does not feel worth a new command. Decide that explicitly at the
top of this batch rather than starting to build.

---

## Decisions already made — do not relitigate

- **#32 — absolute paths only.** `text_file` and `jsonl_file` must be absolute;
  relative paths are rejected at config load. This means neither viewer needs
  path-resolution logic at all. Rationale and the four known implementation
  costs (Windows test paths, demo config generation, UNC paths, no environment
  variable expansion) are recorded on the issue.
- **#22 — closed as won't-do** (settled in batch 2). minilog's current
  registration (LocalSystem, AUTO_START, no dependencies) already matches the
  target deployment's conventions, so none of the proposed flags would be
  exercised. Keeping the service name a compile-time constant is also what makes
  the idempotent `--uninstall` from #21 unambiguous — there is no
  `--service-name` to typo. #21 preserves a hand-set account and start type
  across an upgrade, so an environment that mandates them can use `sc config`
  once instead.
- **#18 — resolve once at startup, retry in the background on failure.** The
  forwarding destination is resolved when the Forwarder is constructed and the
  endpoint reused for the process lifetime; re-resolving per message puts a
  lookup on the hot path, and a periodic re-resolve buys nothing until a
  deployment turns up whose collector moves. A name that does not resolve is
  reported once and retried rather than failing the start, because a Windows
  `AUTO_START` service is routinely running before DNS is. #38 changes only
  *how* the startup lookup is performed, not either of these.
- **#34 — an unknown config key is an error, not a warning.** Settled in batch
  5. Sections minilog does not know are still ignored, so the file can carry
  another tool's settings — claiming the whole namespace is what would have
  rejected the `[web_viewer]` section #19 added.
- **#16 — `max_size = 0` means no rotation.** Three documents and
  `rotateIfNeeded` already said so; only the parser disagreed.
  `max_queue_bytes` still rejects `0`: there is deliberately no unlimited
  setting for the receive queue.
- **#16 — `matchStringField` keeps its substring scan.** Parsing every line of
  a chain that can be gigabytes, on every request, is not the trade. The
  assumption it rests on — fixed field order with `message` last, table-driven
  facility/severity values — is documented at the function, in the README's
  JSONL section and in AGENTS.md instead.
- **#40 — unparseable config values are rejected**, consistent with unknown
  keys. The known cost is accepted: `max_files = 10 ; ten generations` becomes a
  startup failure rather than a silent fall-back, and #31's changelog entry
  describing that fall-back as intended has to be corrected with it.
- **Rotation races in the web-viewer — declined.** Requires `max_files = 1`
  together with high traffic, and self-corrects on refresh. Display-level
  integrity only.
- **`sanitizeUtf8` U+FFFD counting — declined.** One replacement character per
  byte rather than per maximal subpart. Output remains valid UTF-8; cosmetic.

## CI flakes to watch — not to chase

Noted as they turn up while running the builds for these issues. A flake seen
once is noise; the same one twice is a defect, and the point of this list is to
be able to tell the difference across context resets rather than re-diagnosing
it each time.

- **`test_binary.py::test_inflight_messages_complete_before_exit`, Windows.**
  Failed once as `17 != 20` (run 35359400495, 2026-09-18, on the #35 commit,
  which touches nothing but the installer and its test). Three of twenty
  datagrams sent in a tight loop never reached the log before the shutdown
  signal. A re-run passed. This is the same territory as **#10** — what happens
  to datagrams that arrive faster than they are processed — so if it recurs,
  record it here and treat it as evidence for that issue rather than as a test
  to loosen. The test allows 0.3 s between the last send and the signal, which
  is the first thing to look at if #10's answer turns out not to explain it.

## Minimum set if the rollout lands early

The original minimum — #26, #33, #17, so that a failed install or a typo'd
config cannot produce a service that reports success and does nothing — has
landed, along with the ergonomics that were to follow it (#19, #20).

Of what is left, two matter for a rollout and the rest can wait:

- **#38**, but only where `[forwarding]` is enabled with a hostname. A slow or
  unresponsive resolver at boot can make the SCM treat the start as hung, and it
  delays the UDP bind on every platform. An IP literal resolves instantly and is
  unaffected.
- **#36**, because a sink closed by a two-second storage blip stays closed until
  somebody restarts the service, and nothing after the moment it happened says
  so. A running service with a log file that stops mid-afternoon is the failure
  this release is otherwise about removing.

#39 needs two receive errors alternating to bite. #40 costs diagnosis quality
rather than correct operation. #37 needs either an attacker who can already
reach the UDP port or an application that honestly logs very long lines. #23 is
a convenience, and may not be built at all.
