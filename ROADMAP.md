# Roadmap — v1.4.0

Working order for the 27 issues (#10–#36) to be closed before the v1.4.0
release. Current version is 1.3.0.

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

## Batch 6 — #23 last

`--check` is a preflight that validates everything the earlier batches
establish, and it depends on #26. Writing it before that validation exists means
writing it twice. Re-evaluate its scope when reached.

## Batch 7 — #36, after everything else

A sink closed by a filesystem error stays closed until minilog is restarted. #13
chose that deliberately and it is the right default, but "restart to recover"
was accepted as a cost rather than decided on its merits.

Deliberately last. It is a design decision rather than a defect, and the choice
depends on batches that come first: #10 settles behaviour under load, and
#31/#32 settle how much config surface is acceptable — which decides whether a
retry interval can be configurable. Starting from the options recorded on the
issue rather than from scratch.

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

**#26, #33, #17.** Without those three, a failed install or a typo'd config in a
locked-down Windows environment produces a service that reports success and does
nothing, with no Event Log trail. #20 and #19 are the ergonomics to add next,
but they are comfort rather than correctness.
