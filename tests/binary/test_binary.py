#!/usr/bin/env python3
"""
Binary-level tests for minilog.

Invoked by CTest as:
    python3 test_binary.py <path-to-minilog-binary>

Or directly for development:
    python3 tests/test_binary.py ./build/linux-debug/minilog
"""

import gzip
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from collections.abc import Callable
from pathlib import Path

BINARY: str = ""  # set from argv[1] before test discovery

# On Windows, processes must be in their own process group so that
# CTRL_C_EVENT can be delivered without also interrupting the test runner.
_POPEN_FLAGS: dict = (
    {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if sys.platform == "win32" else {}
)


# ── helpers ──────────────────────────────────────────────────────────────────


def free_port() -> int:
    """Return an ephemeral port that is currently unused."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_for_text(path: Path, needle: str, timeout: float = 10.0) -> bool:
    """Block until *needle* appears in the file at *path*.

    For output a process writes at a moment of its own choosing. Reading the
    file once after stopping the process only sees what happened to be written
    by then, which turns "did it report this?" into a race with shutdown.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists() and needle in path.read_text(errors="replace"):
            return True
        time.sleep(0.02)
    return False


# Logged by minilog once its socket is bound.
SERVER_READY = "minilog: listening on UDP"


class RunningServer:
    """A minilog started on a fresh port, receiving by the time it is entered.

    Readiness is the line minilog logs once its socket is bound, never a probe of
    the port: a probe that binds the port itself can hold it at the instant the
    server binds, and the server then exits with "address in use" (#48). The port
    comes from free_port(), which another process can still take before minilog
    binds it, so a start that fails to bind is retried on a new port.

    *make_conf* writes the config for a given port and returns its path. stderr
    goes to a file in *d*, for stderr() or for wait_for_text() on stderr_path.
    Leaving the block stops the server; proc.returncode is set afterwards.
    """

    ATTEMPTS = 3

    def __init__(self, d: Path, make_conf: Callable[[int], Path]):
        self.dir = d
        self._make_conf = make_conf

    def __enter__(self) -> "RunningServer":
        for attempt in range(1, self.ATTEMPTS + 1):
            self.port = free_port()
            self.conf = self._make_conf(self.port)
            self.stderr_path = self.dir / f"minilog-stderr-{attempt}.txt"
            self._err = self.stderr_path.open("w")
            self.proc = subprocess.Popen([BINARY, str(self.conf)], stderr=self._err, **_POPEN_FLAGS)
            if self._wait_ready():
                return self
            if self.proc.poll() is None:
                self.proc.kill()
            self.proc.wait(timeout=10)
            self._err.close()
            if "failed to bind" not in self.stderr():
                break
        raise AssertionError(f"server did not start: {self.stderr()!r}")

    def __exit__(self, *exc) -> bool:
        try:
            terminate(self.proc)
            self.proc.wait(timeout=10)
        finally:
            self._err.close()
        return False

    def _wait_ready(self, timeout: float = 10.0) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if SERVER_READY in self.stderr():
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.02)
        return False

    def stderr(self) -> str:
        return self.stderr_path.read_text(errors="replace")


def send_udp(msg: str, port: int) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.sendto(msg.encode(), ("127.0.0.1", port))


def write_config(
    d: Path,
    port: int,
    *,
    workers: int = 1,
    max_size: str = "100MB",
    forward_to: int = 0,
    log_dir: Path | None = None,
    compress: bool = False,
) -> Path:
    log_file = (log_dir or d) / "syslog.log"
    conf = d / "minilog.conf"
    lines = [
        "[server]",
        "host = 127.0.0.1",
        f"udp_port = {port}",
        f"workers = {workers}",
        "",
        "[output.main]",
        f"text_file = {log_file}",
        f"max_size = {max_size}",
        "max_files = 0",
        "facility = *",
        "include_malformed = true",
    ]
    if compress:
        lines.append("compress = true")
    if forward_to:
        lines += [
            "",
            "[forwarding]",
            "enabled = true",
            "host = 127.0.0.1",
            f"port = {forward_to}",
            "facility = *",
            "max_message_size = 0",
        ]
    conf.write_text("\n".join(lines))
    return conf


def terminate(proc: subprocess.Popen) -> None:
    """Send a graceful shutdown signal."""
    if sys.platform == "win32":
        # Processes are started with CREATE_NEW_PROCESS_GROUP, which disables
        # CTRL_C handling in the child (Windows sets SetConsoleCtrlHandler(NULL,TRUE)
        # implicitly).  CTRL_BREAK_EVENT is not maskable and is delivered to the
        # process group; the C++ server handles it via signal_set(SIGBREAK).
        proc.send_signal(signal.CTRL_BREAK_EVENT)
    else:
        proc.send_signal(signal.SIGTERM)


def _is_root() -> bool:
    return hasattr(os, "geteuid") and os.geteuid() == 0


# Denying access to a directory only proves anything where the permission bits
# are enforced: POSIX, and not as root.
CAN_DENY_ACCESS = sys.platform != "win32" and not _is_root()


def count_lines(path: Path) -> int:
    try:
        return path.read_text().count("\n")
    except FileNotFoundError:
        return 0


# ── CLI argument tests ────────────────────────────────────────────────────────


class TestCLI(unittest.TestCase):
    def test_no_args_exits_nonzero(self):
        r = subprocess.run([BINARY], timeout=5)
        self.assertNotEqual(r.returncode, 0)

    def test_bad_config_path_exits_nonzero(self):
        r = subprocess.run([BINARY, "/nonexistent/does-not-exist.conf"], timeout=5)
        self.assertNotEqual(r.returncode, 0)

    def test_too_many_args_exits_nonzero(self):
        r = subprocess.run([BINARY, "a", "b", "c"], timeout=5)
        self.assertNotEqual(r.returncode, 0)

    @unittest.skipUnless(sys.platform == "win32", "--install is a Windows-only option")
    def test_install_with_unreadable_config_registers_nothing(self):
        """Registering a service against a config it cannot read only moves the
        failure to the next boot, where it is far harder to see.

        The check runs before the SCM is opened, so this case needs no
        privileges -- unlike the rest of --install, which is covered by
        tests/installer/test_installer.py.
        """
        with tempfile.TemporaryDirectory() as d:
            missing = Path(d) / "no-such-file.conf"
            r = subprocess.run(
                [BINARY, "--install", str(missing)],
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("no-such-file.conf", r.stderr)


# ── --check preflight ─────────────────────────────────────────────────────────


class TestCheckMode(unittest.TestCase):
    """`--check` validates the config and the machine without running anything.

    The checks themselves are covered by tests/server/test_preflight.cpp; what
    only the real executable can show is that the flag is wired up, that the
    report goes to stdout, that the exit code follows the findings, and that a
    run leaves nothing behind.
    """

    def test_valid_config_reports_no_problems_and_exits_zero(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            conf = write_config(d, free_port())

            r = subprocess.run(
                [BINARY, "--check", str(conf)],
                capture_output=True,
                text=True,
                timeout=10,
            )

            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("No problems found", r.stdout)
            self.assertIn("This configuration requires:", r.stdout)

    def test_check_creates_no_log_file(self):
        """Running the server creates syslog.log eagerly; --check must not.

        A preflight that changes the machine it inspects cannot be run twice with
        the same meaning -- and on a host being provisioned, a stray empty log
        file with the wrong owner is its own fault to chase down.
        """
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            conf = write_config(d, free_port())

            subprocess.run(
                [BINARY, "--check", str(conf)],
                capture_output=True,
                timeout=10,
                check=True,
            )

            self.assertEqual([p.name for p in sorted(d.iterdir())], ["minilog.conf"])

    def test_missing_log_directory_exits_nonzero(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            conf = write_config(d, free_port(), log_dir=d / "not-provisioned")

            r = subprocess.run(
                [BINARY, "--check", str(conf)],
                capture_output=True,
                text=True,
                timeout=10,
            )

            self.assertNotEqual(r.returncode, 0)
            self.assertIn("does not exist", r.stdout)
            self.assertIn("not-provisioned", r.stdout)

    def test_unloadable_config_exits_nonzero(self):
        with tempfile.TemporaryDirectory() as d:
            conf = Path(d) / "minilog.conf"
            conf.write_text("[server]\nudp_port = not-a-number\n")

            r = subprocess.run(
                [BINARY, "--check", str(conf)],
                capture_output=True,
                text=True,
                timeout=10,
            )

            self.assertNotEqual(r.returncode, 0)
            self.assertIn("config:", r.stdout)

    def test_check_without_config_path_exits_nonzero(self):
        r = subprocess.run([BINARY, "--check"], capture_output=True, text=True, timeout=5)
        self.assertNotEqual(r.returncode, 0)

    def test_port_held_by_another_process_is_reported(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as holder:
                holder.bind(("127.0.0.1", 0))
                conf = write_config(d, holder.getsockname()[1])

                r = subprocess.run(
                    [BINARY, "--check", str(conf)],
                    capture_output=True,
                    text=True,
                    timeout=10,
                )

            # No minilog service is registered on a CI runner, so the port being
            # taken is a real error rather than the expected state.
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("cannot bind UDP", r.stdout)

    def test_check_against_a_running_minilog_reports_the_port_as_taken(self):
        """The bind test is the one check that a healthy machine can fail.

        Only a minilog running *as a service* is excused, and that path needs a
        Windows SCM (covered by tests/server/test_preflight.cpp).  A minilog
        started by hand -- what this test can arrange on either platform -- is
        indistinguishable from any other process holding the port, so the report
        is a hard error.  What is pinned here is that --check says so, and that
        it stays a read-only observer: the live server keeps logging after it.
        """
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            with RunningServer(d, lambda port: write_config(d, port)) as srv:
                r = subprocess.run(
                    [BINARY, "--check", str(srv.conf)],
                    capture_output=True,
                    text=True,
                    timeout=10,
                )
                self.assertIn(str(srv.port), r.stdout)
                self.assertIn("cannot bind UDP", r.stdout)
                self.assertNotEqual(r.returncode, 0)

                send_udp("<34>Oct 11 22:14:15 mymachine su[1]: still alive", srv.port)
                self.assertTrue(wait_for_text(d / "syslog.log", "still alive"))

            self.assertEqual(srv.proc.returncode, 0)


# ── Basic smoke test ──────────────────────────────────────────────────────────


class TestSmoke(unittest.TestCase):
    def test_receive_and_write(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            with RunningServer(d, lambda port: write_config(d, port)) as srv:
                send_udp("<34>Oct 11 22:14:15 mymachine su[1]: smoke test", srv.port)
                time.sleep(0.3)

            self.assertEqual(srv.proc.returncode, 0)
            log = (d / "syslog.log").read_text()
            self.assertIn("smoke test", log)

    def test_port_busy_exits_nonzero(self):
        """If the configured UDP port is already in use the binary must exit
        with a non-zero code promptly (no hang)."""
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as blocker:
                blocker.bind(("127.0.0.1", 0))
                busy_port = blocker.getsockname()[1]
                conf = write_config(d, busy_port)

                proc = subprocess.Popen([BINARY, str(conf)], **_POPEN_FLAGS)
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                    self.fail("process hung on port-busy instead of exiting")

            self.assertNotEqual(proc.returncode, 0)

    def test_two_rfc_formats_both_written(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            with RunningServer(d, lambda port: write_config(d, port)) as srv:
                send_udp("<34>Oct 11 22:14:15 mymachine su[1]: from3164", srv.port)
                send_udp("<34>1 2026-03-12T14:30:22Z host app 1 - - from5424", srv.port)
                time.sleep(0.3)

            log = (d / "syslog.log").read_text()
            self.assertIn("from3164", log)
            self.assertIn("from5424", log)


# ── Log injection ─────────────────────────────────────────────────────────────


class TestLogInjection(unittest.TestCase):
    """One datagram must be one line in the text sink, whatever it contains.

    The reproduction from the issue: an embedded newline used to end the record
    and start a second one written entirely by the sender, PRI included, which
    nothing reading the file afterwards could tell from a genuine entry.
    """

    def _run_and_read(self, payload: str) -> str:
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            with RunningServer(d, lambda port: write_config(d, port)) as srv:
                send_udp(payload, srv.port)
                time.sleep(0.3)

            # Explicit UTF-8: Windows would otherwise decode with the locale
            # code page and mangle the non-ASCII case.
            return (d / "syslog.log").read_text(encoding="utf-8")

    def test_embedded_newline_produces_one_line(self):
        forged = "<0>Mar 15 12:00:00 host sshd[1]: root login SUCCEEDED from 10.0.0.1"
        log = self._run_and_read("<14>Mar 15 12:00:00 host real: benign\n" + forged)

        self.assertEqual(log.count("\n"), 1)
        self.assertNotIn("\n" + forged, log)
        self.assertIn("benign\\n" + forged, log)

    def test_control_characters_are_escaped(self):
        log = self._run_and_read("<14>Mar 15 12:00:01 host app: \x1b[2J\x00\rdone")

        self.assertEqual(log.count("\n"), 1)
        self.assertNotIn("\x1b", log)
        self.assertNotIn("\x00", log)
        self.assertNotIn("\r", log)
        self.assertIn("\\x1B[2J\\x00\\rdone", log)

    def test_c1_controls_are_escaped(self):
        """U+009B is the 8-bit CSI and U+009D the 8-bit OSC: the same terminal
        control as ESC, reachable without an ESC byte in the datagram."""
        log = self._run_and_read("<14>Mar 15 12:00:03 host app: \u009b2J\u009d0;t")

        self.assertNotIn("\u009b", log)
        self.assertNotIn("\u009d", log)
        self.assertIn("\\u009B2J\\u009D0;t", log)

    def test_utf8_survives_escaping(self):
        log = self._run_and_read("<14>Mar 15 12:00:02 host app: 日本語 café")

        self.assertIn("日本語 café", log)


# ── Graceful shutdown ─────────────────────────────────────────────────────────


class TestGracefulShutdown(unittest.TestCase):
    def test_inflight_messages_complete_before_exit(self):
        """Messages received before SIGTERM must all appear in the output file."""
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            n_messages = 20
            with RunningServer(d, lambda port: write_config(d, port)) as srv:
                for i in range(n_messages):
                    send_udp(f"<34>Oct 11 22:14:15 mymachine su[1]: msg{i:04d}", srv.port)
                # Give the server enough time to receive and process everything
                # before signalling shutdown.
                time.sleep(0.3)

            self.assertEqual(srv.proc.returncode, 0)
            self.assertEqual(count_lines(d / "syslog.log"), n_messages)


# ── Forward-to-self (two binary instances) ───────────────────────────────────


class TestForwardToSelf(unittest.TestCase):
    def test_forward_a_to_b(self):
        """Spawn two instances: A forwards to B.
        After sending to A both output files must contain the message."""
        with (
            tempfile.TemporaryDirectory() as da_str,
            tempfile.TemporaryDirectory() as db_str,
        ):
            da, db = Path(da_str), Path(db_str)
            with (
                RunningServer(db, lambda port: write_config(db, port)) as srv_b,
                RunningServer(
                    da, lambda port: write_config(da, port, forward_to=srv_b.port)
                ) as srv_a,
            ):
                send_udp("<34>Oct 11 22:14:15 mymachine su[1]: forwarded binary", srv_a.port)
                # Extra time for the A→B forwarding leg.
                time.sleep(0.5)

            log_a = (da / "syslog.log").read_text()
            log_b = (db / "syslog.log").read_text()
            self.assertIn("forwarded binary", log_a)
            self.assertIn("forwarded binary", log_b)


# ── Multi-worker / concurrent senders ────────────────────────────────────────


class TestMultiWorker(unittest.TestCase):
    def test_concurrent_senders_no_corruption(self):
        """8 concurrent sender threads with workers=4 — no torn lines."""
        n_threads = 8
        n_per_thread = 50  # 400 total; fast enough for CI

        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            with RunningServer(d, lambda port: write_config(d, port, workers=4)) as srv:

                def sender(tid: int) -> None:
                    for i in range(n_per_thread):
                        send_udp(
                            f"<34>Oct 11 22:14:15 mymachine su[{tid}]: t{tid:02d}m{i:04d}",
                            srv.port,
                        )

                threads = [threading.Thread(target=sender, args=(t,)) for t in range(n_threads)]
                for t in threads:
                    t.start()
                for t in threads:
                    t.join()
                time.sleep(0.5)

            lines = (d / "syslog.log").read_text().splitlines()

            # Every line must end with the recognisable pattern tNNmNNNN.
            for line in lines:
                self.assertRegex(line, r"t\d{2}m\d{4}$", f"corrupted line: {line!r}")

            # On loopback UDP is reliable; allow a small margin for loaded CI.
            total = n_threads * n_per_thread
            self.assertGreaterEqual(len(lines), total * 9 // 10)


# ── Address validation ────────────────────────────────────────────────────────


class TestInvalidAddresses(unittest.TestCase):
    """Both host fields used to fail unreadably.

    An unparseable [forwarding] host aborted the process (SIGABRT, no message
    naming the key), and an unparseable [server] host exited non-zero with
    nothing on stderr at all. Both must be ordinary config errors now. Asserting
    on stderr covers it on every platform: an abort never gets that far.

    The two fields do not have the same rule. [server] host binds an interface
    and must be an IP literal; [forwarding] host names a destination, so a
    hostname is accepted and resolved, and one that does not resolve is reported
    and retried rather than being a startup failure.
    """

    def _run_with_config(self, d: Path, body: str) -> subprocess.CompletedProcess:
        conf = d / "minilog.conf"
        conf.write_text(body)
        return subprocess.run([BINARY, str(conf)], capture_output=True, text=True, timeout=10)

    @staticmethod
    def _forwarding_config(d: Path, port: int, host: str) -> str:
        return (
            "[server]\n"
            "host = 127.0.0.1\n"
            f"udp_port = {port}\n"
            "\n"
            "[output.main]\n"
            f"text_file = {d / 'syslog.log'}\n"
            "\n"
            "[forwarding]\n"
            "enabled = true\n"
            f"host = {host}\n"
            "port = 514\n"
        )

    def test_hostname_with_port_as_forwarding_host_is_a_config_error(self):
        # A name is fine; a name with the port appended is the mistake worth
        # being loud about, because as a name it would simply never resolve and
        # would be retried in the background forever.
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            r = self._run_with_config(
                d, self._forwarding_config(d, free_port(), "syslog.example.com:514")
            )

            self.assertNotEqual(r.returncode, 0)
            self.assertIn("[forwarding] host", r.stderr)
            self.assertIn("syslog.example.com:514", r.stderr)

    def test_unresolvable_forwarding_host_starts_and_reports(self):
        # .invalid never resolves (RFC 2606). minilog must still start: a
        # Windows auto-start service routinely runs before DNS does, and losing
        # the whole collector over a forwarding destination would be worse than
        # losing forwarding.
        #
        # The report is waited for in stderr. The lookup runs on the io_context
        # rather than blocking the constructor, so the report arrives some time
        # after the port is bound instead of before it — and reading stderr
        # once, after stopping the process, would be a race with however long
        # the resolver takes to answer.
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)

            def make_conf(port: int) -> Path:
                conf = d / "minilog.conf"
                conf.write_text(self._forwarding_config(d, port, "collector.invalid"))
                return conf

            with RunningServer(d, make_conf) as srv:
                send_udp("<14>hello", srv.port)
                time.sleep(0.3)
                self.assertIn("hello", (d / "syslog.log").read_text())

                # Generous, and free when it passes: the wait ends as soon
                # as the line appears. A resolver under load can take many
                # seconds to return NXDOMAIN for a name in a TLD it has to
                # ask upstream about, and that delay is now visible here
                # instead of in the time the server takes to bind.
                self.assertTrue(
                    wait_for_text(srv.stderr_path, "cannot resolve", timeout=60.0),
                    f"no resolve failure reported: {srv.stderr()!r}",
                )

            stderr = srv.stderr()
            self.assertEqual(srv.proc.returncode, 0, f"stderr: {stderr}")
            self.assertIn("collector.invalid", stderr)

    def test_resolvable_hostname_as_forwarding_host_is_accepted(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)

            def make_conf(port: int) -> Path:
                conf = d / "minilog.conf"
                conf.write_text(self._forwarding_config(d, port, "localhost"))
                return conf

            with RunningServer(d, make_conf) as srv:
                pass

            stderr = srv.stderr()
            self.assertEqual(srv.proc.returncode, 0, f"stderr: {stderr}")
            self.assertNotIn("cannot resolve", stderr)

    def test_hostname_as_server_host_is_a_config_error(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            r = self._run_with_config(
                d,
                "[server]\n"
                "host = localhost\n"
                f"udp_port = {free_port()}\n"
                "\n"
                "[output.main]\n"
                f"text_file = {d / 'syslog.log'}\n",
            )

            self.assertNotEqual(r.returncode, 0)
            self.assertIn("[server] host", r.stderr)
            self.assertIn("localhost", r.stderr)

    def test_bind_failure_is_still_reported(self):
        """The bind error moved from start() to its caller; it must still appear."""
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as blocker:
                blocker.bind(("127.0.0.1", 0))
                busy_port = blocker.getsockname()[1]
                r = self._run_with_config(
                    d,
                    "[server]\n"
                    "host = 127.0.0.1\n"
                    f"udp_port = {busy_port}\n"
                    "\n"
                    "[output.main]\n"
                    f"text_file = {d / 'syslog.log'}\n",
                )

            self.assertNotEqual(r.returncode, 0)
            self.assertIn(str(busy_port), r.stderr)
            # Reported by runServer now, so exactly once.
            self.assertEqual(r.stderr.count("failed to bind"), 1)


# ── Startup validation of sink paths ──────────────────────────────────────────


class TestSinkPathsCheckedAtStartup(unittest.TestCase):
    """Sinks are opened before the service reports itself running.

    They used to open lazily on the first message, so an unwritable log path let
    `net start` succeed and the SCM see a healthy service; the sink then died on
    the first datagram with no non-zero exit code and no recovery action.
    """

    def test_unwritable_log_path_fails_the_start(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            conf = d / "minilog.conf"
            missing = d / "no-such-dir" / "syslog.log"
            conf.write_text(
                "[server]\n"
                "host = 127.0.0.1\n"
                f"udp_port = {free_port()}\n"
                "\n"
                "[output.main]\n"
                f"text_file = {missing}\n"
            )

            r = subprocess.run([BINARY, str(conf)], capture_output=True, text=True, timeout=10)

            self.assertNotEqual(r.returncode, 0, "server started with an unusable sink path")
            self.assertIn("failed to open", r.stderr)
            self.assertIn(str(missing), r.stderr)
            # A sink that fails at startup is not retried -- the process exits
            # before anything could -- so the message must not promise one.
            self.assertNotIn("retrying", r.stderr)

    def test_usable_log_path_still_starts(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            with RunningServer(d, lambda port: write_config(d, port)) as srv:
                # Opened eagerly now, so the file exists before any message.
                self.assertTrue((d / "syslog.log").exists())

            self.assertEqual(srv.proc.returncode, 0)


# ── Filesystem failure handling ───────────────────────────────────────────────


@unittest.skipUnless(CAN_DENY_ACCESS, "needs enforced POSIX permission bits")
class TestFilesystemFailure(unittest.TestCase):
    def test_denied_log_directory_does_not_abort_the_server(self):
        """A rotation that hits a permission error must close that sink and leave
        the process running.

        This used to take the whole server down: the throwing std::filesystem
        overload escaped the strand handler, escaped io_context::run() on a bare
        worker thread, and became std::terminate — SIGABRT, exit 134.
        """
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            log_dir = d / "logs"
            log_dir.mkdir()
            filler = "x" * 60
            with RunningServer(
                d, lambda port: write_config(d, port, max_size="200B", log_dir=log_dir)
            ) as srv:
                # Push past max_size so that the next message triggers a rotation.
                for i in range(10):
                    send_udp(f"<34>Oct 11 22:14:15 host su[1]: before {i} {filler}", srv.port)
                time.sleep(0.3)

                log_dir.chmod(0o000)
                try:
                    for i in range(10):
                        send_udp(f"<34>Oct 11 22:14:15 host su[1]: after {i} {filler}", srv.port)
                    time.sleep(0.5)
                    self.assertIsNone(
                        srv.proc.poll(),
                        f"server died on a denied log directory (exit {srv.proc.returncode})",
                    )
                finally:
                    log_dir.chmod(0o755)

                # Still healthy once access is restored — the sink is out of
                # service, but the process is not.
                self.assertIsNone(srv.proc.poll())

            self.assertEqual(srv.proc.returncode, 0, "server did not shut down cleanly")


# ── Compressed rotation ───────────────────────────────────────────────────────


class TestCompressedRotation(unittest.TestCase):
    def test_older_generations_are_gzipped_and_hold_every_message(self):
        """With compress = true, .1 stays plain and .2 and older become .gz;
        together they still hold every message sent, in order."""
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            sent = [
                f"<34>Oct 11 22:14:15 host su[1]: message {i:03d} " + "x" * 40 for i in range(60)
            ]
            with RunningServer(
                d, lambda port: write_config(d, port, max_size="500B", compress=True)
            ) as srv:
                for msg in sent:
                    send_udp(msg, srv.port)
                    time.sleep(0.005)  # keep them in order on the way in
                # Compression runs in the background after each rotation.
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline and not (d / "syslog.3.log.gz").exists():
                    time.sleep(0.05)
                time.sleep(0.5)
            # Where a failed rotation or compression is reported.
            stderr = srv.stderr()
            self.assertEqual(srv.proc.returncode, 0, stderr)

            self.assertTrue((d / "syslog.1.log").exists())
            self.assertFalse((d / "syslog.1.log.gz").exists())
            self.assertFalse((d / "syslog.2.log").exists())
            self.assertFalse(list(d.glob("*.tmp")))

            generations = sorted((int(p.name.split(".")[1]), p) for p in d.glob("syslog.*.log*"))
            self.assertGreaterEqual(len(generations), 3)
            text = ""
            for _, p in reversed(generations):  # oldest first
                data = gzip.decompress(p.read_bytes()) if p.suffix == ".gz" else p.read_bytes()
                text += data.decode()
            text += (d / "syslog.log").read_text()
            self.assertEqual(text.splitlines(), sent, f"server stderr:\n{stderr}")

    def test_a_restart_carries_on_with_the_same_chain(self):
        """Stopped and started again on the same files, minilog keeps shifting the
        compressed generations it already wrote; nothing is lost or repeated."""
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            sent = []
            stderr = ""
            for run in range(2):
                with RunningServer(
                    d, lambda port: write_config(d, port, max_size="500B", compress=True)
                ) as srv:
                    for i in range(30):
                        msg = (
                            f"<34>Oct 11 22:14:15 host su[1]: run {run} message {i:02d} " + "y" * 40
                        )
                        send_udp(msg, srv.port)
                        sent.append(msg)
                        time.sleep(0.005)
                    time.sleep(1.0)
                stderr += srv.stderr()
                self.assertEqual(srv.proc.returncode, 0, stderr)

            generations = sorted((int(p.name.split(".")[1]), p) for p in d.glob("syslog.*.log*"))
            text = ""
            for _, p in reversed(generations):
                data = gzip.decompress(p.read_bytes()) if p.suffix == ".gz" else p.read_bytes()
                text += data.decode()
            text += (d / "syslog.log").read_text()
            self.assertEqual(text.splitlines(), sent, f"server stderr:\n{stderr}")
            self.assertFalse(list(d.glob("*.tmp")))


# ── Entry point ───────────────────────────────────────────────────────────────

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <path-to-minilog-binary>", file=sys.stderr)
        sys.exit(1)
    BINARY = sys.argv.pop(1)  # consume before unittest sees argv
    unittest.main()
