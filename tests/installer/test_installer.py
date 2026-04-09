"""Smoke-tests for the minilog Inno Setup installer.

Test groups (run in sequence):
  1. Clean install   — files, directories, service registered + running
  2. UDP smoke       — send a syslog datagram, verify it lands in the log
  3. Upgrade install — service survives; user-modified config is not overwritten
  4. Uninstall       — service and binary removed; config file survives

Must be run as Administrator (the installer registers a Windows service).

Usage:
    python test_installer.py <path-to-minilog-*-setup.exe>
"""

import argparse
import json
import os
import random
import re
import socket
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

# ─── Paths ────────────────────────────────────────────────────────────────────

PROGRAM_FILES  = Path(os.environ["ProgramFiles"])
PROGRAM_DATA   = Path(os.environ["ProgramData"])
APP_DIR        = PROGRAM_FILES / "minilog"
TOOLS_DIR      = APP_DIR / "tools"
DATA_DIR       = PROGRAM_DATA / "minilog"
EXE_PATH       = APP_DIR / "minilog.exe"
WEB_VIEWER_EXE = APP_DIR / "minilog-web-viewer.exe"
VIEWER_PATH    = TOOLS_DIR / "minilog-cli-viewer.py"
UNINST_PATH    = APP_DIR / "unins000.exe"
CONFIG_PATH    = DATA_DIR / "minilog.conf"
VIEWER_CONFIG  = DATA_DIR / "minilog-cli-viewer.conf"
LOG_DIR        = DATA_DIR / "logs"
LOG_FILE       = LOG_DIR / "syslog.log"
SERVICE_NAME   = "minilog"
WEB_SERVICE    = "minilog-web-viewer"
WEB_VIEWER_URL = "http://localhost:8080"

# ─── Helpers ──────────────────────────────────────────────────────────────────

passed = 0
failed = 0


def check(condition: bool, message: str) -> None:
    global passed, failed
    if condition:
        print(f"  PASS: {message}")
        passed += 1
    else:
        print(f"  FAIL: {message}", file=sys.stderr)
        failed += 1


def run_installer(path: Path) -> None:
    result = subprocess.run(
        [str(path), "/VERYSILENT", "/SUPPRESSMSGBOXES"],
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(f"Installer exited with code {result.returncode}")


def run_uninstaller() -> None:
    result = subprocess.run(
        [str(UNINST_PATH), "/VERYSILENT", "/SUPPRESSMSGBOXES"],
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(f"Uninstaller exited with code {result.returncode}")


def sc(*args: str) -> subprocess.CompletedProcess:
    """Run sc.exe and return the completed process (does not raise on failure)."""
    return subprocess.run(
        ["sc.exe", *args],
        capture_output=True,
        text=True,
        check=False,
    )


def service_exists(name: str = SERVICE_NAME) -> bool:
    return sc("query", name).returncode == 0


def service_state(name: str = SERVICE_NAME) -> str:
    """Return the service state string, e.g. 'RUNNING', or '' if not found."""
    result = sc("query", name)
    m = re.search(r"STATE\s*:\s*\d+\s+(\w+)", result.stdout)
    return m.group(1) if m else ""


def service_start_type(name: str = SERVICE_NAME) -> str:
    """Return the start type string, e.g. 'AUTO_START', or '' if not found."""
    result = sc("qc", name)
    m = re.search(r"START_TYPE\s*:\s*\d+\s+(\w+)", result.stdout)
    return m.group(1) if m else ""


def wait_service_running(name: str = SERVICE_NAME, timeout: int = 15) -> bool:
    for _ in range(timeout):
        if service_state(name) == "RUNNING":
            return True
        time.sleep(1)
    return False


# ─── Test 1: Clean install ────────────────────────────────────────────────────

def test_clean_install(installer: Path) -> None:
    print("\n=== Test 1: Clean install ===")
    run_installer(installer)

    check(EXE_PATH.exists(),    f"minilog.exe present at {APP_DIR}")
    check(TOOLS_DIR.exists(),   f"Tools directory created at {TOOLS_DIR}")
    check(VIEWER_PATH.exists(), f"minilog-cli-viewer.py present at {TOOLS_DIR}")
    check(LOG_DIR.exists(),     f"Log directory created at {LOG_DIR}")
    check(CONFIG_PATH.exists(), f"Config file present at {DATA_DIR}")
    check(VIEWER_CONFIG.exists(), f"Viewer config file present at {DATA_DIR}")
    check(service_exists(),                       f"Service '{SERVICE_NAME}' registered")
    check(service_start_type() == "AUTO_START",   "Service start type is AUTO_START")
    check(wait_service_running(),                  "Service is Running")

    # Test that the viewer script is runnable
    if VIEWER_PATH.exists():
        result = subprocess.run(
            ["python", str(VIEWER_PATH), "--help"],
            capture_output=True,
            text=True,
            check=False,
            timeout=5,
        )
        check(result.returncode == 0, "Viewer script runs successfully (--help)")
        check("minilog-cli-viewer" in result.stdout, "Viewer help output looks correct")

    # Web viewer checks
    check(WEB_VIEWER_EXE.exists(), f"minilog-web-viewer.exe present at {APP_DIR}")
    check(service_exists(WEB_SERVICE), f"Service '{WEB_SERVICE}' registered")
    check(wait_service_running(WEB_SERVICE), f"Service '{WEB_SERVICE}' is Running")

    # Verify the web viewer responds to HTTP requests
    if service_state(WEB_SERVICE) == "RUNNING":
        ok = False
        for _ in range(10):
            try:
                resp = urllib.request.urlopen(f"{WEB_VIEWER_URL}/sinks", timeout=3)
                data = json.loads(resp.read())
                ok = isinstance(data, list)
                break
            except Exception:
                time.sleep(1)
        check(ok, "Web viewer /sinks endpoint returns a JSON array")


# ─── Test 2: UDP smoke test ───────────────────────────────────────────────────

def test_udp_smoke() -> None:
    print("\n=== Test 2: UDP smoke test ===")

    marker = f"installer-test-{random.randint(100000, 999999)}"
    msg = f"<13>Mar 15 10:00:00 testhost minilog-ci: {marker}"

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.sendto(msg.encode("ascii"), ("127.0.0.1", 514))

    time.sleep(2)

    check(LOG_FILE.exists(), f"syslog.log created at {LOG_FILE}")
    if LOG_FILE.exists():
        content = LOG_FILE.read_text(encoding="utf-8", errors="replace")
        check(marker in content, "Sent message appears in syslog.log")


# ─── Test 3: Upgrade install (config not overwritten) ─────────────────────────

def test_upgrade(installer: Path) -> None:
    print("\n=== Test 3: Upgrade install ===")

    sentinel = f"; MODIFIED-BY-INSTALLER-TEST-{random.randint(100000, 999999)}"
    with CONFIG_PATH.open("a", encoding="utf-8") as f:
        f.write(f"\n{sentinel}\n")

    run_installer(installer)
    check(wait_service_running(), "Service running after upgrade")
    check(wait_service_running(WEB_SERVICE), "Web viewer service running after upgrade")

    content = CONFIG_PATH.read_text(encoding="utf-8")
    check(sentinel in content, "Config not overwritten on upgrade")


# ─── Test 4: Uninstall ────────────────────────────────────────────────────────

def test_uninstall() -> None:
    print("\n=== Test 4: Uninstall ===")

    run_uninstaller()

    check(not service_exists(),     "Service removed after uninstall")
    check(not service_exists(WEB_SERVICE), "Web viewer service removed after uninstall")
    check(not EXE_PATH.exists(),    "minilog.exe removed after uninstall")
    check(not WEB_VIEWER_EXE.exists(), "minilog-web-viewer.exe removed after uninstall")
    check(not VIEWER_PATH.exists(), "minilog-cli-viewer.py removed after uninstall")
    check(CONFIG_PATH.exists(),     "Config file survives uninstall")
    check(VIEWER_CONFIG.exists(),   "Viewer config file survives uninstall")


# ─── Entry point ──────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("installer", type=Path, help="Path to minilog-*-setup.exe")
    args = parser.parse_args()

    if not args.installer.exists():
        sys.exit(f"Installer not found: {args.installer}")

    test_clean_install(args.installer)
    test_udp_smoke()
    test_upgrade(args.installer)
    test_uninstall()

    color = "\033[92m" if failed == 0 else "\033[91m"
    reset = "\033[0m"
    print(f"\n{color}=== Results: {passed} passed, {failed} failed ==={reset}")
    if failed > 0:
        sys.exit(1)


if __name__ == "__main__":
    main()
