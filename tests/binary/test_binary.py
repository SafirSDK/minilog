#!/usr/bin/env python3
"""
Binary-level tests for minilog.

Invoked by CTest as:
    python3 test_binary.py <path-to-minilog-binary>

Or directly for development:
    python3 tests/test_binary.py ./build/linux-debug/minilog
"""

import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

BINARY: str = ""  # set from argv[1] before test discovery

# On Windows, processes must be in their own process group so that
# CTRL_C_EVENT can be delivered without also interrupting the test runner.
_POPEN_FLAGS: dict = (
    {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP}
    if sys.platform == "win32"
    else {}
)


# ── helpers ──────────────────────────────────────────────────────────────────


def free_port() -> int:
    """Return an ephemeral port that is currently unused."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_for_port(port: int, timeout: float = 5.0) -> bool:
    """Block until a process has bound to *port* (server is ready)."""
    deadline = time.monotonic() + timeout
    # On Windows, SO_EXCLUSIVEADDRUSE creates a race: the probe socket can
    # briefly hold the port between the server's open() and bind() calls,
    # causing the server's bind() to fail with WSAEADDRINUSE.  A short
    # initial wait lets the server complete its bind before we start probing.
    if sys.platform == "win32":
        time.sleep(0.3)
    while time.monotonic() < deadline:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            try:
                probe.bind(("127.0.0.1", port))
                # Could bind — server not yet up; release and retry.
            except OSError:
                return True  # Cannot bind — server owns it.
        time.sleep(0.02)
    return False


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
) -> Path:
    log_file = (log_dir or d) / "syslog.log"
    conf = d / "minilog.conf"
    lines = [
        "[server]",
        "host = 127.0.0.1",
        f"udp_port = {port}",
        "encoding = utf-8",
        f"workers = {workers}",
        "",
        "[output.main]",
        f"text_file = {log_file}",
        f"max_size = {max_size}",
        "max_files = 0",
        "facility = *",
        "include_malformed = true",
    ]
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


# ── Basic smoke test ──────────────────────────────────────────────────────────


class TestSmoke(unittest.TestCase):
    def test_receive_and_write(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            port = free_port()
            conf = write_config(d, port)

            proc = subprocess.Popen([BINARY, str(conf)], **_POPEN_FLAGS)
            try:
                self.assertTrue(wait_for_port(port), "server did not start in time")
                send_udp("<34>Oct 11 22:14:15 mymachine su[1]: smoke test", port)
                time.sleep(0.3)
            finally:
                terminate(proc)
                proc.wait(timeout=10)

            self.assertEqual(proc.returncode, 0)
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
            port = free_port()
            conf = write_config(d, port)

            proc = subprocess.Popen([BINARY, str(conf)], **_POPEN_FLAGS)
            try:
                self.assertTrue(wait_for_port(port))
                send_udp("<34>Oct 11 22:14:15 mymachine su[1]: from3164", port)
                send_udp("<34>1 2026-03-12T14:30:22Z host app 1 - - from5424", port)
                time.sleep(0.3)
            finally:
                terminate(proc)
                proc.wait(timeout=10)

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
            port = free_port()
            conf = write_config(d, port)

            proc = subprocess.Popen([BINARY, str(conf)], **_POPEN_FLAGS)
            try:
                self.assertTrue(wait_for_port(port), "server did not start in time")
                send_udp(payload, port)
                time.sleep(0.3)
            finally:
                terminate(proc)
                proc.wait(timeout=10)

            return (d / "syslog.log").read_text()

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

    def test_utf8_survives_escaping(self):
        log = self._run_and_read("<14>Mar 15 12:00:02 host app: 日本語 café")

        self.assertIn("日本語 café", log)


# ── Graceful shutdown ─────────────────────────────────────────────────────────


class TestGracefulShutdown(unittest.TestCase):
    def test_inflight_messages_complete_before_exit(self):
        """Messages received before SIGTERM must all appear in the output file."""
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            port = free_port()
            conf = write_config(d, port)

            proc = subprocess.Popen([BINARY, str(conf)], **_POPEN_FLAGS)
            try:
                self.assertTrue(wait_for_port(port))
                n_messages = 20
                for i in range(n_messages):
                    send_udp(f"<34>Oct 11 22:14:15 mymachine su[1]: msg{i:04d}", port)
                # Give the server enough time to receive and process everything
                # before signalling shutdown.
                time.sleep(0.3)
            finally:
                terminate(proc)
                proc.wait(timeout=10)

            self.assertEqual(proc.returncode, 0)
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
            port_b = free_port()
            port_a = free_port()
            conf_b = write_config(db, port_b)
            conf_a = write_config(da, port_a, forward_to=port_b)

            proc_b = subprocess.Popen([BINARY, str(conf_b)], **_POPEN_FLAGS)
            proc_a = subprocess.Popen([BINARY, str(conf_a)], **_POPEN_FLAGS)
            try:
                self.assertTrue(wait_for_port(port_b), "server B did not start")
                self.assertTrue(wait_for_port(port_a), "server A did not start")
                send_udp("<34>Oct 11 22:14:15 mymachine su[1]: forwarded binary", port_a)
                # Extra time for the A→B forwarding leg.
                time.sleep(0.5)
            finally:
                terminate(proc_a)
                proc_a.wait(timeout=10)
                terminate(proc_b)
                proc_b.wait(timeout=10)

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
            port = free_port()
            conf = write_config(d, port, workers=4)

            proc = subprocess.Popen([BINARY, str(conf)], **_POPEN_FLAGS)
            try:
                self.assertTrue(wait_for_port(port))

                def sender(tid: int) -> None:
                    for i in range(n_per_thread):
                        send_udp(
                            f"<34>Oct 11 22:14:15 mymachine su[{tid}]: t{tid:02d}m{i:04d}",
                            port,
                        )

                threads = [
                    threading.Thread(target=sender, args=(t,)) for t in range(n_threads)
                ]
                for t in threads:
                    t.start()
                for t in threads:
                    t.join()
                time.sleep(0.5)
            finally:
                terminate(proc)
                proc.wait(timeout=10)

            lines = (d / "syslog.log").read_text().splitlines()

            # Every line must end with the recognisable pattern tNNmNNNN.
            for line in lines:
                self.assertRegex(line, r"t\d{2}m\d{4}$", f"corrupted line: {line!r}")

            # On loopback UDP is reliable; allow a small margin for loaded CI.
            total = n_threads * n_per_thread
            self.assertGreaterEqual(len(lines), total * 9 // 10)


# ── Address validation ────────────────────────────────────────────────────────


class TestInvalidAddresses(unittest.TestCase):
    """Neither host field is resolvable, and both used to fail unreadably.

    An unparseable [forwarding] host aborted the process (SIGABRT, no message
    naming the key), and an unparseable [server] host exited non-zero with
    nothing on stderr at all. Both must now be ordinary config errors. Asserting
    on stderr covers it on every platform: an abort never gets that far.
    """

    def _run_with_config(self, d: Path, body: str) -> subprocess.CompletedProcess:
        conf = d / "minilog.conf"
        conf.write_text(body)
        return subprocess.run(
            [BINARY, str(conf)], capture_output=True, text=True, timeout=10
        )

    def test_hostname_as_forwarding_host_is_a_config_error(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            r = self._run_with_config(
                d,
                "[server]\n"
                "host = 127.0.0.1\n"
                f"udp_port = {free_port()}\n"
                "\n"
                "[output.main]\n"
                f"text_file = {d / 'syslog.log'}\n"
                "\n"
                "[forwarding]\n"
                "enabled = true\n"
                "host = syslog.example.com\n"
                "port = 514\n",
            )

            self.assertNotEqual(r.returncode, 0)
            self.assertIn("[forwarding] host", r.stderr)
            self.assertIn("syslog.example.com", r.stderr)

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

            r = subprocess.run(
                [BINARY, str(conf)], capture_output=True, text=True, timeout=10
            )

            self.assertNotEqual(r.returncode, 0, "server started with an unusable sink path")
            self.assertIn("failed to open", r.stderr)
            self.assertIn(str(missing), r.stderr)

    def test_usable_log_path_still_starts(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            port = free_port()
            conf = write_config(d, port)

            proc = subprocess.Popen([BINARY, str(conf)], **_POPEN_FLAGS)
            try:
                self.assertTrue(wait_for_port(port), "server did not start in time")
                # Opened eagerly now, so the file exists before any message.
                self.assertTrue((d / "syslog.log").exists())
            finally:
                terminate(proc)
                proc.wait(timeout=10)

            self.assertEqual(proc.returncode, 0)


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
            port = free_port()
            conf = write_config(d, port, max_size="200B", log_dir=log_dir)

            filler = "x" * 60
            proc = subprocess.Popen([BINARY, str(conf)], **_POPEN_FLAGS)
            try:
                self.assertTrue(wait_for_port(port), "server did not start in time")

                # Push past max_size so that the next message triggers a rotation.
                for i in range(10):
                    send_udp(f"<34>Oct 11 22:14:15 host su[1]: before {i} {filler}", port)
                time.sleep(0.3)

                log_dir.chmod(0o000)
                try:
                    for i in range(10):
                        send_udp(f"<34>Oct 11 22:14:15 host su[1]: after {i} {filler}", port)
                    time.sleep(0.5)
                    self.assertIsNone(
                        proc.poll(),
                        f"server died on a denied log directory (exit {proc.returncode})",
                    )
                finally:
                    log_dir.chmod(0o755)

                # Still healthy once access is restored — the sink is out of
                # service, but the process is not.
                self.assertIsNone(proc.poll())
            finally:
                terminate(proc)
                proc.wait(timeout=10)

            self.assertEqual(proc.returncode, 0, "server did not shut down cleanly")


# ── Entry point ───────────────────────────────────────────────────────────────

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <path-to-minilog-binary>", file=sys.stderr)
        sys.exit(1)
    BINARY = sys.argv.pop(1)  # consume before unittest sees argv
    unittest.main()
