"""Smoke-tests for the minilog Inno Setup installer.

Test groups (run in sequence):
  1. Clean install   — files, directories, services registered + running,
                       recovery actions and Event Log sources configured
  2. Registration    — --install records the real image path and an absolute
                       config path, and refuses a config it cannot read
  3. UDP smoke       — send a syslog datagram, verify it lands in the log
  4. Recovery        — kill the service process; the SCM restarts it
  5. Failed start    — an unusable config makes `sc start` fail, reports a
                       non-zero exit code, and leaves an Event Log trail
  6. Stop            — --stop and --uninstall wait for the process, so the
                       executables can be overwritten and re-registered
  7. Re-register    — --install over an existing service updates it and leaves
                       the administrator's start type and account alone
  8. Upgrade install — service survives; user-modified config is not overwritten
  9. Uninstall       — services and binaries removed; config file survives

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

# TEST-NET-1 (RFC 5737) — never assigned to a host, so binding it always fails.
# More deterministic than contending for a live port, which Windows may allow.
UNBINDABLE_ADDR = f"192.0.2.1:{WEB_VIEWER_PORT}"

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


def service_binary_path(name: str = SERVICE_NAME) -> str:
    """Return the command line the SCM will run for `name`, or '' if not found."""
    result = sc("qc", name)
    m = re.search(r"BINARY_PATH_NAME\s*:\s*(.*)", result.stdout)
    return m.group(1).strip() if m else ""


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


def wait_service_absent(name: str = SERVICE_NAME, timeout: int = 30) -> bool:
    """Wait until `name` is gone from the SCM database.

    DeleteService only *marks* a running service for deletion, and the
    registration lingers until the last handle to it closes, so the removal is
    not necessarily visible the instant --uninstall returns.
    """
    for _ in range(timeout):
        if not service_exists(name):
            return True
        time.sleep(1)
    return False


def stop_service(name: str = SERVICE_NAME) -> bool:
    """Stop the service and wait for it to reach STOPPED (already-stopped is fine)."""
    sc("stop", name)
    return wait_service_stopped(name)


def net_start(name: str) -> subprocess.CompletedProcess:
    """Start a service with net.exe, which waits for the outcome.

    `sc start` returns as soon as StartService succeeds, and StartService
    succeeds the moment the service reports SERVICE_START_PENDING — so its exit
    code says nothing about whether startup then went on to succeed. net.exe
    waits for the service to reach RUNNING or fail, so its exit code does.
    """
    return subprocess.run(
        ["net.exe", "start", name],
        capture_output=True,
        text=True,
        check=False,
    )


def reinstall_web_viewer(addr: str) -> None:
    """Re-register the web viewer service against a different listen address."""
    for args in (["--uninstall"], ["--install", "--config", str(CONFIG_PATH), "--addr", addr]):
        subprocess.run(
            [str(WEB_VIEWER_EXE), *args],
            capture_output=True,
            text=True,
            check=False,
        )
        # Give the SCM a moment to finish the deletion; creating a service that
        # is still marked for delete fails with ERROR_SERVICE_MARKED_FOR_DELETE.
        time.sleep(1)


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


def event_source_message_file(source: str) -> str:
    """Return the EventMessageFile recorded for `source`, or '' if unset."""
    result = subprocess.run(
        ["reg.exe", "query", f"{EVENTLOG_KEY}\\{source}", "/v", "EventMessageFile"],
        capture_output=True,
        text=True,
        check=False,
    )
    m = re.search(r"EventMessageFile\s+REG_\w+\s+(.+)", result.stdout)
    return m.group(1).strip() if m else ""


def service_account(name: str = SERVICE_NAME) -> str:
    """Return the account the service runs as, e.g. 'LocalSystem'."""
    m = re.search(r"SERVICE_START_NAME\s*:\s*(.+)", sc("qc", name).stdout)
    return m.group(1).strip() if m else ""


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


# ─── Test 2: --install records paths the SCM can actually use ─────────────────

def install_server(*args: str, cwd: Path | None = None,
                   exe: str | None = None) -> subprocess.CompletedProcess:
    """Run `minilog --install`, optionally by bare name through PATH.

    Passing exe="minilog" leaves argv[0] as the unqualified name, which is how
    the executable is invoked once its directory is on PATH — the case that used
    to register a BINARY_PATH_NAME pointing at a file that does not exist.
    """
    # CreateProcess resolves an unqualified name against the PATH of the calling
    # process, not against any environment block handed to the child, so this
    # has to go into our own environment.
    if str(APP_DIR).lower() not in os.environ["PATH"].lower():
        os.environ["PATH"] = f"{APP_DIR};{os.environ['PATH']}"
    return subprocess.run(
        [exe or str(EXE_PATH), "--install", *args],
        capture_output=True,
        text=True,
        check=False,
        cwd=str(cwd) if cwd else None,
    )


def remove_server_service() -> None:
    """Deregister the syslog service and wait until the SCM agrees it is gone."""
    stop_service(SERVICE_NAME)
    subprocess.run([str(EXE_PATH), "--uninstall"], capture_output=True, text=True, check=False)
    wait_service_absent(SERVICE_NAME)


def test_install_paths() -> None:
    print("\n=== Test 2: --install records usable paths ===")

    expected = f'"{EXE_PATH}" "{CONFIG_PATH}"'
    check(service_binary_path() == expected,
          f"Installer registered {expected} (got {service_binary_path()})")

    # Re-register the way an administrator would once {app} is on PATH: bare
    # executable name, relative config path, working directory somewhere else.
    remove_server_service()
    result = install_server("minilog.conf", cwd=DATA_DIR, exe="minilog")
    check(result.returncode == 0,
          f"`minilog --install minilog.conf` via PATH succeeds (stderr: {result.stderr.strip()})")
    check(service_binary_path() == expected,
          "Service registered with the real image path and an absolute config path "
          f"(got {service_binary_path()})")
    check(service_start_type() == "AUTO_START", "Re-registered service is AUTO_START")

    # The point of recording those paths correctly: the service can start from
    # them, with the System32 working directory the SCM gives it.
    check(net_start(SERVICE_NAME).returncode == 0,
          "Service started from the registered paths")
    check(wait_service_running(), "Service is Running again after re-registration")

    # A config that cannot be read would produce a service that fails at every
    # boot, so --install must refuse it and leave nothing behind.
    remove_server_service()
    result = install_server(str(DATA_DIR / "no-such-file.conf"))
    check(result.returncode != 0, "--install rejects a config file it cannot read")
    check("no-such-file.conf" in result.stderr,
          f"--install names the unreadable config (stderr: {result.stderr.strip()})")
    check(not service_exists(), "Nothing is registered after a rejected --install")

    # Leave the service as the installer left it, for the tests that follow.
    check(install_server(str(CONFIG_PATH)).returncode == 0, "Service re-registered")
    sc("start", SERVICE_NAME)
    check(wait_service_running(), "Service running again after the registration tests")


# ─── Test 3: UDP smoke test ───────────────────────────────────────────────────

def test_udp_smoke() -> None:
    print("\n=== Test 3: UDP smoke test ===")

    marker = f"installer-test-{random.randint(100000, 999999)}"
    msg = f"<13>Mar 15 10:00:00 testhost minilog-ci: {marker}"

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.sendto(msg.encode("ascii"), ("127.0.0.1", 514))

    time.sleep(2)

    check(LOG_FILE.exists(), f"syslog.log created at {LOG_FILE}")
    if LOG_FILE.exists():
        content = LOG_FILE.read_text(encoding="utf-8", errors="replace")
        check(marker in content, "Sent message appears in syslog.log")


# ─── Test 4: SCM recovery after a crash ───────────────────────────────────────

def test_recovery_restart() -> None:
    print("\n=== Test 4: SCM restarts the service after a crash ===")

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


# ─── Test 5: Startup failure is reported, not hidden ──────────────────────────

def check_failed_start(name: str) -> None:
    """Start `name`, expecting it to fail and to say so to the SCM."""
    result = net_start(name)
    check(result.returncode != 0,
          f"`net start {name}` fails instead of reporting success")

    # `sc query` reports the last status, so read it only once the service has
    # reached STOPPED — a start still pending reports an exit code of 0.
    check(wait_service_stopped(name), f"{name}: reaches STOPPED after a failed start")

    win32, specific = service_exit_codes(name)
    check(win32 == ERROR_SERVICE_SPECIFIC_ERROR,
          f"{name}: WIN32_EXIT_CODE is {ERROR_SERVICE_SPECIFIC_ERROR} (got {win32})")
    check(specific != 0,
          f"{name}: SERVICE_EXIT_CODE is non-zero (got {specific})")


def test_failed_start() -> None:
    print("\n=== Test 5: Startup failure is reported, not hidden ===")

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
    reinstall_web_viewer(UNBINDABLE_ADDR)
    try:
        check_failed_start(WEB_SERVICE)

        messages = event_log_messages(WEB_SERVICE)
        check(any(UNBINDABLE_ADDR in m for m in messages),
              f"'{WEB_SERVICE}' named the unbindable address in the Event Log")

        check(wait_service_settled(WEB_SERVICE),
              f"'{WEB_SERVICE}' stays stopped while its address is unbindable")
    finally:
        reinstall_web_viewer(f":{WEB_VIEWER_PORT}")

    # Leave both services as the upgrade test expects to find them.
    sc("start", SERVICE_NAME)
    sc("start", WEB_SERVICE)
    check(wait_service_running(SERVICE_NAME),
          f"'{SERVICE_NAME}' starts again once the config is back")
    check(wait_service_running(WEB_SERVICE),
          f"'{WEB_SERVICE}' starts again once its address is bindable")


# ─── Test 6: --stop and --uninstall wait for the process ──────────────────────

def service_cmd(exe: Path, *args: str) -> subprocess.CompletedProcess:
    """Run one of the executables' service subcommands."""
    return subprocess.run(
        [str(exe), *args],
        capture_output=True,
        text=True,
        check=False,
    )


def process_alive(pid: int) -> bool:
    out = subprocess.run(
        ["tasklist.exe", "/FI", f"PID eq {pid}", "/NH"],
        capture_output=True,
        text=True,
        check=False,
    ).stdout
    return str(pid) in out


def can_overwrite(path: Path) -> bool:
    """Rewrite a file with its own bytes.

    This is the point of --stop: the SCM reporting STOPPED says nothing about
    whether the process has exited, and a running image cannot be opened for
    writing — which is exactly what an upgrade does next.  The contents are
    unchanged, so a success leaves the installation as it was.
    """
    try:
        data = path.read_bytes()
        with path.open("r+b") as f:
            f.write(data)
        return True
    except OSError as e:
        print(f"    ({path.name}: {e})")
        return False


def check_stop_releases_the_image(exe: Path, name: str) -> None:
    if not wait_service_running(name):
        check(False, f"'{name}' running before the stop test")
        return

    pid = service_pid(name)
    result = service_cmd(exe, "--stop")
    check(result.returncode == 0,
          f"`{exe.name} --stop` succeeds (stderr: {result.stderr.strip()})")
    check(service_state(name) == "STOPPED", f"'{name}' is STOPPED as soon as --stop returns")
    check(pid != 0 and not process_alive(pid),
          f"'{name}' process {pid} has exited as soon as --stop returns")
    check(can_overwrite(exe), f"{exe.name} can be overwritten once --stop has returned")

    check(service_cmd(exe, "--stop").returncode == 0,
          f"`{exe.name} --stop` on an already stopped service succeeds")


def test_stop_waits() -> None:
    print("\n=== Test 6: --stop and --uninstall wait for the process ===")

    check_stop_releases_the_image(EXE_PATH, SERVICE_NAME)
    check_stop_releases_the_image(WEB_VIEWER_EXE, WEB_SERVICE)

    check(net_start(WEB_SERVICE).returncode == 0, f"'{WEB_SERVICE}' starts again after --stop")

    # --uninstall of a *running* service must delete it outright.  Deleting one
    # that is still running only marks it for deletion, which shows up as a
    # registration that lingers and a re-register that fails with 1072.
    check(net_start(SERVICE_NAME).returncode == 0, f"'{SERVICE_NAME}' starts again after --stop")
    pid = service_pid()
    result = service_cmd(EXE_PATH, "--uninstall")
    check(result.returncode == 0,
          f"`minilog --uninstall` of a running service succeeds (stderr: {result.stderr.strip()})")
    check(pid != 0 and not process_alive(pid),
          f"Service process {pid} has exited as soon as --uninstall returns")
    check(not service_exists(), "Service is gone as soon as --uninstall returns")

    check(service_cmd(EXE_PATH, "--stop").returncode == 0,
          "`minilog --stop` against an unregistered service succeeds")

    # The proof that the deletion was real rather than pending: re-registering
    # immediately would fail with ERROR_SERVICE_MARKED_FOR_DELETE otherwise.
    result = install_server(str(CONFIG_PATH))
    check(result.returncode == 0,
          f"--install straight after --uninstall succeeds (stderr: {result.stderr.strip()})")
    check(net_start(SERVICE_NAME).returncode == 0, "Service starts again after re-registration")


# ─── Test 7: --install over an existing registration updates it ───────────────

def test_reregister() -> None:
    print("\n=== Test 7: --install updates an existing registration ===")

    # Idempotence first: nothing about the second run may fail.
    check(service_exists(), "Service registered before the re-registration test")
    first = install_server(str(CONFIG_PATH))
    second = install_server(str(CONFIG_PATH))
    check(first.returncode == 0 and second.returncode == 0,
          "--install run twice in succession succeeds both times")
    check("updated" in (second.stdout + second.stderr).lower(),
          f"--install reports an update rather than an install "
          f"(stderr: {second.stderr.strip()})")

    # What an administrator may have changed by hand must survive an upgrade.
    # LocalService is a built-in account with no password, so this is a change
    # the SCM accepts without credentials.
    sc("config", SERVICE_NAME, "start=", "demand")
    sc("config", SERVICE_NAME, "obj=", "NT AUTHORITY\\LocalService")
    check(service_start_type() == "DEMAND_START", "Start type set to manual for the test")
    check(service_account() == "NT AUTHORITY\\LocalService",
          "Account set to LocalService for the test")

    result = install_server(str(CONFIG_PATH))
    check(result.returncode == 0,
          f"--install over a hand-configured service succeeds (stderr: {result.stderr.strip()})")
    check(service_start_type() == "DEMAND_START", "Manual start type survives --install")
    check(service_account() == "NT AUTHORITY\\LocalService", "Service account survives --install")
    check(service_binary_path() == f'"{EXE_PATH}" "{CONFIG_PATH}"',
          f"Binary path is still updated (got {service_binary_path()})")
    check_recovery_actions(SERVICE_NAME)

    # An upgrade that moves the executable must leave neither the registration
    # nor the Event Log message file pointing at the old location.
    moved_dir = Path(os.environ["TEMP"]) / "minilog-moved"
    moved_dir.mkdir(parents=True, exist_ok=True)
    moved_exe = moved_dir / "minilog.exe"
    moved_exe.write_bytes(EXE_PATH.read_bytes())
    try:
        result = install_server(str(CONFIG_PATH), exe=str(moved_exe))
        check(result.returncode == 0,
              f"--install from a moved executable succeeds (stderr: {result.stderr.strip()})")
        check(service_binary_path() == f'"{moved_exe}" "{CONFIG_PATH}"',
              f"Registration points at the moved executable (got {service_binary_path()})")
        check(event_source_message_file(SERVICE_NAME) == str(moved_exe),
              "Event Log source points at the moved executable "
              f"(got {event_source_message_file(SERVICE_NAME)})")
    finally:
        # Put the installed executable back in charge before restoring the
        # service to the state the later tests expect.
        install_server(str(CONFIG_PATH))
        moved_exe.unlink(missing_ok=True)

    check(event_source_message_file(SERVICE_NAME) == str(EXE_PATH),
          "Event Log source points at the installed executable again")

    sc("config", SERVICE_NAME, "start=", "auto")
    sc("config", SERVICE_NAME, "obj=", "LocalSystem")
    check(service_start_type() == "AUTO_START", "Start type restored to AUTO_START")

    # --uninstall against a service that is not there is the state it asks for.
    check(service_cmd(WEB_VIEWER_EXE, "--stop").returncode == 0,
          f"'{WEB_SERVICE}' stopped before deregistering it")
    check(service_cmd(WEB_VIEWER_EXE, "--uninstall").returncode == 0,
          f"`minilog-web-viewer --uninstall` removes '{WEB_SERVICE}'")
    check(service_cmd(WEB_VIEWER_EXE, "--uninstall").returncode == 0,
          "`minilog-web-viewer --uninstall` against an absent service succeeds")
    check(service_cmd(EXE_PATH, "--uninstall").returncode == 0, f"'{SERVICE_NAME}' deregistered")
    check(service_cmd(EXE_PATH, "--uninstall").returncode == 0,
          "`minilog --uninstall` against an absent service succeeds")

    # Leave both services as the upgrade test expects to find them.
    check(install_server(str(CONFIG_PATH)).returncode == 0, f"'{SERVICE_NAME}' re-registered")
    check(service_cmd(WEB_VIEWER_EXE, "--install", "--config", str(CONFIG_PATH),
                      "--addr", f":{WEB_VIEWER_PORT}").returncode == 0,
          f"'{WEB_SERVICE}' re-registered")
    sc("start", SERVICE_NAME)
    sc("start", WEB_SERVICE)
    check(wait_service_running(SERVICE_NAME), f"'{SERVICE_NAME}' running again")
    check(wait_service_running(WEB_SERVICE), f"'{WEB_SERVICE}' running again")


# ─── Test 8: Upgrade install (config not overwritten) ─────────────────────────

def test_upgrade(installer: Path) -> None:
    print("\n=== Test 8: Upgrade install ===")

    sentinel = f"; MODIFIED-BY-INSTALLER-TEST-{random.randint(100000, 999999)}"
    with CONFIG_PATH.open("a", encoding="utf-8") as f:
        f.write(f"\n{sentinel}\n")

    run_installer(installer)
    check(wait_service_running(), "Service running after upgrade")
    check(wait_service_running(WEB_SERVICE), "Web viewer service running after upgrade")

    content = CONFIG_PATH.read_text(encoding="utf-8")
    check(sentinel in content, "Config not overwritten on upgrade")


# ─── Test 9: Uninstall ────────────────────────────────────────────────────────

def test_uninstall() -> None:
    print("\n=== Test 9: Uninstall ===")

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
    test_install_paths()
    test_udp_smoke()
    test_recovery_restart()
    test_failed_start()
    test_stop_waits()
    test_reregister()
    test_upgrade(args.installer)
    test_uninstall()

    color = "\033[92m" if failed == 0 else "\033[91m"
    reset = "\033[0m"
    print(f"\n{color}=== Results: {passed} passed, {failed} failed ==={reset}")
    if failed > 0:
        sys.exit(1)


if __name__ == "__main__":
    main()
