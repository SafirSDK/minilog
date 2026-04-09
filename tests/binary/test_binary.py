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
) -> Path:
    log_file = d / "syslog.log"
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


# ── Entry point ───────────────────────────────────────────────────────────────

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <path-to-minilog-binary>", file=sys.stderr)
        sys.exit(1)
    BINARY = sys.argv.pop(1)  # consume before unittest sees argv
    unittest.main()
