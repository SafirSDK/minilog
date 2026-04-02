#!/usr/bin/env python3
"""
Tests for minilog-cli-viewer.py

Invoked by CTest as:
    python3 test_viewer.py <path-to-minilog-cli-viewer.py>

Or directly for development:
    python3 tests/test_viewer.py ./minilog-cli-viewer.py
"""

import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

# Set VIEWER from command line argument if provided
VIEWER: str = ""
if len(sys.argv) >= 2 and not sys.argv[1].startswith("-"):
    # Check if second arg looks like a path (not a unittest flag)
    potential_viewer = sys.argv[1]
    if potential_viewer.endswith(".py") or "/" in potential_viewer or "\\" in potential_viewer:
        VIEWER = str(Path(sys.argv.pop(1)).absolute())


# ── Helpers ──────────────────────────────────────────────────────────────────


def write_server_config(config_dir: Path, jsonl_path: Path) -> Path:
    """Create a minimal minilog.conf pointing to a JSONL file"""
    conf = config_dir / "minilog.conf"
    conf.write_text(
        f"""[server]
host = 127.0.0.1
udp_port = 514

[output.main]
jsonl_file = {jsonl_path}
max_size = 100MB
"""
    )
    return conf


def write_viewer_config(
    config_dir: Path,
    columns: str = "rcv, facility, severity, message",
    exclude: str = "",
    include: str = "",
) -> Path:
    """Create a minilog-cli-viewer.conf"""
    conf = config_dir / "minilog-cli-viewer.conf"
    conf.write_text(
        f"""[viewer]
columns = {columns}
timestamp_format = short
use_colors = false

[filters]
exclude = {exclude}
include = {include}
"""
    )
    return conf


def write_jsonl_record(
    f,
    message: str,
    facility: str = "daemon",
    severity: str = "INFO",
    hostname: str = "testhost",
    app: str = "testapp",
):
    """Write a single JSONL record to a file"""
    record = {
        "rcv": "2026-03-29T12:00:00Z",
        "src": "192.168.1.1",
        "proto": "RFC3164",
        "facility": facility,
        "severity": severity,
        "hostname": hostname,
        "app": app,
        "pid": "123",
        "msgid": None,
        "message": message,
    }
    f.write(json.dumps(record) + "\n")
    f.flush()


# ── Config discovery tests ───────────────────────────────────────────────────


class TestConfigDiscovery(unittest.TestCase):
    def test_finds_server_config_in_current_dir(self):
        """Viewer should find ./minilog.conf"""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir = Path(tmpdir)
            jsonl = tmpdir / "test.jsonl"
            jsonl.touch()

            write_server_config(tmpdir, jsonl)

            # Run viewer from tmpdir so it finds ./minilog.conf
            proc = subprocess.Popen(
                [sys.executable, VIEWER, "--no-color", "--verbose"],
                cwd=tmpdir,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            time.sleep(0.5)
            proc.terminate()
            proc.wait(timeout=5)

            stderr = proc.stderr.read()
            self.assertIn("Found server config:", stderr)
            self.assertIn("minilog.conf", stderr)

    def test_error_when_no_server_config_found(self):
        """Viewer should exit with error if minilog.conf not found"""
        with tempfile.TemporaryDirectory() as tmpdir:
            result = subprocess.run(
                [sys.executable, VIEWER],
                cwd=tmpdir,
                capture_output=True,
                text=True,
                timeout=5,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("minilog.conf not found", result.stderr)

    def test_finds_viewer_config_in_same_dir_as_server_config(self):
        """Viewer should find minilog-cli-viewer.conf next to minilog.conf"""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir = Path(tmpdir)
            jsonl = tmpdir / "test.jsonl"
            jsonl.touch()

            write_server_config(tmpdir, jsonl)
            write_viewer_config(tmpdir, columns="message")

            proc = subprocess.Popen(
                [sys.executable, VIEWER, "--no-color", "--verbose"],
                cwd=tmpdir,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            time.sleep(0.5)
            proc.terminate()
            proc.wait(timeout=5)

            stderr = proc.stderr.read()
            self.assertIn("Found viewer config:", stderr)
            self.assertIn("minilog-cli-viewer.conf", stderr)


# ── Output section tests ─────────────────────────────────────────────────────


class TestOutputSection(unittest.TestCase):
    def test_default_main_section(self):
        """Viewer should read [output.main] by default"""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir = Path(tmpdir)
            jsonl = tmpdir / "test.jsonl"
            jsonl.touch()

            write_server_config(tmpdir, jsonl)

            proc = subprocess.Popen(
                [sys.executable, VIEWER, "--no-color", "--verbose"],
                cwd=tmpdir,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            time.sleep(0.5)
            proc.terminate()
            proc.wait(timeout=5)

            stderr = proc.stderr.read()
            self.assertIn("Reading", stderr)
            self.assertIn(str(jsonl), stderr)

    def test_custom_output_section(self):
        """Viewer should support --output-section flag"""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir = Path(tmpdir)
            jsonl = tmpdir / "auth.jsonl"
            jsonl.touch()

            conf = tmpdir / "minilog.conf"
            conf.write_text(
                f"""[server]
host = 127.0.0.1

[output.auth]
jsonl_file = {jsonl}
"""
            )

            proc = subprocess.Popen(
                [sys.executable, VIEWER, "--output-section", "auth", "--no-color", "--verbose"],
                cwd=tmpdir,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            time.sleep(0.5)
            proc.terminate()
            proc.wait(timeout=5)

            stderr = proc.stderr.read()
            self.assertIn("Reading", stderr)
            self.assertIn("auth.jsonl", stderr)

    def test_error_on_missing_output_section(self):
        """Viewer should error if specified output section doesn't exist"""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir = Path(tmpdir)
            jsonl = tmpdir / "test.jsonl"
            jsonl.touch()

            write_server_config(tmpdir, jsonl)

            result = subprocess.run(
                [sys.executable, VIEWER, "--output-section", "nonexistent"],
                cwd=tmpdir,
                capture_output=True,
                text=True,
                timeout=5,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("not found", result.stderr)


# ── Filtering tests ──────────────────────────────────────────────────────────


class TestFiltering(unittest.TestCase):
    def test_exclude_pattern_filters_messages(self):
        """Messages matching --exclude pattern should not appear"""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir = Path(tmpdir)
            jsonl = tmpdir / "test.jsonl"
            jsonl.touch()  # Create empty file first

            write_server_config(tmpdir, jsonl)

            # Start viewer with exclude pattern
            proc = subprocess.Popen(
                [sys.executable, VIEWER, "--exclude", "skip_this", "--no-color", "--verbose"],
                cwd=tmpdir,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

            # Give viewer time to start tailing
            time.sleep(0.5)

            # Write test messages
            with open(jsonl, "a") as f:  # Append mode
                write_jsonl_record(f, "this should appear")
                write_jsonl_record(f, "skip_this message")
                write_jsonl_record(f, "another visible message")

            time.sleep(0.5)
            proc.terminate()
            stdout, stderr = proc.communicate(timeout=5)

            # Debug output
            if not stdout:
                print(f"STDOUT was empty. STDERR: {stderr}", file=sys.stderr)

            self.assertIn("this should appear", stdout)
            self.assertNotIn("skip_this", stdout)
            self.assertIn("another visible message", stdout)

    def test_include_pattern_shows_only_matching(self):
        """Only messages matching --include pattern should appear"""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir = Path(tmpdir)
            jsonl = tmpdir / "test.jsonl"
            jsonl.touch()

            write_server_config(tmpdir, jsonl)

            proc = subprocess.Popen(
                [sys.executable, VIEWER, "--include", "error", "--no-color"],
                cwd=tmpdir,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

            time.sleep(0.5)

            with open(jsonl, "a") as f:
                write_jsonl_record(f, "this is an error")
                write_jsonl_record(f, "info message")
                write_jsonl_record(f, "another error here")

            time.sleep(0.5)
            proc.terminate()
            stdout, _ = proc.communicate(timeout=5)

            self.assertIn("this is an error", stdout)
            self.assertNotIn("info message", stdout)
            self.assertIn("another error here", stdout)

    def test_exclude_takes_precedence_over_include(self):
        """Exclude patterns should take precedence over include patterns"""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir = Path(tmpdir)
            jsonl = tmpdir / "test.jsonl"
            jsonl.touch()

            write_server_config(tmpdir, jsonl)

            proc = subprocess.Popen(
                [
                    sys.executable,
                    VIEWER,
                    "--include",
                    "error",
                    "--exclude",
                    "test",
                    "--no-color",
                ],
                cwd=tmpdir,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

            time.sleep(0.5)

            with open(jsonl, "a") as f:
                write_jsonl_record(f, "real error")
                write_jsonl_record(f, "test error")  # matches both
                write_jsonl_record(f, "production error")

            time.sleep(0.5)
            proc.terminate()
            stdout, _ = proc.communicate(timeout=5)

            self.assertIn("real error", stdout)
            self.assertNotIn("test error", stdout)  # excluded
            self.assertIn("production error", stdout)

    def test_multiple_exclude_patterns(self):
        """Multiple --exclude patterns should all be applied"""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir = Path(tmpdir)
            jsonl = tmpdir / "test.jsonl"
            jsonl.touch()

            write_server_config(tmpdir, jsonl)

            proc = subprocess.Popen(
                [
                    sys.executable,
                    VIEWER,
                    "--exclude",
                    "debug",
                    "--exclude",
                    "test",
                    "--no-color",
                ],
                cwd=tmpdir,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

            time.sleep(0.5)

            with open(jsonl, "a") as f:
                write_jsonl_record(f, "normal message")
                write_jsonl_record(f, "debug output")
                write_jsonl_record(f, "test result")

            time.sleep(0.5)
            proc.terminate()
            stdout, _ = proc.communicate(timeout=5)

            self.assertIn("normal message", stdout)
            self.assertNotIn("debug output", stdout)
            self.assertNotIn("test result", stdout)

    def test_config_file_filters_combined_with_cli(self):
        """Config file filters should combine with CLI filters"""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir = Path(tmpdir)
            jsonl = tmpdir / "test.jsonl"
            jsonl.touch()

            write_server_config(tmpdir, jsonl)
            write_viewer_config(tmpdir, exclude="config_exclude")

            proc = subprocess.Popen(
                [sys.executable, VIEWER, "--exclude", "cli_exclude", "--no-color", "--verbose"],
                cwd=tmpdir,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

            time.sleep(0.5)

            with open(jsonl, "a") as f:
                write_jsonl_record(f, "normal message")
                write_jsonl_record(f, "config_exclude message")
                write_jsonl_record(f, "cli_exclude message")

            time.sleep(0.5)
            proc.terminate()
            stdout, stderr = proc.communicate(timeout=5)

            # Check that both config and CLI excludes are reported (in verbose mode)
            self.assertIn("config_exclude", stderr)
            self.assertIn("cli_exclude", stderr)

            # Check filtering works
            self.assertIn("normal message", stdout)
            self.assertNotIn("config_exclude", stdout)
            self.assertNotIn("cli_exclude", stdout)


# ── Column selection tests ───────────────────────────────────────────────────


class TestColumnSelection(unittest.TestCase):
    def test_default_columns_shown(self):
        """Default columns should be displayed"""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir = Path(tmpdir)
            jsonl = tmpdir / "test.jsonl"
            jsonl.touch()

            write_server_config(tmpdir, jsonl)

            proc = subprocess.Popen(
                [sys.executable, VIEWER, "--no-color"],
                cwd=tmpdir,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

            time.sleep(0.5)

            with open(jsonl, "a") as f:
                write_jsonl_record(f, "test message", facility="daemon", severity="INFO")

            time.sleep(0.5)
            proc.terminate()
            stdout, _ = proc.communicate(timeout=5)

            # Default columns include facility, severity, message
            self.assertIn("daemon", stdout)
            self.assertIn("INFO", stdout)
            self.assertIn("test message", stdout)

    def test_custom_columns_from_config(self):
        """Custom column selection from config file should work"""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir = Path(tmpdir)
            jsonl = tmpdir / "test.jsonl"
            jsonl.touch()

            write_server_config(tmpdir, jsonl)
            write_viewer_config(tmpdir, columns="message")  # only message column

            proc = subprocess.Popen(
                [sys.executable, VIEWER, "--no-color", "--verbose"],
                cwd=tmpdir,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

            time.sleep(0.5)

            with open(jsonl, "a") as f:
                write_jsonl_record(f, "test message", facility="daemon", severity="INFO")

            time.sleep(0.5)
            proc.terminate()
            stdout, stderr = proc.communicate(timeout=5)

            # Should only show message column (in verbose mode)
            self.assertIn("Columns: message", stderr)
            self.assertIn("test message", stdout)


# ── Tail behavior tests ──────────────────────────────────────────────────────


class TestTailBehavior(unittest.TestCase):
    def test_show_all_mode(self):
        """--show-all should display all lines and exit"""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir = Path(tmpdir)
            jsonl = tmpdir / "test.jsonl"

            write_server_config(tmpdir, jsonl)

            # Write 15 test messages
            with open(jsonl, "w") as f:
                for i in range(1, 16):
                    write_jsonl_record(f, f"Message {i}")

            result = subprocess.run(
                [sys.executable, VIEWER, "--show-all", "--no-color"],
                cwd=tmpdir,
                capture_output=True,
                text=True,
                timeout=5,
            )

            lines = [line for line in result.stdout.split("\n") if "Message" in line]
            self.assertEqual(len(lines), 15, "Should show all 15 messages")
            self.assertIn("Message 1", lines[0], "First message should be Message 1")
            self.assertIn("Message 15", lines[-1], "Last message should be Message 15")

    def test_default_shows_last_10_lines(self):
        """Default behavior should show last 10 lines"""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir = Path(tmpdir)
            jsonl = tmpdir / "test.jsonl"

            write_server_config(tmpdir, jsonl)

            # Write 15 test messages
            with open(jsonl, "w") as f:
                for i in range(1, 16):
                    write_jsonl_record(f, f"Message {i}")

            proc = subprocess.Popen(
                [sys.executable, VIEWER, "--no-color"],
                cwd=tmpdir,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

            time.sleep(0.5)
            proc.terminate()
            stdout, _ = proc.communicate(timeout=5)

            lines = [line for line in stdout.split("\n") if "Message" in line]
            self.assertEqual(len(lines), 10, "Should show last 10 messages")
            self.assertIn("Message 6", lines[0], "First shown should be Message 6")
            self.assertIn("Message 15", lines[-1], "Last shown should be Message 15")

    def test_custom_lines_parameter(self):
        """--lines N should show last N lines"""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir = Path(tmpdir)
            jsonl = tmpdir / "test.jsonl"

            write_server_config(tmpdir, jsonl)

            # Write 15 test messages
            with open(jsonl, "w") as f:
                for i in range(1, 16):
                    write_jsonl_record(f, f"Message {i}")

            proc = subprocess.Popen(
                [sys.executable, VIEWER, "--lines", "3", "--no-color"],
                cwd=tmpdir,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

            time.sleep(0.5)
            proc.terminate()
            stdout, _ = proc.communicate(timeout=5)

            lines = [line for line in stdout.split("\n") if "Message" in line]
            self.assertEqual(len(lines), 3, "Should show last 3 messages")
            self.assertIn("Message 13", lines[0])
            self.assertIn("Message 15", lines[-1])

    def test_lines_zero_shows_no_initial_lines(self):
        """--lines 0 should show no initial lines (follow-only mode)"""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir = Path(tmpdir)
            jsonl = tmpdir / "test.jsonl"

            write_server_config(tmpdir, jsonl)

            # Write test messages before starting viewer
            with open(jsonl, "w") as f:
                for i in range(1, 6):
                    write_jsonl_record(f, f"Message {i}")

            proc = subprocess.Popen(
                [sys.executable, VIEWER, "--lines", "0", "--no-color"],
                cwd=tmpdir,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

            time.sleep(0.5)
            proc.terminate()
            stdout, _ = proc.communicate(timeout=5)

            # Should not show any of the existing messages
            self.assertNotIn("Message 1", stdout)
            self.assertNotIn("Message 5", stdout)

    def test_lines_exceeds_available(self):
        """--lines N where N > available should show all lines"""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir = Path(tmpdir)
            jsonl = tmpdir / "test.jsonl"

            write_server_config(tmpdir, jsonl)

            # Write only 5 messages
            with open(jsonl, "w") as f:
                for i in range(1, 6):
                    write_jsonl_record(f, f"Message {i}")

            proc = subprocess.Popen(
                [sys.executable, VIEWER, "--lines", "20", "--no-color"],
                cwd=tmpdir,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

            time.sleep(0.5)
            proc.terminate()
            stdout, _ = proc.communicate(timeout=5)

            lines = [line for line in stdout.split("\n") if "Message" in line]
            self.assertEqual(len(lines), 5, "Should show all 5 available messages")
            self.assertIn("Message 1", lines[0])
            self.assertIn("Message 5", lines[-1])


# ── UNKNOWN proto tests ───────────────────────────────────────────────────────


class TestUnknownProto(unittest.TestCase):
    def test_unknown_proto_record_displayed(self):
        """proto=UNKNOWN records (only rcv/src/message populated) should display"""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir = Path(tmpdir)
            jsonl = tmpdir / "test.jsonl"

            write_server_config(tmpdir, jsonl)

            # Write a minimal UNKNOWN record (as the server would emit)
            record = {
                "rcv": "2026-03-29T12:00:00Z",
                "src": "10.0.0.1",
                "proto": "UNKNOWN",
                "facility": None,
                "severity": None,
                "hostname": None,
                "app": None,
                "pid": None,
                "msgid": None,
                "message": "malformed syslog payload",
            }
            with open(jsonl, "w") as f:
                f.write(json.dumps(record) + "\n")

            result = subprocess.run(
                [sys.executable, VIEWER, "--show-all", "--no-color"],
                cwd=tmpdir,
                capture_output=True,
                text=True,
                timeout=5,
            )

            self.assertIn("malformed syslog payload", result.stdout)

    def test_unknown_proto_null_fields_omitted(self):
        """Null fields in an UNKNOWN record should not appear as the word 'None'"""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir = Path(tmpdir)
            jsonl = tmpdir / "test.jsonl"

            write_server_config(tmpdir, jsonl)

            record = {
                "rcv": "2026-03-29T12:00:00Z",
                "src": "10.0.0.1",
                "proto": "UNKNOWN",
                "facility": None,
                "severity": None,
                "hostname": None,
                "app": None,
                "pid": None,
                "msgid": None,
                "message": "raw payload",
            }
            with open(jsonl, "w") as f:
                f.write(json.dumps(record) + "\n")

            result = subprocess.run(
                [sys.executable, VIEWER, "--show-all", "--no-color"],
                cwd=tmpdir,
                capture_output=True,
                text=True,
                timeout=5,
            )

            self.assertNotIn("None", result.stdout)
            self.assertIn("raw payload", result.stdout)

    def test_unknown_proto_excluded_by_filter(self):
        """Filtering should work on the message field of UNKNOWN records"""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir = Path(tmpdir)
            jsonl = tmpdir / "test.jsonl"

            write_server_config(tmpdir, jsonl)

            records = [
                {
                    "rcv": "2026-03-29T12:00:00Z",
                    "src": "10.0.0.1",
                    "proto": "UNKNOWN",
                    "facility": None,
                    "severity": None,
                    "hostname": None,
                    "app": None,
                    "pid": None,
                    "msgid": None,
                    "message": "keep this",
                },
                {
                    "rcv": "2026-03-29T12:00:00Z",
                    "src": "10.0.0.1",
                    "proto": "UNKNOWN",
                    "facility": None,
                    "severity": None,
                    "hostname": None,
                    "app": None,
                    "pid": None,
                    "msgid": None,
                    "message": "drop this",
                },
            ]
            with open(jsonl, "w") as f:
                for r in records:
                    f.write(json.dumps(r) + "\n")

            result = subprocess.run(
                [sys.executable, VIEWER, "--show-all", "--no-color", "--exclude", "drop"],
                cwd=tmpdir,
                capture_output=True,
                text=True,
                timeout=5,
            )

            self.assertIn("keep this", result.stdout)
            self.assertNotIn("drop this", result.stdout)


# ── show-all + filter tests ───────────────────────────────────────────────────


class TestShowAllWithFilters(unittest.TestCase):
    def test_show_all_with_exclude(self):
        """--show-all combined with --exclude should filter output"""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir = Path(tmpdir)
            jsonl = tmpdir / "test.jsonl"

            write_server_config(tmpdir, jsonl)

            with open(jsonl, "w") as f:
                write_jsonl_record(f, "keep message")
                write_jsonl_record(f, "skip message")
                write_jsonl_record(f, "another keep")

            result = subprocess.run(
                [sys.executable, VIEWER, "--show-all", "--no-color", "--exclude", "skip"],
                cwd=tmpdir,
                capture_output=True,
                text=True,
                timeout=5,
            )

            self.assertIn("keep message", result.stdout)
            self.assertNotIn("skip message", result.stdout)
            self.assertIn("another keep", result.stdout)

    def test_show_all_with_include(self):
        """--show-all combined with --include should filter output"""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir = Path(tmpdir)
            jsonl = tmpdir / "test.jsonl"

            write_server_config(tmpdir, jsonl)

            with open(jsonl, "w") as f:
                write_jsonl_record(f, "error occurred")
                write_jsonl_record(f, "info event")
                write_jsonl_record(f, "another error")

            result = subprocess.run(
                [sys.executable, VIEWER, "--show-all", "--no-color", "--include", "error"],
                cwd=tmpdir,
                capture_output=True,
                text=True,
                timeout=5,
            )

            self.assertIn("error occurred", result.stdout)
            self.assertNotIn("info event", result.stdout)
            self.assertIn("another error", result.stdout)


# ── Log rotation tests ────────────────────────────────────────────────────────


class TestLogRotation(unittest.TestCase):
    def test_follows_new_file_after_rotation(self):
        """Viewer should continue showing messages after the log file is rotated"""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir = Path(tmpdir)
            jsonl = tmpdir / "test.jsonl"
            jsonl.touch()

            write_server_config(tmpdir, jsonl)

            proc = subprocess.Popen(
                [sys.executable, VIEWER, "--lines", "0", "--no-color"],
                cwd=tmpdir,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

            # Give viewer time to start and reach follow mode
            time.sleep(0.5)

            # Write a message to the original file
            with open(jsonl, "a") as f:
                write_jsonl_record(f, "before rotation")

            time.sleep(0.3)

            # Simulate rotation: rename the current file, create a new empty one
            rotated = tmpdir / "test.jsonl.1"
            os.rename(jsonl, rotated)
            jsonl.touch()

            # Give viewer time to detect the rotation
            time.sleep(0.5)

            # Write a message to the new file
            with open(jsonl, "a") as f:
                write_jsonl_record(f, "after rotation")

            time.sleep(0.3)
            proc.terminate()
            stdout, _ = proc.communicate(timeout=5)

            self.assertIn("before rotation", stdout)
            self.assertIn("after rotation", stdout)

    def test_negative_lines_rejected(self):
        """--lines with a negative value should exit with an error"""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir = Path(tmpdir)
            jsonl = tmpdir / "test.jsonl"
            jsonl.touch()

            write_server_config(tmpdir, jsonl)

            result = subprocess.run(
                [sys.executable, VIEWER, "--lines", "-1", "--no-color"],
                cwd=tmpdir,
                capture_output=True,
                text=True,
                timeout=5,
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("non-negative", result.stderr)


# ── Entry point ──────────────────────────────────────────────────────────────

if __name__ == "__main__":
    if not VIEWER:
        print(f"Usage: {sys.argv[0]} <path-to-minilog-cli-viewer.py>", file=sys.stderr)
        sys.exit(1)
    unittest.main()
