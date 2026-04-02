#!/usr/bin/env python3
"""
minilog-cli-viewer - Real-time JSONL log viewer for minilog

Copyright Saab AB, 2026 (https://github.com/SafirSDK/minilog)
Created by: Lars Hagström / lars@foldspace.nu

This file is part of minilog.
minilog is released under the MIT License.
"""

import argparse
import configparser
import json
import os
import platform
import sys
import time
from collections import deque
from datetime import datetime
from pathlib import Path

if platform.system() == "Windows":
    import ctypes
    import msvcrt
    from ctypes import wintypes

    _kernel32 = ctypes.windll.kernel32  # type: ignore[attr-defined]
    _CreateFileW = _kernel32.CreateFileW
    _CreateFileW.restype = wintypes.HANDLE
    _CreateFileW.argtypes = [
        wintypes.LPCWSTR,
        wintypes.DWORD,
        wintypes.DWORD,
        ctypes.c_void_p,
        wintypes.DWORD,
        wintypes.DWORD,
        wintypes.HANDLE,
    ]
    _GENERIC_READ = 0x80000000
    _FILE_SHARE_READ = 0x00000001
    _FILE_SHARE_WRITE = 0x00000002
    _FILE_SHARE_DELETE = 0x00000004
    _OPEN_EXISTING = 3
    _FILE_ATTRIBUTE_NORMAL = 0x80
    _INVALID_HANDLE_VALUE = wintypes.HANDLE(-1).value

    def _open_shared(path: Path):  # type: ignore[return]
        """Open a file on Windows with FILE_SHARE_DELETE so the server can
        rotate (rename) it while we hold the handle."""
        handle = _CreateFileW(
            str(path),
            _GENERIC_READ,
            _FILE_SHARE_READ | _FILE_SHARE_WRITE | _FILE_SHARE_DELETE,
            None,
            _OPEN_EXISTING,
            _FILE_ATTRIBUTE_NORMAL,
            None,
        )
        if handle == _INVALID_HANDLE_VALUE:
            raise OSError(f"Cannot open {path}: WinError {_kernel32.GetLastError()}")
        fd = msvcrt.open_osfhandle(handle, os.O_RDONLY)
        return open(fd, encoding="utf-8", errors="replace")  # noqa: SIM115

else:

    def _open_shared(path: Path):  # type: ignore[return]
        """On POSIX, a plain open is sufficient — no exclusive locking."""
        return open(path, encoding="utf-8", errors="replace")  # noqa: SIM115


class Colors:
    """ANSI color codes for terminal output"""

    RESET = "\033[0m"
    BOLD = "\033[1m"

    # Facility colors
    KERN = "\033[95m"  # Magenta
    USER = "\033[94m"  # Blue
    MAIL = "\033[96m"  # Cyan
    DAEMON = "\033[92m"  # Green
    AUTH = "\033[93m"  # Yellow
    SYSLOG = "\033[97m"  # White
    LOCAL = "\033[90m"  # Gray

    # Severity colors
    EMERG = "\033[41m\033[97m"  # Red background, white text
    ALERT = "\033[91m\033[1m"  # Bright red, bold
    CRIT = "\033[91m"  # Red
    ERR = "\033[31m"  # Dark red
    WARNING = "\033[33m"  # Yellow
    NOTICE = "\033[36m"  # Cyan
    INFO = "\033[37m"  # Light gray
    DEBUG = "\033[90m"  # Dark gray


class ViewerConfig:
    """Configuration for the log viewer"""

    def __init__(self):
        self.jsonl_file: str | None = None
        self.columns: list[str] = [
            "rcv",
            "facility",
            "severity",
            "hostname",
            "app",
            "pid",
            "message",
        ]
        self.timestamp_format: str = "short"
        self.use_colors: bool = True
        self.exclude_patterns: list[str] = []
        self.include_patterns: list[str] = []


def find_server_config() -> Path | None:
    """Find minilog.conf using standard search order"""
    search_paths = [
        Path("./minilog.conf"),
    ]

    # Platform-specific standard locations
    if platform.system() == "Windows":
        # Windows installer puts config in ProgramData
        programdata = os.environ.get("ProgramData", "C:/ProgramData")
        search_paths.append(Path(programdata) / "minilog" / "minilog.conf")
    else:
        search_paths.append(Path("/etc/minilog/minilog.conf"))

    # Same directory as this script
    script_dir = Path(__file__).parent.absolute()
    search_paths.append(script_dir / "minilog.conf")

    for path in search_paths:
        if path.exists():
            return path

    return None


def find_viewer_config(server_config_path: Path) -> Path | None:
    """Find minilog-cli-viewer.conf"""
    search_paths = [
        Path("./minilog-cli-viewer.conf"),
        server_config_path.parent / "minilog-cli-viewer.conf",
    ]

    for path in search_paths:
        if path.exists():
            return path

    return None


def parse_server_config(config_path: Path, output_section: str = "main") -> str | None:
    """Parse minilog.conf and extract jsonl_file path from specified output section"""
    config = configparser.ConfigParser()
    try:
        config.read(config_path)
        section_name = f"output.{output_section}"

        if section_name not in config:
            print(
                f"Error: output section '{output_section}' not found in {config_path}",
                file=sys.stderr,
            )
            print(
                f"Available sections: {', '.join(s for s in config.sections() if s.startswith('output.'))}",
                file=sys.stderr,
            )
            return None

        if "jsonl_file" not in config[section_name]:
            print(f"Error: jsonl_file not configured in [{section_name}]", file=sys.stderr)
            return None

        return config[section_name]["jsonl_file"]
    except Exception as e:
        print(f"Error parsing server config: {e}", file=sys.stderr)
        return None


def parse_viewer_config(config_path: Path) -> ViewerConfig:
    """Parse minilog-cli-viewer.conf"""
    config = ViewerConfig()
    parser = configparser.ConfigParser()

    try:
        parser.read(config_path)

        if "viewer" in parser:
            viewer = parser["viewer"]

            if "columns" in viewer:
                config.columns = [c.strip() for c in viewer["columns"].split(",")]

            if "timestamp_format" in viewer:
                config.timestamp_format = viewer["timestamp_format"]

            if "use_colors" in viewer:
                config.use_colors = viewer.getboolean("use_colors")

        if "filters" in parser:
            filters = parser["filters"]

            if "exclude" in filters:
                patterns = filters["exclude"].strip().split("\n")
                config.exclude_patterns = [p.strip() for p in patterns if p.strip()]

            if "include" in filters:
                patterns = filters["include"].strip().split("\n")
                config.include_patterns = [p.strip() for p in patterns if p.strip()]

    except Exception as e:
        print(f"Warning: error parsing viewer config, using defaults: {e}", file=sys.stderr)

    return config


def format_timestamp(iso_timestamp: str, format_type: str) -> str:
    """Format ISO8601 timestamp according to format_type"""
    try:
        dt = datetime.fromisoformat(iso_timestamp.replace("Z", "+00:00"))

        if format_type == "iso":
            return iso_timestamp
        elif format_type == "time":
            return dt.strftime("%H:%M:%S")
        else:  # 'short'
            return dt.strftime("%m-%d %H:%M:%S")
    except (ValueError, AttributeError):
        return iso_timestamp


def get_facility_color(facility: str | None) -> str:
    """Get ANSI color code for facility"""
    if not facility:
        return ""

    facility_lower = facility.lower()
    if facility_lower == "kern" or facility_lower == "kernel":
        return Colors.KERN
    elif facility_lower == "user":
        return Colors.USER
    elif facility_lower == "mail":
        return Colors.MAIL
    elif facility_lower == "daemon":
        return Colors.DAEMON
    elif facility_lower in ("auth", "authpriv", "security"):
        return Colors.AUTH
    elif facility_lower == "syslog":
        return Colors.SYSLOG
    elif facility_lower.startswith("local"):
        return Colors.LOCAL
    else:
        return ""


def get_severity_color(severity: str | None) -> str:
    """Get ANSI color code for severity"""
    if not severity:
        return ""

    severity_upper = severity.upper()
    if severity_upper == "EMERG":
        return Colors.EMERG
    elif severity_upper == "ALERT":
        return Colors.ALERT
    elif severity_upper in ("CRIT", "CRITICAL"):
        return Colors.CRIT
    elif severity_upper in ("ERR", "ERROR"):
        return Colors.ERR
    elif severity_upper in ("WARNING", "WARN"):
        return Colors.WARNING
    elif severity_upper == "NOTICE":
        return Colors.NOTICE
    elif severity_upper == "INFO":
        return Colors.INFO
    elif severity_upper == "DEBUG":
        return Colors.DEBUG
    else:
        return ""


def format_message(record: dict, config: ViewerConfig) -> str:
    """Format a JSONL record for display"""
    parts = []

    for col in config.columns:
        value = record.get(col)

        if value is None:
            continue

        if col == "rcv":
            formatted = format_timestamp(value, config.timestamp_format)
            if config.use_colors:
                parts.append(f"{Colors.BOLD}[{formatted}]{Colors.RESET}")
            else:
                parts.append(f"[{formatted}]")

        elif col == "facility":
            if config.use_colors:
                color = get_facility_color(value)
                parts.append(f"{color}{value}{Colors.RESET}")
            else:
                parts.append(value)

        elif col == "severity":
            if config.use_colors:
                color = get_severity_color(value)
                parts.append(f"{color}{value}{Colors.RESET}")
            else:
                parts.append(value)

        elif col == "pid":
            if value:
                parts.append(f"[{value}]")

        elif col in ("src", "proto", "hostname", "app", "msgid"):
            parts.append(str(value))

        elif col == "message":
            # Message is typically the last column and can be long
            parts.append(str(value))

    return " ".join(parts)


def matches_filter(message_text: str, patterns: list[str]) -> bool:
    """Check if message matches any of the patterns (case-insensitive)"""
    message_lower = message_text.lower()
    return any(pattern.lower() in message_lower for pattern in patterns)


def should_display(record: dict, config: ViewerConfig) -> bool:
    """Determine if a record should be displayed based on filters"""
    message = record.get("message", "")

    # Check exclude patterns
    if config.exclude_patterns and matches_filter(message, config.exclude_patterns):
        return False

    # Check include patterns (if any are specified, at least one must match)
    return not config.include_patterns or matches_filter(message, config.include_patterns)


def _file_id(file_path: Path):
    """Return a value that changes when the file is rotated.

    On POSIX we use the inode number; on Windows (where inodes are not
    meaningful) we fall back to the file size, which regresses to 0 on
    rotation and is good enough in practice.
    """
    try:
        st = file_path.stat()
        if platform.system() == "Windows":
            return st.st_size
        return st.st_ino
    except OSError:
        return None


def tail_file(
    file_path: Path,
    config: ViewerConfig,
    lines: int = 10,
    show_all: bool = False,
    verbose: bool = False,
):
    """Tail a file and print new lines as they appear

    Args:
        file_path: Path to JSONL file
        config: Viewer configuration
        lines: Number of lines to show initially (0 = none, only follow new)
        show_all: If True, show all lines and exit (don't follow)
        verbose: Show verbose diagnostic output
    """
    try:
        # Wait for file to exist if it doesn't
        while not file_path.exists():
            if verbose:
                print(f"Waiting for {file_path} to be created...", file=sys.stderr)
            time.sleep(1)

        if verbose:
            mode = (
                "all lines" if show_all else f"last {lines} lines" if lines > 0 else "follow mode"
            )
            print(f"Reading {file_path} ({mode})...", file=sys.stderr)
            print(f"Columns: {', '.join(config.columns)}", file=sys.stderr)
            if config.exclude_patterns:
                print(f"Excluding: {', '.join(config.exclude_patterns)}", file=sys.stderr)
            if config.include_patterns:
                print(f"Including: {', '.join(config.include_patterns)}", file=sys.stderr)
            print("", file=sys.stderr)

        with _open_shared(file_path) as f:
            # Read initial lines from beginning
            if show_all:
                # Show all matching lines and exit
                for line in f:
                    line = line.strip()
                    if not line:
                        continue

                    try:
                        record = json.loads(line)
                        if should_display(record, config):
                            formatted = format_message(record, config)
                            print(formatted)
                    except json.JSONDecodeError:
                        continue

                return  # Exit after showing all

            elif lines > 0:
                # Collect last N matching lines
                last_lines = deque(maxlen=lines)

                for line in f:
                    line = line.strip()
                    if not line:
                        continue

                    try:
                        record = json.loads(line)
                        if should_display(record, config):
                            formatted = format_message(record, config)
                            last_lines.append(formatted)
                    except json.JSONDecodeError:
                        continue

                # Display collected lines
                for formatted in last_lines:
                    print(formatted)
                sys.stdout.flush()
            else:
                # lines == 0: skip to end for follow-only mode
                f.seek(0, 2)

            # Follow mode (if not show_all)
            current_id = _file_id(file_path)
            while True:
                line = f.readline()

                if not line:
                    # Check whether the file has been rotated
                    new_id = _file_id(file_path)
                    if new_id is not None and new_id != current_id:
                        # File was rotated — re-open from the beginning
                        if verbose:
                            print(
                                f"Log rotation detected, re-opening {file_path}...", file=sys.stderr
                            )
                        f.close()
                        # Wait briefly in case the new file is still being created
                        while not file_path.exists():
                            time.sleep(0.1)
                        f = _open_shared(file_path)
                        current_id = _file_id(file_path)
                    else:
                        time.sleep(0.1)
                    continue

                line = line.strip()
                if not line:
                    continue

                try:
                    record = json.loads(line)

                    if should_display(record, config):
                        formatted = format_message(record, config)
                        print(formatted)
                        sys.stdout.flush()

                except json.JSONDecodeError:
                    # Skip malformed JSON lines
                    continue

    except KeyboardInterrupt:
        print("\nExiting...", file=sys.stderr)
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser(
        description="Real-time JSONL log viewer for minilog",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Filtering behavior:
  Both --include and --exclude can be specified multiple times to add multiple
  patterns. Command-line patterns are ADDED to any patterns defined in the config
  file (they do not replace them).

  Exclude patterns are checked first - if a message matches ANY exclude pattern,
  it is filtered out. Include patterns are then checked - if specified, at least
  ONE must match for the message to be shown.

  Examples:
    --include "error" --include "warning"
      Shows messages containing "error" OR "warning"

    --exclude "test" --exclude "debug"
      Hides messages containing "test" OR "debug"

    --include "error" --exclude "test"
      Shows messages with "error" but excludes any that also contain "test"
        """,
    )
    parser.add_argument(
        "--output-section",
        default="main",
        help="Output section name from minilog.conf (default: main)",
    )
    parser.add_argument(
        "--exclude",
        action="append",
        default=[],
        help="Exclude messages containing this string (repeatable, adds to config file patterns)",
    )
    parser.add_argument(
        "--include",
        action="append",
        default=[],
        help="Include only messages containing this string (repeatable, adds to config file patterns)",
    )
    parser.add_argument("--no-color", action="store_true", help="Disable colored output")
    parser.add_argument(
        "--verbose",
        "-v",
        action="store_true",
        help="Show verbose output (config discovery, file paths)",
    )
    parser.add_argument(
        "--lines",
        "-n",
        type=int,
        default=10,
        metavar="N",
        help="Number of lines to show initially (default: 10, 0 = none); ignored when --show-all is set",
    )
    parser.add_argument(
        "--show-all",
        action="store_true",
        help="Show all log entries and exit (do not follow); takes precedence over --lines",
    )

    args = parser.parse_args()

    if args.lines < 0:
        parser.error("--lines must be a non-negative integer")

    # Find server config
    server_config_path = find_server_config()
    if not server_config_path:
        print("Error: minilog.conf not found in standard locations:", file=sys.stderr)
        print("  - ./minilog.conf", file=sys.stderr)
        if platform.system() == "Windows":
            print("  - C:/Program Files/minilog/minilog.conf", file=sys.stderr)
        else:
            print("  - /etc/minilog/minilog.conf", file=sys.stderr)
        print(f"  - {Path(__file__).parent.absolute()}/minilog.conf", file=sys.stderr)
        sys.exit(1)

    if args.verbose:
        print(f"Found server config: {server_config_path}", file=sys.stderr)

    # Parse server config to get JSONL file path
    jsonl_file = parse_server_config(server_config_path, args.output_section)
    if not jsonl_file:
        sys.exit(1)

    # Find and parse viewer config
    viewer_config_path = find_viewer_config(server_config_path)
    if viewer_config_path:
        if args.verbose:
            print(f"Found viewer config: {viewer_config_path}", file=sys.stderr)
        config = parse_viewer_config(viewer_config_path)
    else:
        if args.verbose:
            print("No viewer config found, using defaults", file=sys.stderr)
        config = ViewerConfig()

    config.jsonl_file = jsonl_file

    # Apply command-line overrides
    if args.exclude:
        config.exclude_patterns.extend(args.exclude)
    if args.include:
        config.include_patterns.extend(args.include)
    if args.no_color:
        config.use_colors = False

    # Disable colors if not a TTY or on Windows without proper terminal support
    if not sys.stdout.isatty():
        config.use_colors = False
    elif platform.system() == "Windows" and not (
        os.environ.get("WT_SESSION") or os.environ.get("ConEmuANSI") or os.environ.get("COLORTERM")
    ):
        # On Windows, only enable colors if running in a modern terminal
        # (Windows Terminal, ConEmu, or COLORTERM is set)
        config.use_colors = False

    # Start tailing
    tail_file(
        Path(jsonl_file), config, lines=args.lines, show_all=args.show_all, verbose=args.verbose
    )


if __name__ == "__main__":
    main()
