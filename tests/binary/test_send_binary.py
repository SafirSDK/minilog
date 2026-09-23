#!/usr/bin/env python3
"""
Binary-level tests for minilog-send.

The formatting and argument parsing are covered by tests/send/test_send.cpp;
what only the real executable can show is that a datagram it sends lands in a
running minilog with the fields the flags asked for, that stdin mode sends one
message per line, and that the exit codes and stderr are what --help promises.

Invoked by CTest as:
    python3 test_send_binary.py <path-to-minilog> <path-to-minilog-send>
"""

import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

# The server-side helpers (process flags, shutdown) live beside this file.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_binary import _POPEN_FLAGS, terminate  # noqa: E402

SERVER: str = ""  # set from argv before test discovery
SEND: str = ""

EXIT_SEND_FAILED = 1
EXIT_USAGE = 2


# ── helpers ──────────────────────────────────────────────────────────────────


def family(host: str) -> int:
    return socket.AF_INET6 if ":" in host else socket.AF_INET


def free_port(host: str) -> int:
    """Return an ephemeral UDP port that is currently unused on *host*."""
    with socket.socket(family(host), socket.SOCK_DGRAM) as s:
        s.bind((host, 0))
        return s.getsockname()[1]


def wait_for_listener(host: str, port: int, timeout: float = 5.0) -> bool:
    """Block until a process has bound UDP *host*:*port* (server is ready).

    test_binary.wait_for_port does the same for 127.0.0.1 only; the server
    here may be on ::1, see test_localhost_by_name.
    """
    deadline = time.monotonic() + timeout
    # On Windows, SO_EXCLUSIVEADDRUSE creates a race: the probe socket can
    # briefly hold the port between the server's open() and bind() calls.  A
    # short initial wait lets the server complete its bind before probing.
    if sys.platform == "win32":
        time.sleep(0.3)
    while time.monotonic() < deadline:
        with socket.socket(family(host), socket.SOCK_DGRAM) as probe:
            try:
                probe.bind((host, port))
            except OSError:
                return True  # Cannot bind — server owns it.
        time.sleep(0.02)
    return False


def first_resolved(name: str) -> str:
    """The address minilog-send will send to for *name*.

    Asio's resolver is getaddrinfo with AI_ADDRCONFIG, and the tool takes the
    first result; this is the same call, so the test can put the server where
    the datagram is going to go.  On a dual-stack machine that is ::1 for
    "localhost", not 127.0.0.1.
    """
    infos = socket.getaddrinfo(name, None, type=socket.SOCK_DGRAM, flags=socket.AI_ADDRCONFIG)
    return infos[0][4][0]


def write_config(d: Path, host: str, port: int) -> Path:
    conf = d / "minilog.conf"
    conf.write_text(
        "\n".join(
            [
                "[server]",
                f"host = {host}",
                f"udp_port = {port}",
                "workers = 1",
                "",
                "[output.main]",
                f"text_file = {d / 'syslog.log'}",
                f"jsonl_file = {d / 'syslog.jsonl'}",
                "max_size = 100MB",
                "max_files = 0",
                "facility = *",
                "include_malformed = true",
            ]
        )
    )
    return conf


def wait_for_records(path: Path, count: int, timeout: float = 10.0) -> list[dict]:
    """Block until *count* JSONL records are in *path*, and return them."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            lines = [ln for ln in path.read_text(encoding="utf-8").splitlines() if ln]
            if len(lines) >= count:
                return [json.loads(ln) for ln in lines]
        time.sleep(0.02)
    raise AssertionError(f"{count} record(s) did not arrive in {path} within {timeout}s")


def run_send(*args: str, stdin: str | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(
        [SEND, *args],
        input=stdin,
        capture_output=True,
        text=True,
        timeout=15,
    )


class ServerFixture:
    """A minilog listening on an ephemeral port, writing JSONL into a tempdir."""

    def __init__(self, host: str = "127.0.0.1"):
        self.host = host

    def __enter__(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)
        self.port = free_port(self.host)
        self.jsonl = self.dir / "syslog.jsonl"
        conf = write_config(self.dir, self.host, self.port)
        self.proc = subprocess.Popen([SERVER, str(conf)], **_POPEN_FLAGS)
        if not wait_for_listener(self.host, self.port):
            self.proc.kill()
            raise AssertionError("server did not start in time")
        return self

    def __exit__(self, *exc):
        terminate(self.proc)
        self.proc.wait(timeout=10)
        self._tmp.cleanup()
        return False

    def send(self, *args: str, stdin: str | None = None) -> subprocess.CompletedProcess:
        return run_send("--port", str(self.port), *args, stdin=stdin)


# ── no server needed ─────────────────────────────────────────────────────────


class TestCommandLine(unittest.TestCase):
    def test_help_exits_zero_and_documents_the_udp_caveat(self):
        r = run_send("--help")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("Usage: minilog-send", r.stdout)
        self.assertIn("--rfc3164", r.stdout)
        self.assertIn("does not mean the message arrived", r.stdout)
        self.assertEqual(r.stderr, "")

    def test_version_exits_zero(self):
        r = run_send("--version")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertRegex(r.stdout, r"^minilog-send \d+\.\d+\.\d+")

    def test_unknown_option_is_a_usage_error(self):
        r = run_send("--tcp", "hello")
        self.assertEqual(r.returncode, EXIT_USAGE)
        self.assertIn("tcp", r.stderr)
        self.assertIn("--help", r.stderr)
        self.assertEqual(r.stdout, "")

    def test_unknown_facility_is_a_usage_error(self):
        r = run_send("--facility", "kitchen", "hello")
        self.assertEqual(r.returncode, EXIT_USAGE)
        self.assertIn("kitchen", r.stderr)

    def test_unknown_severity_is_a_usage_error(self):
        r = run_send("--severity", "loud", "hello")
        self.assertEqual(r.returncode, EXIT_USAGE)
        self.assertIn("loud", r.stderr)

    def test_bad_port_is_a_usage_error(self):
        for port in ("0", "65536", "syslog"):
            with self.subTest(port=port):
                r = run_send("--port", port, "hello")
                self.assertEqual(r.returncode, EXIT_USAGE)
                self.assertIn("port", r.stderr)

    def test_msgid_with_rfc3164_is_a_usage_error(self):
        r = run_send("--rfc3164", "--msgid", "X", "hello")
        self.assertEqual(r.returncode, EXIT_USAGE)
        self.assertIn("msgid", r.stderr)

    def test_space_in_app_is_a_usage_error(self):
        r = run_send("--app", "two words", "hello")
        self.assertEqual(r.returncode, EXIT_USAGE)
        self.assertIn("app", r.stderr)

    @unittest.skipIf(
        sys.platform == "win32",
        "a Windows command line is capped at 32767 characters, below the datagram "
        "limit, so an oversize argv message cannot exist there; the stdin path is "
        "covered by test_stdin_stops_at_the_first_line_that_cannot_be_sent",
    )
    def test_oversize_message_is_refused_before_anything_is_sent(self):
        # Port 1 is nothing anybody listens on; the point is that the size check
        # happens first, so no resolution or socket is involved in the failure.
        r = run_send("--port", "1", "x" * 70000)
        self.assertEqual(r.returncode, EXIT_USAGE)
        self.assertIn("65507", r.stderr)

    def test_no_message_and_empty_stdin_is_a_usage_error(self):
        r = run_send("--port", "1", stdin="\n\n")
        self.assertEqual(r.returncode, EXIT_USAGE)
        self.assertIn("nothing on stdin", r.stderr)

    def test_unresolvable_host_is_a_send_failure(self):
        # .invalid is reserved (RFC 2606) never to resolve.
        r = run_send("--host", "no-such-host.invalid", "hello")
        self.assertEqual(r.returncode, EXIT_SEND_FAILED)
        self.assertIn("no-such-host.invalid", r.stderr)
        self.assertEqual(r.stdout, "")


# ── against a running minilog ────────────────────────────────────────────────


class TestAgainstServer(unittest.TestCase):
    def test_defaults_land_as_rfc5424_from_this_machine_and_process(self):
        with ServerFixture() as srv:
            proc = subprocess.Popen(
                [SEND, "--port", str(srv.port), "hello", "from", "the", "cli"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            out, err = proc.communicate(timeout=15)
            self.assertEqual(proc.returncode, 0, err)
            self.assertEqual(out, "")
            self.assertEqual(err, "")

            (rec,) = wait_for_records(srv.jsonl, 1)

        self.assertEqual(rec["proto"], "RFC5424")
        self.assertEqual(rec["facility"], "user")
        self.assertEqual(rec["severity"], "INFO")
        self.assertEqual(rec["app"], "minilog-send")
        self.assertEqual(rec["hostname"], socket.gethostname())
        self.assertEqual(rec["pid"], str(proc.pid))
        self.assertIsNone(rec["msgid"])
        self.assertEqual(rec["src"], "127.0.0.1")
        # Structured data is kept as a prefix of the message; "-" means none.
        self.assertEqual(rec["message"], "- hello from the cli")
        # A real timestamp with a zone offset, not a placeholder.
        self.assertRegex(rec["msg_time"], r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{6}[+-]\d\d:\d\d$")

    def test_every_flag_shows_up_in_the_record(self):
        with ServerFixture() as srv:
            r = srv.send(
                "--facility", "local3",
                "--severity", "error",
                "--app", "deploy",
                "--hostname", "web01",
                "--pid", "77",
                "--msgid", "STEP3",
                "release 1.4 rolled out",
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            (rec,) = wait_for_records(srv.jsonl, 1)

        self.assertEqual(rec["proto"], "RFC5424")
        self.assertEqual(rec["facility"], "local3")
        self.assertEqual(rec["severity"], "ERROR")
        self.assertEqual(rec["app"], "deploy")
        self.assertEqual(rec["hostname"], "web01")
        self.assertEqual(rec["pid"], "77")
        self.assertEqual(rec["msgid"], "STEP3")
        self.assertEqual(rec["message"], "- release 1.4 rolled out")

    def test_numeric_facility_and_severity(self):
        with ServerFixture() as srv:
            r = srv.send("-f", "4", "-s", "2", "numbers")
            self.assertEqual(r.returncode, 0, r.stderr)
            (rec,) = wait_for_records(srv.jsonl, 1)
        self.assertEqual(rec["facility"], "auth")
        self.assertEqual(rec["severity"], "CRITICAL")

    def test_rfc3164(self):
        with ServerFixture() as srv:
            r = srv.send(
                "--rfc3164",
                "-f", "daemon",
                "-s", "warning",
                "-a", "backup",
                "--hostname", "nas",
                "--pid", "4242",
                "nightly backup finished",
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            (rec,) = wait_for_records(srv.jsonl, 1)

        self.assertEqual(rec["proto"], "RFC3164")
        self.assertEqual(rec["facility"], "daemon")
        self.assertEqual(rec["severity"], "WARNING")
        self.assertEqual(rec["app"], "backup")
        self.assertEqual(rec["hostname"], "nas")
        self.assertEqual(rec["pid"], "4242")
        self.assertIsNone(rec["msgid"])
        self.assertEqual(rec["message"], "nightly backup finished")
        self.assertRegex(rec["msg_time"], r"^[A-Z][a-z]{2} [ \d]\d \d\d:\d\d:\d\d$")

    def test_stdin_sends_one_message_per_line_in_order(self):
        with ServerFixture() as srv:
            r = srv.send("-s", "notice", stdin="first line\r\n\nsecond line\nthird, no newline")
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout, "")
            recs = wait_for_records(srv.jsonl, 3)
            # Give a stray fourth record (the empty line) time to show up if the
            # tool were sending it.
            time.sleep(0.3)
            recs = wait_for_records(srv.jsonl, 3)

        self.assertEqual(len(recs), 3)
        self.assertEqual(
            [rec["message"] for rec in recs],
            ["- first line", "- second line", "- third, no newline"],
        )
        self.assertTrue(all(rec["severity"] == "NOTICE" for rec in recs))

    def test_stdin_stops_at_the_first_line_that_cannot_be_sent(self):
        with ServerFixture() as srv:
            r = srv.send(stdin="ok one\n" + "x" * 70000 + "\nnever sent\n")
            self.assertEqual(r.returncode, EXIT_SEND_FAILED)
            self.assertIn("line 2", r.stderr)
            self.assertIn("1 of 3 sent", r.stderr)
            (rec,) = wait_for_records(srv.jsonl, 1)
            time.sleep(0.3)
            recs = wait_for_records(srv.jsonl, 1)

        self.assertEqual(len(recs), 1)
        self.assertEqual(rec["message"], "- ok one")

    def test_message_starting_with_a_dash_after_double_dash(self):
        with ServerFixture() as srv:
            r = srv.send("--", "-v", "--looks-like-a-flag")
            self.assertEqual(r.returncode, 0, r.stderr)
            (rec,) = wait_for_records(srv.jsonl, 1)
        self.assertEqual(rec["message"], "- -v --looks-like-a-flag")

    def test_localhost_by_name(self):
        """A name goes to the first address it resolves to, so the server is
        bound there -- ::1 on a dual-stack machine, where a server on 127.0.0.1
        would never see the datagram.  The README says as much."""
        with ServerFixture(host=first_resolved("localhost")) as srv:
            r = srv.send("--host", "localhost", "by name")
            self.assertEqual(r.returncode, 0, r.stderr)
            (rec,) = wait_for_records(srv.jsonl, 1)
        self.assertEqual(rec["message"], "- by name")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <path-to-minilog> <path-to-minilog-send>", file=sys.stderr)
        sys.exit(1)
    SERVER = sys.argv.pop(1)  # consume before unittest sees argv
    SEND = sys.argv.pop(1)
    unittest.main()
