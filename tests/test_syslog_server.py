import socket
import threading
import time
import types
import importlib.util
from pathlib import Path
import re


def load_module_from_path(mod_name: str, path: Path) -> types.ModuleType:
    spec = importlib.util.spec_from_file_location(mod_name, str(path))
    module = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(module)  # type: ignore[attr-defined]
    return module


# Load the project module from the repository root
PROJECT_ROOT = Path(__file__).resolve().parents[1]
SYSLOG_SERVER_PATH = PROJECT_ROOT / "syslog-server.py"
syslog = load_module_from_path("syslog_server_module", SYSLOG_SERVER_PATH)


class LocalSink:
    def __init__(self):
        self.lines = []
        self._lock = threading.Lock()

    def write_line(self, text: str):
        with self._lock:
            self.lines.append(text)

    def get_lines(self):
        with self._lock:
            return list(self.lines)


def run_udp_server(host="127.0.0.1", port=0, patterns=None, encoding="utf-8", workers=4):
    sink = LocalSink()
    server = syslog.SyslogUDPServer((host, port), syslog.SyslogUDPHandler)
    server.patterns = patterns or []
    server.sink = sink  # inject our capturing sink
    server.encoding = encoding
    server.set_worker_pool(workers)

    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.1}, daemon=True)
    thread.start()

    addr = server.server_address  # (host, assigned_port)
    return server, sink, addr, thread


def wait_for_lines(sink: LocalSink, n: int, timeout: float = 2.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if len(sink.get_lines()) >= n:
            return True
        time.sleep(0.02)
    return False


def test_parse_rfc3164_basic():
    line = "<14>Oct 11 22:14:15 myhost app[123]: hello world"
    msg = syslog.parse_syslog_line(line)
    assert msg.protocol == "RFC3164"
    assert msg.pri == 14
    assert msg.facility == 1  # user
    assert msg.severity == 6  # info
    assert msg.facility_name == "user"
    assert msg.severity_name == "INFO"
    assert msg.hostname == "myhost"
    assert msg.app_name == "app"
    assert msg.procid == "123"
    assert msg.message == "hello world"


def test_parse_rfc3164_without_pid():
    line = "<14>Oct 11 22:14:15 myhost app: test"
    msg = syslog.parse_syslog_line(line)
    assert msg.protocol == "RFC3164"
    assert msg.app_name == "app"
    assert msg.procid is None
    assert msg.message == "test"


def test_parse_rfc5424_with_sd_kept_in_message():
    # From RFC5424 style: we keep the SD in message since we don't support SD parsing
    line = "<34>1 2003-10-11T22:14:15.003Z mymachine su - ID47 [exampleSDID@32473 iut=\"3\"] 'su root' failed for lonvick"
    msg = syslog.parse_syslog_line(line)
    assert msg.protocol == "RFC5424"
    assert msg.version == 1
    assert msg.pri == 34
    assert msg.facility == 4  # auth
    assert msg.severity == 2  # critical
    assert msg.hostname == "mymachine"
    assert msg.app_name == "su"
    assert msg.procid is None  # "-"
    assert msg.msgid == "ID47"
    # SD should still be there because we don't support structured data
    assert "[exampleSDID@32473" in msg.message
    assert "'su root' failed" in msg.message


def test_parse_rfc5424_nil_values():
    # All NILVALUE except pri/ver; structured data '-' is preserved; message starts after it
    line = "<165>1 - - - - - - test message"
    msg = syslog.parse_syslog_line(line)
    assert msg.protocol == "RFC5424"
    assert msg.timestamp is None
    assert msg.hostname is None
    assert msg.app_name is None
    assert msg.procid is None
    assert msg.msgid is None
    assert msg.message == "- test message"


def test_parse_unknown_format():
    line = "this is not a syslog message"
    msg = syslog.parse_syslog_line(line)
    assert msg.protocol == "UNKNOWN"
    assert msg.message == line
    assert msg.raw == line


def test_server_receives_and_formats_rfc3164():
    server, sink, addr, thread = run_udp_server()
    try:
        host, port = addr
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            msg = "<14>Oct 11 22:14:15 myhost app[99]: hello"
            sock.sendto(msg.encode("utf-8"), (host, port))
        finally:
            sock.close()

        assert wait_for_lines(sink, 1)
        out = sink.get_lines()[0]
        # Output should contain:
        # - protocol
        assert "RFC3164" in out
        # - facility.severity
        assert "user.INFO" in out
        # - hostname and tag
        assert "myhost" in out
        assert "app[99]" in out
        # - ts and src are hidden by default
        assert "ts=" not in out
        assert "src=" not in out
        # - message text
        assert out.endswith("-- hello") or " -- hello" in out
    finally:
        server.shutdown()
        server.server_close()


def test_server_exclusion_case_insensitive_default():
    # Compile pattern without case_sensitive => IGNORECASE
    patterns = [re.compile("hello", re.IGNORECASE)]
    server, sink, addr, thread = run_udp_server(patterns=patterns)
    try:
        host, port = addr
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            msg = "<14>Oct 11 22:14:15 myhost app: HeLLo"
            sock.sendto(msg.encode("utf-8"), (host, port))
        finally:
            sock.close()

        # Should be dropped due to exclusion
        time.sleep(0.2)
        assert len(sink.get_lines()) == 0
    finally:
        server.shutdown()
        server.server_close()


def test_server_exclusion_case_sensitive():
    # Case-sensitive pattern should NOT match different case
    patterns = [re.compile("HELLO", 0)]
    server, sink, addr, thread = run_udp_server(patterns=patterns)
    try:
        host, port = addr
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            msg = "<14>Oct 11 22:14:15 myhost app: hello"
            sock.sendto(msg.encode("utf-8"), (host, port))
        finally:
            sock.close()

        assert wait_for_lines(sink, 1)
        out = sink.get_lines()[0]
        assert "hello" in out
    finally:
        server.shutdown()
        server.server_close()


def test_max_packet_size_constant():
    assert getattr(syslog.SyslogUDPServer, "max_packet_size", None) == 65507


def test_encoding_latin1_roundtrip():
    server, sink, addr, thread = run_udp_server(encoding="latin-1")
    try:
        host, port = addr
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            # message contains 'é' encoded in latin-1
            msg = "<14>Oct 11 22:14:15 myhost app: " + "caf" + "\xe9"
            sock.sendto(msg.encode("latin-1"), (host, port))
        finally:
            sock.close()

        assert wait_for_lines(sink, 1)
        out = sink.get_lines()[0]
        assert "café" in out  # decoded properly
    finally:
        server.shutdown()
        server.server_close()


def test_worker_pool_handles_multiple_messages():
    server, sink, addr, thread = run_udp_server(workers=2)
    try:
        host, port = addr
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            for i in range(10):
                msg = f"<14>Oct 11 22:14:15 myhost app[{i}]: msg{i}"
                sock.sendto(msg.encode("utf-8"), (host, port))
        finally:
            sock.close()

        assert wait_for_lines(sink, 10)
        lines = sink.get_lines()
        assert len(lines) >= 10
        # Simple spot check
        assert any("app[0]" in l for l in lines)
        assert any("app[9]" in l for l in lines)
    finally:
        server.shutdown()
        server.server_close()


def test_load_config_success_and_failure(tmp_path):
    # Create a temporary copy of the module next to different config files
    mod_copy = tmp_path / "syslog-server.py"

    # Copy contents of current module file
    content = SYSLOG_SERVER_PATH.read_text(encoding="utf-8")
    mod_copy.write_text(content, encoding="utf-8")

    # 1) Valid config
    good_cfg = tmp_path / "syslog-server.conf"
    good_cfg.write_text(
        "[server]\n"
        "host = 127.0.0.1\n"
        "port = 5514\n"
        "encoding = latin-1\n"
        "output = test.log\n"
        "no_stdout = true\n"
        "case_sensitive = true\n"
        "workers = 8\n"
        "exclude = foo, bar\n",
        encoding="utf-8",
    )
    mod_good = load_module_from_path("syslog_server_good", mod_copy)
    cfg = mod_good._load_config()
    assert cfg["host"] == "127.0.0.1"
    assert cfg["port"] == 5514
    assert cfg["encoding"] == "latin-1"
    assert cfg["output"] == "test.log"
    assert cfg["no_stdout"] is True
    assert cfg["case_sensitive"] is True
    assert cfg["workers"] == 8
    assert cfg["exclude"] == ["foo", "bar"]

    # 2) Invalid config -> expect SystemExit(2)
    bad_cfg = tmp_path / "syslog-server.conf"
    bad_cfg.write_text(
        "[server]\n"
        "this is not a valid line without equals\n",
        encoding="utf-8",
    )
    mod_bad = load_module_from_path("syslog_server_bad", mod_copy)
    try:
        mod_bad._load_config()
        assert False, "Expected SystemExit due to bad config"
    except SystemExit as e:
        assert e.code == 2
