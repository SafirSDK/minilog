"""Smoke-test for the zip archive that installs minilog without the installer.

Does what the README's "Windows deployment without the installer" section tells
an administrator to do — on paths the installer never uses, with the config
edited to keep its logs somewhere else again — and checks that the result is a
working pair of services:

  1. Contents  — the archive holds exactly the files the README lists
  2. Install   — unpack, copy the files to their own directories, point the
                 config at a separate log directory, --check, --install both,
                 start both
  3. Smoke     — a datagram lands in the log; the web viewer answers; the CLI
                 viewer finds the config it is given
  4. Remove    — --stop and --uninstall leave nothing registered, and the
                 files can be deleted

Must be run as Administrator (--install registers a Windows service). Run it
after test_installer.py: both register the same two service names, and this one
expects to find them unregistered.

Usage:
    python test_zip_install.py <path-to-minilog-*-win64.zip>
"""

import argparse
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path

import test_installer as ti
from test_installer import (
    check,
    check_recovery_actions,
    event_source_registered,
    net_start,
    service_binary_path,
    service_cmd,
    service_exists,
    service_start_type,
    wait_service_absent,
    wait_service_running,
    web_viewer_responds,
)

# ─── Paths ────────────────────────────────────────────────────────────────────
#
# Three directories, none under Program Files or ProgramData: the point of the
# archive is that the executables, the config and the logs can each live where
# the deployment says, and a test that used the installer's locations would not
# show that.

ROOT = Path(os.environ.get("SystemDrive", "C:") + "\\") / "minilog-zip-test"
BIN_DIR = ROOT / "bin"
ETC_DIR = ROOT / "etc"
LOG_DIR = ROOT / "var" / "logs"
EXE_PATH = BIN_DIR / "minilog.exe"
WEB_EXE = BIN_DIR / "minilog-web-viewer.exe"
SEND_EXE = BIN_DIR / "minilog-send.exe"
VIEWER_PY = BIN_DIR / "minilog-cli-viewer.py"
CONFIG = ETC_DIR / "minilog.conf"
LOG_FILE = LOG_DIR / "syslog.log"

# What the archive contains, and nothing else. Kept in step with
# cmake/package_zip.cmake and the README's table by hand; this test is what
# notices when one of the three drifts.
EXPECTED_FILES = {
    "minilog.exe",
    "minilog.pdb",
    "minilog-web-viewer.exe",
    "minilog-send.exe",
    "minilog.conf",
    "minilog-cli-viewer.py",
    "minilog-cli-viewer.conf",
    "LICENSE",
    "README.md",
    "CHANGES.md",
}


def archive_version(archive: Path) -> str:
    m = re.fullmatch(r"minilog-(.+)-win64\.zip", archive.name)
    if not m:
        sys.exit(f"Archive name is not minilog-<version>-win64.zip: {archive.name}")
    return m.group(1)


def retarget_logs(config: Path, log_dir: Path) -> None:
    """Point every text_file and jsonl_file in `config` at `log_dir`.

    The shipped config names C:\\ProgramData\\minilog\\logs. Only the directory
    is replaced — the file names and everything else in the file stay as they
    are, which is what an administrator editing the file would do.
    """
    out = []
    for line in config.read_text(encoding="utf-8").splitlines():
        key = line.split("=", 1)[0].strip().lower() if "=" in line else ""
        if key in ("text_file", "jsonl_file"):
            old = Path(line.split("=", 1)[1].strip())
            line = f"{key:<16} = {log_dir / old.name}"
        out.append(line)
    config.write_text("\n".join(out) + "\n", encoding="utf-8")


def run_check() -> subprocess.CompletedProcess:
    return subprocess.run(
        [str(EXE_PATH), "--check", str(CONFIG)],
        capture_output=True,
        text=True,
        check=False,
    )


# ─── Test 1: contents ─────────────────────────────────────────────────────────


def test_contents(archive: Path) -> str:
    print("\n=== Test 1: Archive contents ===")
    version = archive_version(archive)
    top = f"minilog-{version}/"

    with zipfile.ZipFile(archive) as zf:
        names = [n for n in zf.namelist() if not n.endswith("/")]

    check(
        all(n.startswith(top) for n in names),
        f"Every entry is under {top} (got {sorted(names)[:3]}...)",
    )
    files = {n[len(top) :] for n in names if n.startswith(top)}
    check("/" not in "".join(files), "No subdirectories inside the archive")
    check(
        files == EXPECTED_FILES,
        f"Archive holds exactly the documented files "
        f"(missing {sorted(EXPECTED_FILES - files)}, extra {sorted(files - EXPECTED_FILES)})",
    )
    return top


# ─── Test 2: install ──────────────────────────────────────────────────────────


def test_install(archive: Path, top: str) -> None:
    print("\n=== Test 2: Install from the archive ===")

    check(not service_exists(ti.SERVICE_NAME), f"'{ti.SERVICE_NAME}' not registered beforehand")
    check(not service_exists(ti.WEB_SERVICE), f"'{ti.WEB_SERVICE}' not registered beforehand")

    if ROOT.exists():
        shutil.rmtree(ROOT)
    BIN_DIR.mkdir(parents=True)
    ETC_DIR.mkdir(parents=True)

    with tempfile.TemporaryDirectory() as tmp:
        with zipfile.ZipFile(archive) as zf:
            zf.extractall(tmp)
        src = Path(tmp) / top.rstrip("/")
        for name in (
            "minilog.exe",
            "minilog.pdb",
            "minilog-web-viewer.exe",
            "minilog-send.exe",
            "minilog-cli-viewer.py",
        ):
            shutil.copy2(src / name, BIN_DIR / name)
        for name in ("minilog.conf", "minilog-cli-viewer.conf"):
            shutil.copy2(src / name, ETC_DIR / name)

    retarget_logs(CONFIG, LOG_DIR)
    check(
        str(LOG_DIR) in CONFIG.read_text(encoding="utf-8"),
        f"Config rewritten to log under {LOG_DIR}",
    )

    # The log directory does not exist yet, and minilog never creates one.
    # --check is what tells an administrator that before the service is
    # registered and fails silently into a dead sink.
    result = run_check()
    check(result.returncode != 0, "--check fails while the log directory is missing")
    check(
        "does not exist" in result.stdout,
        f"--check says the directory is missing (stdout: {result.stdout.strip()[:200]})",
    )

    LOG_DIR.mkdir(parents=True)
    result = run_check()
    check(
        result.returncode == 0,
        f"--check passes once the directory exists (stdout: {result.stdout.strip()[:200]})",
    )

    result = service_cmd(EXE_PATH, "--install", str(CONFIG))
    check(result.returncode == 0, f"`minilog --install` succeeds (stderr: {result.stderr.strip()})")
    result = service_cmd(WEB_EXE, "--install", "--config", str(CONFIG))
    check(
        result.returncode == 0,
        f"`minilog-web-viewer --install` succeeds (stderr: {result.stderr.strip()})",
    )

    check(
        service_binary_path(ti.SERVICE_NAME) == f'"{EXE_PATH}" "{CONFIG}"',
        f"'{ti.SERVICE_NAME}' registered from {BIN_DIR} with the config in {ETC_DIR} "
        f"(got {service_binary_path(ti.SERVICE_NAME)})",
    )
    web_cmdline = service_binary_path(ti.WEB_SERVICE)
    check(
        str(WEB_EXE) in web_cmdline and str(CONFIG) in web_cmdline,
        f"'{ti.WEB_SERVICE}' registered from {BIN_DIR} with the config in {ETC_DIR} "
        f"(got {web_cmdline})",
    )
    check(service_start_type(ti.SERVICE_NAME) == "AUTO_START", f"'{ti.SERVICE_NAME}' is AUTO_START")
    check(
        event_source_registered(ti.SERVICE_NAME), f"Event Log source '{ti.SERVICE_NAME}' registered"
    )
    check(
        event_source_registered(ti.WEB_SERVICE), f"Event Log source '{ti.WEB_SERVICE}' registered"
    )
    check_recovery_actions(ti.SERVICE_NAME)
    check_recovery_actions(ti.WEB_SERVICE)

    check(net_start(ti.SERVICE_NAME).returncode == 0, f"`net start {ti.SERVICE_NAME}` succeeds")
    check(net_start(ti.WEB_SERVICE).returncode == 0, f"`net start {ti.WEB_SERVICE}` succeeds")
    check(wait_service_running(ti.SERVICE_NAME), f"'{ti.SERVICE_NAME}' is Running")
    check(wait_service_running(ti.WEB_SERVICE), f"'{ti.WEB_SERVICE}' is Running")


# ─── Test 3: smoke ────────────────────────────────────────────────────────────


def test_smoke() -> None:
    print("\n=== Test 3: Services work from where they were put ===")

    # Sent with the shipped sender rather than a raw socket: this is the step the
    # README tells an administrator to do, with the tool it tells them to use.
    marker = f"zip-test-{random.randint(100000, 999999)}"
    result = subprocess.run(
        [str(SEND_EXE), "--port", "514", "--app", "minilog-ci", marker],
        capture_output=True,
        text=True,
        check=False,
        timeout=15,
    )
    check(
        result.returncode == 0,
        f"`minilog-send.exe --port 514 ... {marker}` exits 0 (stderr: {result.stderr.strip()})",
    )
    check(result.stdout == "", "minilog-send.exe prints nothing on success")
    time.sleep(2)

    check(LOG_FILE.exists(), f"syslog.log written under {LOG_DIR}")
    if LOG_FILE.exists():
        text = LOG_FILE.read_text(encoding="utf-8", errors="replace")
        check(marker in text, "Sent message appears in syslog.log")
        check("minilog-ci" in text, "Sent message carries the --app given to minilog-send.exe")
    check(
        not (ti.LOG_DIR / "syslog.log").exists()
        or marker not in (ti.LOG_DIR / "syslog.log").read_text(encoding="utf-8", errors="replace"),
        "Nothing was written to the installer's default log directory",
    )

    check(web_viewer_responds(), "Web viewer /sinks endpoint returns a JSON array")

    # The CLI viewer is a script with a search order of its own; --config is how
    # a deployment that keeps the config elsewhere points it at the right file.
    result = subprocess.run(
        ["python", str(VIEWER_PY), "--config", str(CONFIG), "--show-all", "--no-color"],
        capture_output=True,
        text=True,
        check=False,
        timeout=15,
    )
    check(
        result.returncode == 0,
        f"`minilog-cli-viewer.py --config {CONFIG} --show-all` succeeds "
        f"(stderr: {result.stderr.strip()})",
    )
    check(marker in result.stdout, "CLI viewer shows the message from the redirected log")


# ─── Test 4: remove ───────────────────────────────────────────────────────────


def test_remove() -> None:
    print("\n=== Test 4: Remove ===")

    check(service_cmd(EXE_PATH, "--stop").returncode == 0, "`minilog --stop` succeeds")
    check(service_cmd(WEB_EXE, "--stop").returncode == 0, "`minilog-web-viewer --stop` succeeds")
    check(
        service_cmd(WEB_EXE, "--uninstall").returncode == 0,
        "`minilog-web-viewer --uninstall` succeeds",
    )
    check(service_cmd(EXE_PATH, "--uninstall").returncode == 0, "`minilog --uninstall` succeeds")

    check(wait_service_absent(ti.SERVICE_NAME), f"'{ti.SERVICE_NAME}' gone from the SCM")
    check(wait_service_absent(ti.WEB_SERVICE), f"'{ti.WEB_SERVICE}' gone from the SCM")
    check(
        not event_source_registered(ti.SERVICE_NAME),
        f"Event Log source '{ti.SERVICE_NAME}' removed",
    )
    check(
        not event_source_registered(ti.WEB_SERVICE), f"Event Log source '{ti.WEB_SERVICE}' removed"
    )

    # --stop waits for the processes to exit, which is what makes this possible.
    try:
        shutil.rmtree(ROOT)
        check(True, f"{ROOT} deleted")
    except OSError as e:
        check(False, f"{ROOT} deleted ({e})")


# ─── Entry point ──────────────────────────────────────────────────────────────


def main() -> None:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path, help="Path to minilog-*-win64.zip")
    args = parser.parse_args()

    if not args.archive.exists():
        sys.exit(f"Archive not found: {args.archive}")

    top = test_contents(args.archive)
    test_install(args.archive, top)
    test_smoke()
    test_remove()

    color = "\033[92m" if ti.failed == 0 else "\033[91m"
    reset = "\033[0m"
    print(f"\n{color}=== Results: {ti.passed} passed, {ti.failed} failed ==={reset}")
    if ti.failed > 0:
        sys.exit(1)


if __name__ == "__main__":
    main()
