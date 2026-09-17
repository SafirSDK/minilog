"""Smoke-tests for the minilog Inno Setup installer.

Test groups (run in sequence):
  1. Clean install   — files, directories, services registered + running,
                       recovery actions and Event Log sources configured
  2. UDP smoke       — send a syslog datagram, verify it lands in the log
  3. Recovery        — kill the service process; the SCM restarts it
  4. Failed start    — an unusable config makes `sc start` fail, reports a
                       non-zero exit code, and leaves an Event Log trail
  5. Upgrade install — service survives; user-modified config is not overwritten
  6. Uninstall       — services and binaries removed; config file survives

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
# Derived from the port so the two cannot drift apart when the default changes.
WEB_VIEWER_PORT = 9514
WEB_VIEWER_URL = f"http://localhost:{WEB_VIEWER_PORT}"
CONFIG_BACKUP  = DATA_DIR / "minilog.conf.installer-test-backup"

EVENTLOG_KEY = r"HKLM\SYSTEM\CurrentControlSet\Services\EventLog\Application"

# Win32 status the SCM records when a service reports its own failure; the real
# code is then in SERVICE_EXIT_CODE.
ERROR_SERVICE_SPECIFIC_ERROR = 1066

# Recovery configuration both services are installed with.
EXPECTED_RESTART_DELAYS = ["5000", "5000"]
EXPECTED_RESET_PERIOD   = 300

# A pending restart action fires 5 s after the failure, so a service seen STOPPED
# for longer than that has no restart left queued.
SETTLED_QUIET_SECONDS = 8

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


def wait_service_stopped(name: str = SERVICE_NAME, timeout: int = 30) -> bool:
    for _ in range(timeout):
        if service_state(name) == "STOPPED":
            return True
        time.sleep(1)
    return False


def wait_service_settled(name: str = SERVICE_NAME, timeout: int = 45) -> bool:
    """Wait until `name` has been STOPPED for SETTLED_QUIET_SECONDS in a row.

    A failed start queues SCM restart attempts, each of which briefly puts the
    service back into START_PENDING. Waiting for a stable STOPPED keeps the next
    case from racing a restart that is still queued.
    """
    stable = 0
    for _ in range(timeout):
        stable = stable + 1 if service_state(name) == "STOPPED" else 0
        if stable >= SETTLED_QUIET_SECONDS:
            return True
        time.sleep(1)
    return False


def stop_service(name: str = SERVICE_NAME) -> bool:
    """Stop the service and wait for it to reach STOPPED (already-stopped is fine)."""
    sc("stop", name)
    return wait_service_stopped(name)


def service_pid(name: str = SERVICE_NAME) -> int:
    """Return the PID of the service process, or 0 if it is not running."""
    m = re.search(r"PID\s*:\s*(\d+)", sc("queryex", name).stdout)
    return int(m.group(1)) if m else 0


def service_exit_codes(name: str = SERVICE_NAME) -> tuple[int, int]:
    """Return (WIN32_EXIT_CODE, SERVICE_EXIT_CODE) as last reported to the SCM.

    Either element is -1 when the field is missing from the `sc query` output.
    """
    out = sc("query", name).stdout
    win32 = re.search(r"WIN32_EXIT_CODE\s*:\s*(\d+)", out)
    specific = re.search(r"SERVICE_EXIT_CODE\s*:\s*(\d+)", out)
    return (int(win32.group(1)) if win32 else -1,
            int(specific.group(1)) if specific else -1)


def event_source_registered(source: str) -> bool:
    """True if `source` has an Event Log registration under the Application log."""
    result = subprocess.run(
        ["reg.exe", "query", f"{EVENTLOG_KEY}\\{source}"],
        capture_output=True,
        text=True,
        check=False,
    )
    return result.returncode == 0


def event_log_messages(source: str, seconds: int = 300) -> list[str]:
    """Return Application-log messages from `source` written in the last `seconds`.

    Each message is flattened to a single line so callers can substring-match it.
    Returns an empty list when the source has logged nothing (Get-WinEvent treats
    an empty result as an error, which SilentlyContinue swallows).
    """
    script = (
        "$ErrorActionPreference = 'SilentlyContinue'; "
        f"$since = (Get-Date).AddSeconds(-{seconds}); "
        "Get-WinEvent -FilterHashtable @{LogName='Application'; "
        f"ProviderName='{source}'; StartTime=$since}} "
        "| ForEach-Object { $_.Message -replace '\\s+', ' ' }"
    )
    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
        capture_output=True,
        text=True,
        check=False,
    )
    return [line for line in result.stdout.splitlines() if line.strip()]


def check_recovery_actions(name: str) -> None:
    """Verify `name` is installed with two 5 s restarts and a 300 s reset period."""
    out = sc("qfailure", name).stdout

    reset = re.search(r"RESET_PERIOD \(in seconds\)\s*:\s*(\d+)", out)
    check(reset is not None and int(reset.group(1)) == EXPECTED_RESET_PERIOD,
          f"{name}: failure counter resets after {EXPECTED_RESET_PERIOD} s")

    delays = re.findall(r"RESTART -- Delay = (\d+) milliseconds", out)
    check(delays == EXPECTED_RESTART_DELAYS,
          f"{name}: restart actions {EXPECTED_RESTART_DELAYS} ms (got {delays})")

    # Without this flag the SCM would act only on an outright crash, never on the
    # non-zero exit code the services report when they fail to start.
    flag = re.search(r"FAILURE_ACTIONS_ON_NONCRASH_FAILURES\s*:\s*(\w+)",
                     sc("qfailureflag", name).stdout)
    check(flag is not None and flag.group(1).upper() == "TRUE",
          f"{name}: recovery actions also fire on non-crash failures")


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
    check(event_source_registered(SERVICE_NAME),
          f"Event Log source '{SERVICE_NAME}' registered")
    check_recovery_actions(SERVICE_NAME)

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
    check(event_source_registered(WEB_SERVICE),
          f"Event Log source '{WEB_SERVICE}' registered")
    check_recovery_actions(WEB_SERVICE)

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


# ─── Test 3: SCM recovery after a crash ───────────────────────────────────────

def test_recovery_restart() -> None:
    print("\n=== Test 3: SCM restarts the service after a crash ===")

    if not wait_service_running():
        check(False, "Service running before the crash test")
        return

    old_pid = service_pid()
    check(old_pid != 0, "Service PID readable from `sc queryex`")

    subprocess.run(
        ["taskkill.exe", "/F", "/PID", str(old_pid)],
        capture_output=True,
        text=True,
        check=False,
    )

    # The first failure action restarts after 5 s; allow ample slack for a busy
    # CI runner. A new PID is what distinguishes a restart from a stale query.
    restarted = False
    for _ in range(40):
        if service_state() == "RUNNING" and service_pid() not in (0, old_pid):
            restarted = True
            break
        time.sleep(1)
    check(restarted, "SCM restarted the service after the process was killed")


# ─── Test 4: Startup failure is reported, not hidden ──────────────────────────

def check_failed_start(name: str) -> None:
    """Start `name`, expecting it to fail and to say so to the SCM."""
    result = sc("start", name)
    check(result.returncode != 0,
          f"`sc start {name}` fails instead of reporting success")

    win32, specific = service_exit_codes(name)
    check(win32 == ERROR_SERVICE_SPECIFIC_ERROR,
          f"{name}: WIN32_EXIT_CODE is {ERROR_SERVICE_SPECIFIC_ERROR} (got {win32})")
    check(specific != 0,
          f"{name}: SERVICE_EXIT_CODE is non-zero (got {specific})")


def test_failed_start() -> None:
    print("\n=== Test 4: Startup failure is reported, not hidden ===")

    check(stop_service(SERVICE_NAME), f"'{SERVICE_NAME}' stopped before the test")
    check(stop_service(WEB_SERVICE), f"'{WEB_SERVICE}' stopped before the test")

    # Both services are installed against the same config file, and neither can
    # start without it.
    CONFIG_PATH.replace(CONFIG_BACKUP)
    try:
        check_failed_start(SERVICE_NAME)
        check_failed_start(WEB_SERVICE)

        # Reporting failure to the SCM says *that* the viewer failed; the Event
        # Log is the only place a service with no console says *why*.
        messages = event_log_messages(WEB_SERVICE)
        check(any("config" in m.lower() for m in messages),
              f"'{WEB_SERVICE}' named the config as the cause in the Event Log")

        # Exhaust the queued restart attempts while the config is still missing,
        # so none of them can succeed behind the next case once it is restored.
        check(wait_service_settled(SERVICE_NAME),
              f"'{SERVICE_NAME}' stays stopped once its restarts are used up")
        check(wait_service_settled(WEB_SERVICE),
              f"'{WEB_SERVICE}' stays stopped once its restarts are used up")
    finally:
        CONFIG_BACKUP.replace(CONFIG_PATH)

    # An unbindable listen address must fail the same way as a bad config.
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("0.0.0.0", WEB_VIEWER_PORT))
        sock.listen(1)
        check_failed_start(WEB_SERVICE)

        messages = event_log_messages(WEB_SERVICE)
        check(any(str(WEB_VIEWER_PORT) in m for m in messages),
              f"'{WEB_SERVICE}' named the unbindable address in the Event Log")

        check(wait_service_settled(WEB_SERVICE),
              f"'{WEB_SERVICE}' stays stopped while the port is held")

    # Leave both services as the upgrade test expects to find them.
    sc("start", SERVICE_NAME)
    sc("start", WEB_SERVICE)
    check(wait_service_running(SERVICE_NAME),
          f"'{SERVICE_NAME}' starts again once the config is back")
    check(wait_service_running(WEB_SERVICE),
          f"'{WEB_SERVICE}' starts again once the port is free")


# ─── Test 5: Upgrade install (config not overwritten) ─────────────────────────

def test_upgrade(installer: Path) -> None:
    print("\n=== Test 5: Upgrade install ===")

    sentinel = f"; MODIFIED-BY-INSTALLER-TEST-{random.randint(100000, 999999)}"
    with CONFIG_PATH.open("a", encoding="utf-8") as f:
        f.write(f"\n{sentinel}\n")

    run_installer(installer)
    check(wait_service_running(), "Service running after upgrade")
    check(wait_service_running(WEB_SERVICE), "Web viewer service running after upgrade")

    content = CONFIG_PATH.read_text(encoding="utf-8")
    check(sentinel in content, "Config not overwritten on upgrade")


# ─── Test 6: Uninstall ────────────────────────────────────────────────────────

def test_uninstall() -> None:
    print("\n=== Test 6: Uninstall ===")

    run_uninstaller()

    check(not service_exists(),     "Service removed after uninstall")
    check(not service_exists(WEB_SERVICE), "Web viewer service removed after uninstall")
    check(not event_source_registered(SERVICE_NAME),
          f"Event Log source '{SERVICE_NAME}' removed after uninstall")
    check(not event_source_registered(WEB_SERVICE),
          f"Event Log source '{WEB_SERVICE}' removed after uninstall")
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
    test_recovery_restart()
    test_failed_start()
    test_upgrade(args.installer)
    test_uninstall()

    color = "\033[92m" if failed == 0 else "\033[91m"
    reset = "\033[0m"
    print(f"\n{color}=== Results: {passed} passed, {failed} failed ==={reset}")
    if failed > 0:
        sys.exit(1)


if __name__ == "__main__":
    main()
