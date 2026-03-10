#!/usr/bin/env python3
"""
UDP-only Syslog server for Windows supporting RFC3164 and RFC5424 message formats (transport RFC5426).
- UDP only (no TCP/TLS).
- Ignores structured data (RFC5424) but preserves message text.
- Exclude messages matching regex patterns.
- Output to stdout and/or a file.

Examples:
  python syslog-server.py --port 514
  python syslog-server.py --port 5514 -e "healthcheck" -e "ignore_this" -o logs.txt
  python syslog-server.py --host 0.0.0.0 --port 514 --case-sensitive -e "DEBUG"

Note: On Windows, binding to port 514 may require Administrator privileges depending on environment.
"""

import argparse
import datetime as _dt
import re
import socketserver
import configparser
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import sys
import threading
from dataclasses import dataclass
from typing import Optional, List


SEVERITY_NAMES = [
    "EMERGENCY",  # 0
    "ALERT",      # 1
    "CRITICAL",   # 2
    "ERROR",      # 3
    "WARNING",    # 4
    "NOTICE",     # 5
    "INFO",       # 6
    "DEBUG",      # 7
]

FACILITY_NAMES = [
    "kern",        # 0
    "user",        # 1
    "mail",        # 2
    "daemon",      # 3
    "auth",        # 4
    "syslog",      # 5
    "lpr",         # 6
    "news",        # 7
    "uucp",        # 8
    "clock",       # 9
    "authpriv",    # 10
    "ftp",         # 11
    "ntp",         # 12
    "audit",       # 13
    "alert",       # 14
    "cron",        # 15
    "local0",      # 16
    "local1",      # 17
    "local2",      # 18
    "local3",      # 19
    "local4",      # 20
    "local5",      # 21
    "local6",      # 22
    "local7",      # 23
]


@dataclass
class SyslogMessage:
    protocol: str
    raw: str
    pri: Optional[int] = None
    facility: Optional[int] = None
    severity: Optional[int] = None
    facility_name: Optional[str] = None
    severity_name: Optional[str] = None
    version: Optional[int] = None
    timestamp: Optional[str] = None
    hostname: Optional[str] = None
    app_name: Optional[str] = None
    procid: Optional[str] = None
    msgid: Optional[str] = None
    message: str = ""


def _facility_name(n: int) -> str:
    if 0 <= n < len(FACILITY_NAMES):
        return FACILITY_NAMES[n]
    return f"facility{n}"


def _severity_name(n: int) -> str:
    if 0 <= n < len(SEVERITY_NAMES):
        return SEVERITY_NAMES[n]
    return f"severity{n}"


def _apply_pri_fields(msg: SyslogMessage) -> None:
    if msg.pri is None:
        return
    fac = msg.pri // 8
    sev = msg.pri % 8
    msg.facility = fac
    msg.severity = sev
    msg.facility_name = _facility_name(fac)
    msg.severity_name = _severity_name(sev)


# Regexes for parsing
_RE_5424 = re.compile(
    r'^<(?P<pri>\d{1,3})>(?P<ver>\d{1,3})\s+'
    r'(?P<ts>\S+)\s+'
    r'(?P<host>\S+)\s+'
    r'(?P<app>\S+)\s+'
    r'(?P<proc>\S+)\s+'
    r'(?P<msgid>\S+)'
    r'(?:\s+(?P<rest>.*))?$'
)

# Example RFC3164:
# <34>Oct 11 22:14:15 mymachine su[123]: 'su root' failed for lonvick on /dev/pts/8
_RE_3164 = re.compile(
    r'^<(?P<pri>\d{1,3})>'
    r'(?P<ts>[A-Z][a-z]{2}\s+\d{1,2}\s+\d{2}:\d{2}:\d{2})\s+'
    r'(?P<host>\S+)\s+'
    r'(?P<tag>[^:]+):?\s*'
    r'(?P<msg>.*)$'
)


def parse_syslog_line(text: str) -> SyslogMessage:
    s = text.strip("\r\n")
    # Try RFC5424 first
    m = _RE_5424.match(s)
    if m:
        pri = int(m.group("pri"))
        version = int(m.group("ver"))
        ts = m.group("ts")
        host = m.group("host")
        app = m.group("app")
        proc = m.group("proc")
        msgid = m.group("msgid")
        rest = m.group("rest") or ""
        # We don't support structured data; treat everything after msgid as message text.
        msg_text = rest
        msg = SyslogMessage(
            protocol="RFC5424",
            raw=s,
            pri=pri,
            version=version,
            timestamp=ts if ts != "-" else None,
            hostname=host if host != "-" else None,
            app_name=app if app != "-" else None,
            procid=proc if proc != "-" else None,
            msgid=msgid if msgid != "-" else None,
            message=msg_text.strip()
        )
        _apply_pri_fields(msg)
        return msg

    # Try RFC3164
    m = _RE_3164.match(s)
    if m:
        pri = int(m.group("pri"))
        ts = m.group("ts")
        host = m.group("host")
        tag = m.group("tag") or ""
        msg_text = m.group("msg") or ""

        app, proc = None, None
        # tag can be 'app', or 'app[pid]'
        tag = tag.strip()
        if tag.endswith("]") and "[" in tag:
            base, rest = tag.split("[", 1)
            app = base
            proc = rest[:-1]  # strip trailing ]
        else:
            app = tag

        msg = SyslogMessage(
            protocol="RFC3164",
            raw=s,
            pri=pri,
            timestamp=ts,
            hostname=host,
            app_name=app if app else None,
            procid=proc if proc else None,
            message=msg_text.strip()
        )
        _apply_pri_fields(msg)
        return msg

    # Unknown format, still return something useful
    return SyslogMessage(
        protocol="UNKNOWN",
        raw=s,
        message=s
    )


class OutputSink:
    def __init__(self, to_stdout: bool = True, file_path: Optional[str] = None, encoding: str = "utf-8"):
        self._to_stdout = to_stdout
        self._lock = threading.Lock()
        self._file = None
        if file_path:
            # Open in append mode, text, with requested encoding
            self._file = open(file_path, "a", encoding=encoding)

    def write_line(self, text: str) -> None:
        # Ensure single-line output
        line = text.replace("\r", " ").replace("\n", " ")
        with self._lock:
            if self._to_stdout:
                print(line, flush=True)
            if self._file:
                self._file.write(line + "\n")
                self._file.flush()

    def close(self) -> None:
        with self._lock:
            if self._file:
                try:
                    self._file.close()
                finally:
                    self._file = None


class SyslogUDPServer(socketserver.UDPServer):
    allow_reuse_address = True
    max_packet_size = 65507
    _executor: Optional[ThreadPoolExecutor] = None

    def __init__(self, server_address, RequestHandlerClass, *, bind_and_activate=True):
        super().__init__(server_address, RequestHandlerClass, bind_and_activate=bind_and_activate)
        # The following attributes will be injected from main():
        # - patterns: List[re.Pattern]
        # - sink: OutputSink
        # - encoding: str
        self.patterns: List[re.Pattern] = []
        self.sink: OutputSink = OutputSink()
        self.encoding: str = "utf-8"
        self.show_ts: bool = False
        self.show_src: bool = False
        self._executor: Optional[ThreadPoolExecutor] = None

    def set_worker_pool(self, max_workers: int) -> None:
        if self._executor is None:
            self._executor = ThreadPoolExecutor(
                max_workers=max(1, int(max_workers)),
                thread_name_prefix="syslog-worker"
            )

    def process_request(self, request, client_address):
        executor = getattr(self, "_executor", None)
        if executor is None:
            # Default to a small pool if not explicitly set
            self.set_worker_pool(4)
            executor = self._executor
        executor.submit(self._process_request_task, request, client_address)

    def _process_request_task(self, request, client_address):
        try:
            self.finish_request(request, client_address)
            self.shutdown_request(request)
        except Exception:
            # Swallow exceptions from handler to keep server running
            pass

    def server_close(self):
        try:
            executor = getattr(self, "_executor", None)
            if executor is not None:
                executor.shutdown(wait=True)
                self._executor = None
        finally:
            super().server_close()


class SyslogUDPHandler(socketserver.BaseRequestHandler):
    def handle(self):
        data = self.request[0]
        if not isinstance(data, (bytes, bytearray)):
            return

        try:
            text = data.decode(self.server.encoding, errors="replace")
        except Exception:
            # Fallback to utf-8 replacement if a custom encoding failed
            text = data.decode("utf-8", errors="replace")

        msg = parse_syslog_line(text)

        # Exclusion check on raw message
        for pat in self.server.patterns:
            if pat.search(msg.raw):
                return

        out_line = self._format_line(msg)
        self.server.sink.write_line(out_line)

    def _format_line(self, msg: SyslogMessage) -> str:
        # RFC3339 UTC receive time
        received_at = _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        parts = [received_at, msg.protocol]

        if msg.facility_name is not None and msg.severity_name is not None:
            parts.append(f"{msg.facility_name}.{msg.severity_name}")
        elif msg.pri is not None:
            parts.append(f"pri={msg.pri}")

        if msg.hostname:
            parts.append(msg.hostname)

        tag_parts = []
        if msg.app_name:
            tag_parts.append(msg.app_name)
        if msg.procid and msg.procid != "-":
            if tag_parts:
                tag_parts[-1] = f"{tag_parts[-1]}[{msg.procid}]"
            else:
                tag_parts.append(f"[{msg.procid}]")
        if msg.msgid and msg.msgid != "-":
            tag_parts.append(msg.msgid)
        if tag_parts:
            parts.append(" ".join(tag_parts))

        if self.server.show_ts and msg.timestamp:
            parts.append(f"ts={msg.timestamp}")

        # Optionally include source address
        if self.server.show_src:
            parts.append(f"src={self.client_address[0]}:{self.client_address[1]}")

        if msg.message:
            safe_msg = msg.message.replace("\r", " ").replace("\n", " ")
            parts.append(f"-- {safe_msg}")

        return " ".join(parts)


def _parse_bool(value: str) -> bool:
    v = value.strip().lower()
    return v in ("1", "true", "yes", "on")

def _load_config() -> dict:
    """
    Load INI config from syslog-server.conf next to this script.
    Returns a dict with optional keys: host, port, encoding, output, no_stdout,
    case_sensitive, show_ts, show_src, workers, exclude (list).
    """
    cfg = {}
    try:
        cfg_path = Path(__file__).with_name("syslog-server.conf")
    except Exception:
        cfg_path = None
    if not cfg_path or not cfg_path.exists():
        return cfg

    try:
        raw_text = cfg_path.read_text(encoding="utf-8")
        parser = configparser.ConfigParser(strict=False)
        parser.read_string(raw_text)
    except (OSError, configparser.Error) as e:
        print(f"Failed to parse configuration file '{cfg_path}': {e}", file=sys.stderr)
        sys.exit(2)

    if not parser.has_section("server"):
        return cfg

    sec = parser["server"]

    if "host" in sec:
        cfg["host"] = sec.get("host", fallback=None)
    if "port" in sec:
        try:
            cfg["port"] = sec.getint("port", fallback=None)
        except Exception:
            pass
    if "encoding" in sec:
        cfg["encoding"] = sec.get("encoding", fallback=None)
    if "output" in sec:
        out = sec.get("output", fallback=None)
        if out:
            cfg["output"] = out
    if "no_stdout" in sec:
        try:
            cfg["no_stdout"] = _parse_bool(sec.get("no_stdout", fallback="false"))
        except Exception:
            pass
    if "case_sensitive" in sec:
        try:
            cfg["case_sensitive"] = _parse_bool(sec.get("case_sensitive", fallback="false"))
        except Exception:
            pass
    if "show_ts" in sec:
        try:
            cfg["show_ts"] = _parse_bool(sec.get("show_ts", fallback="false"))
        except Exception:
            pass
    if "show_src" in sec:
        try:
            cfg["show_src"] = _parse_bool(sec.get("show_src", fallback="false"))
        except Exception:
            pass
    if "workers" in sec:
        try:
            cfg["workers"] = sec.getint("workers", fallback=None)
        except Exception:
            pass

    # Exclusions: support multiple 'exclude =' lines and/or comma/newline separated values.
    excludes: List[str] = []

    def _split_values(val: str) -> List[str]:
        toks: List[str] = []
        for line in val.splitlines():
            for t in line.split(","):
                t = t.strip()
                if t:
                    toks.append(t)
        return toks

    # 1) Collect from (possibly multi-line) single option value (last occurrence)
    if "exclude" in sec:
        raw_val = sec.get("exclude", fallback="")
        if raw_val:
            excludes.extend(_split_values(raw_val))

    # 2) Collect from duplicate 'exclude' entries by scanning raw text of [server] section
    try:
        section_parts = re.split(r"^\s*\[server\]\s*$", raw_text, flags=re.MULTILINE)
        if len(section_parts) > 1:
            after_server = section_parts[1]
            section_text = re.split(r"^\s*\[.+?\]\s*$", after_server, maxsplit=1, flags=re.MULTILINE)[0]
            for line in section_text.splitlines():
                stripped = line.strip()
                if not stripped or stripped.startswith(("#", ";")):
                    continue
                m = re.match(r"exclude\s*=\s*(.*)$", stripped, flags=re.IGNORECASE)
                if m:
                    excludes.extend(_split_values(m.group(1)))
    except Exception:
        # If scanning fails for any reason, ignore and rely on parsed value
        pass

    # De-duplicate while preserving order
    if excludes:
        seen = set()
        uniq: List[str] = []
        for x in excludes:
            if x not in seen:
                seen.add(x)
                uniq.append(x)
        cfg["exclude"] = uniq

    return cfg

def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="UDP-only syslog server (RFC3164, RFC5424). Options can also come from syslog-server.conf.")
    parser.add_argument("--host", default=None, help="Host/IP to bind. Defaults to value in syslog-server.conf or 0.0.0.0.")
    parser.add_argument("--port", type=int, default=None, help="UDP port to bind. Defaults to value in syslog-server.conf or 514.")
    parser.add_argument("-e", "--exclude", action="append", default=None, help="Regex pattern to exclude (can be repeated). Added to patterns from syslog-server.conf.")
    parser.add_argument("--case-sensitive", action="store_true", default=None, help="Enable case-sensitive matching for exclude patterns (default: case-insensitive).")
    parser.add_argument("-o", "--output", default=None, help="Write output to file in addition to stdout (use --no-stdout to disable stdout).")
    parser.add_argument("--no-stdout", action="store_true", default=None, help="Do not write to stdout.")
    parser.add_argument("--encoding", default=None, help="Decode incoming bytes with this encoding.")
    parser.add_argument("--workers", type=int, default=None, help="Number of worker threads for processing packets.")
    parser.add_argument("--show-ts", action="store_true", default=None, help="Include original timestamp field as ts=...")
    parser.add_argument("--show-src", action="store_true", default=None, help="Include source address as src=ip:port")
    parser.add_argument("--verbose", action="store_true", help="Show effective options before starting.")

    args = parser.parse_args(argv)

    cfg = _load_config()

    host = args.host if args.host is not None else cfg.get("host", "0.0.0.0")
    port = args.port if args.port is not None else cfg.get("port", 514)
    encoding = args.encoding if args.encoding is not None else cfg.get("encoding", "utf-8")
    output = args.output if args.output is not None else cfg.get("output", None)
    workers = args.workers if args.workers is not None else cfg.get("workers", 4)

    # Default to case-insensitive matching unless overridden
    case_sensitive = args.case_sensitive if args.case_sensitive is not None else cfg.get("case_sensitive", False)
    no_stdout = args.no_stdout if args.no_stdout is not None else cfg.get("no_stdout", False)
    show_ts = args.show_ts if args.show_ts is not None else cfg.get("show_ts", False)
    show_src = args.show_src if args.show_src is not None else cfg.get("show_src", False)

    exclude_cfg = cfg.get("exclude", [])
    exclude_cli = args.exclude if args.exclude is not None else []
    exclude_all = list(exclude_cfg) + list(exclude_cli)

    flags = 0 if case_sensitive else re.IGNORECASE
    patterns = [re.compile(p, flags) for p in exclude_all]

    if args.verbose:
        print("Effective options:", flush=True)
        print(f"  host           : {host}", flush=True)
        print(f"  port           : {port}", flush=True)
        print(f"  encoding       : {encoding}", flush=True)
        print(f"  output         : {output if output else '-'}", flush=True)
        print(f"  no_stdout      : {no_stdout}", flush=True)
        print(f"  case_sensitive : {case_sensitive}", flush=True)
        print(f"  show_ts        : {show_ts}", flush=True)
        print(f"  show_src       : {show_src}", flush=True)
        print(f"  workers        : {workers}", flush=True)
        print(f"  exclude_count  : {len(exclude_all)}", flush=True)
        if exclude_all:
            for i, pat in enumerate(exclude_all, 1):
                print(f"    [{i}] {pat}", flush=True)

    sink = OutputSink(to_stdout=not no_stdout, file_path=output, encoding=encoding)

    server = SyslogUDPServer((host, port), SyslogUDPHandler)
    server.patterns = patterns
    server.sink = sink
    server.encoding = encoding
    server.show_ts = show_ts
    server.show_src = show_src
    server.set_worker_pool(workers)

    try:
        print(f"Listening UDP on {host}:{port} (RFC3164, RFC5424; UDP transport RFC5426). Press Ctrl+C to stop.", flush=True)
        server.serve_forever(poll_interval=0.5)
    except KeyboardInterrupt:
        print("\nShutting down...", flush=True)
    finally:
        try:
            server.shutdown()
        except Exception:
            pass
        server.server_close()
        sink.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
