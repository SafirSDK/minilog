#!/usr/bin/env python3
"""Verify that every .cpp and .hpp file under src/ and tests/ contains the
expected copyright header.  Exits non-zero if any file is missing it."""

import sys
from pathlib import Path

EXPECTED = "Copyright Saab AB"
REPO_ROOT = Path(__file__).resolve().parent.parent

failed = False
for pattern in ("**/*.cpp", "**/*.hpp"):
    for path in sorted((REPO_ROOT / "src").glob(pattern)):
        header = path.read_text(encoding="utf-8", errors="replace")[:500]
        if EXPECTED not in header:
            print(f"MISSING HEADER: {path.relative_to(REPO_ROOT)}")
            failed = True
    for path in sorted((REPO_ROOT / "tests").glob(pattern)):
        header = path.read_text(encoding="utf-8", errors="replace")[:500]
        if EXPECTED not in header:
            print(f"MISSING HEADER: {path.relative_to(REPO_ROOT)}")
            failed = True

if not failed:
    print("OK: all files have the required header.")

sys.exit(1 if failed else 0)
