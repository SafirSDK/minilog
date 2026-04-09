#!/usr/bin/env python3
"""
Start the minilog-web-viewer demo with live log appending.

Usage: python3 run.py [--addr :8080] [--interval 2.5] [--burst 3]
                      [--viewer /path/to/minilog-web-viewer]

  --addr      Address for the web viewer to listen on  (default: :8080)
  --interval  Average seconds between appended batches (default: 2.5)
  --burst     Max entries per appended batch           (default: 3)
  --viewer    Explicit path to the minilog-web-viewer binary

The script searches for the binary in:
  1. The path given by --viewer (if provided)
  2. build/linux-debug/bin/    (cmake linux-debug preset)
  3. build/linux-release/bin/  (cmake linux-release preset)
  4. src/web-viewer/           (in-place 'go build' output)
"""

import argparse
import os
import subprocess
import sys
import threading

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.normpath(os.path.join(SCRIPT_DIR, "../../.."))

BINARY_NAME = "minilog-web-viewer"
# Search order: cmake linux-debug, cmake linux-release, in-place 'go build'
SEARCH_DIRS = [
    os.path.join(REPO_ROOT, "build", "linux-debug", "bin"),
    os.path.join(REPO_ROOT, "build", "linux-release", "bin"),
    os.path.normpath(os.path.join(SCRIPT_DIR, "..")),
]


def find_viewer(explicit: str | None) -> str:
    if explicit:
        if not os.path.isfile(explicit):
            sys.exit(f"error: --viewer path not found: {explicit}")
        return os.path.abspath(explicit)

    for d in SEARCH_DIRS:
        candidate = os.path.join(d, BINARY_NAME)
        if os.path.isfile(candidate):
            return os.path.abspath(candidate)

    searched = "\n  ".join(SEARCH_DIRS)
    sys.exit(
        f"error: '{BINARY_NAME}' not found in any of:\n  {searched}\n"
        f"Build with cmake --build build/linux-debug, "
        f"or pass --viewer /path/to/{BINARY_NAME}."
    )


def run_appender(interval: float, burst: int, stop_event: threading.Event) -> None:
    """Run append_data as a subprocess on a background thread."""
    proc = subprocess.Popen(
        [
            sys.executable,
            os.path.join(SCRIPT_DIR, "append_data.py"),
            "--interval",
            str(interval),
            "--burst",
            str(burst),
        ],
        stderr=sys.stderr,
    )
    stop_event.wait()
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--addr", default=":8080", help="listener address for the web viewer (default: :8080)"
    )
    parser.add_argument(
        "--interval",
        type=float,
        default=2.5,
        help="average seconds between appended batches (default: 2.5)",
    )
    parser.add_argument(
        "--burst", type=int, default=3, help="max entries per appended batch (default: 3)"
    )
    parser.add_argument(
        "--viewer", default=None, help="explicit path to the minilog-web-viewer binary"
    )
    args = parser.parse_args()

    viewer_bin = find_viewer(args.viewer)

    # (Re-)generate the initial data set.
    print("Generating test data...", flush=True)
    subprocess.run(
        [sys.executable, os.path.join(SCRIPT_DIR, "gen_data.py")],
        check=True,
    )

    # Start the appender on a background thread (runs as a subprocess internally).
    stop_event = threading.Event()
    appender_thread = threading.Thread(
        target=run_appender,
        args=(args.interval, args.burst, stop_event),
        daemon=True,
    )
    appender_thread.start()

    print(f"Starting {BINARY_NAME}...", flush=True)
    try:
        subprocess.run(
            [viewer_bin, "--config", os.path.join(SCRIPT_DIR, "minilog.conf"), "--addr", args.addr],
            check=False,
        )
    except KeyboardInterrupt:
        pass
    finally:
        stop_event.set()
        appender_thread.join(timeout=7)
        print("Stopped.")


if __name__ == "__main__":
    main()
