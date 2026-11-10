#!/usr/bin/env python3
"""Normalise OpenCppCoverage's Cobertura output and print a per-file summary.

Two jobs, because both need the same parse:

1. Rewrite every `filename` to a path relative to the repository root, with
   forward slashes. OpenCppCoverage writes the absolute build-machine path
   (`D:\\a\\minilog\\minilog\\src\\server\\main.cpp`), which Codecov cannot
   match against the repository, so the Windows report would not line up with
   the Linux one for the files both measure.

2. Print the same table the Linux coverage job prints with awk, so that the
   log of a run shows which file is uncovered without opening Codecov.

Usage: summarise_cobertura.py <report.xml> [repo-root]
"""

from __future__ import annotations

import os
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def relative_path(filename: str, sources: list[str], root: str) -> str | None:
    """Return filename relative to root, forward-slashed, or None if outside it.

    OpenCppCoverage writes `filename` relative to one of the <source> roots
    rather than absolute — `a/minilog/minilog/src/server/main.cpp` under a
    source of `D:\\` — so each candidate root is tried before the repository
    prefix is stripped. None means the file is not ours: the UCRT arrives as
    `minkernel/crts/ucrt/src/...`, which --sources src matches on the "src" in
    the middle of it, and which would otherwise be two thirds of the report.

    Deliberately a string operation and not pathlib's relative_to: this runs on
    Windows over Windows paths, and has to be testable on Linux over the same
    paths, which rules out anything whose meaning depends on the host. The
    prefix is matched case-insensitively because Windows paths are, and a drive
    letter reaches us in whichever case the tool wrote it.
    """
    path = filename.replace("\\", "/")
    prefix = root.replace("\\", "/").rstrip("/") + "/"

    candidates = [path]
    for source in sources:
        candidates.append(source.replace("\\", "/").rstrip("/") + "/" + path.lstrip("/"))

    for candidate in candidates:
        if candidate.lower().startswith(prefix.lower()):
            name = candidate[len(prefix) :]
            # Under src/, and not merely under the repository root: joining a
            # foreign relative path onto a <source> of the root itself yields
            # something that passes the prefix test (the UCRT's
            # `minkernel/crts/...` becomes `<root>/minkernel/crts/...`). This
            # is also what drops tests/, which the Linux job drops with
            # lcov --remove, so that the two reports cover the same set.
            if name.startswith("src/"):
                return name
    return None


def main(argv: list[str]) -> int:
    if not 2 <= len(argv) <= 3:
        print(__doc__, file=sys.stderr)
        return 2

    report = Path(argv[1])
    root = argv[2] if len(argv) == 3 else os.getcwd()

    tree = ET.parse(report)
    sources = [s.text for s in tree.iter("source") if s.text]

    # One file can appear as several <class> elements (one per binary that
    # compiled it — service_win.cpp is in both minilog.exe and test_preflight),
    # so the hit counts are merged per line before they are counted: a line
    # covered in one and not the other is covered.
    hits: dict[str, dict[int, int]] = {}
    dropped = 0

    # list(), because classes outside the repository are removed as we go.
    for classes in tree.iter("classes"):
        for cls in list(classes):
            name = relative_path(cls.get("filename") or "", sources, root)
            if name is None:
                classes.remove(cls)
                dropped += 1
                continue
            cls.set("filename", name)
            lines = hits.setdefault(name, {})
            for line in cls.iter("line"):
                number = int(line.get("number", "0"))
                count = int(line.get("hits", "0"))
                lines[number] = max(lines.get(number, 0), count)

    # Every filename is now relative to the repository root, so that is what
    # the single source has to say for Codecov to resolve them.
    for source in tree.iter("source"):
        source.text = "."

    tree.write(report, encoding="utf-8", xml_declaration=True)
    if dropped:
        what = "entry" if dropped == 1 else "entries"
        print(f"({dropped} {what} outside {root}/src dropped from the report)")

    # An empty report uploads as "nothing is covered" rather than failing, so
    # the filters dropping everything has to be an error here. The way that
    # happens is a --sources or --modules pattern that stops matching.
    if not hits:
        print(f"no source file under {root} is in {report}", file=sys.stderr)
        return 1

    total_lines = total_hit = 0
    for name in sorted(hits):
        lines = hits[name]
        found = len(lines)
        covered = sum(1 for count in lines.values() if count > 0)
        total_lines += found
        total_hit += covered
        rate = 100 * covered / found if found else 0.0
        print(f"{name:<45} {rate:6.1f}%  {covered:5d}/{found}")

    rate = 100 * total_hit / total_lines if total_lines else 0.0
    print(f"{'Total':<45} {rate:6.1f}%  {total_hit:5d}/{total_lines}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
