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


def relative_path(filename: str, root: str) -> str:
    """Return filename relative to root, forward-slashed, if it is under it.

    Deliberately a string operation and not pathlib's relative_to: this runs on
    Windows over Windows paths, and has to be testable on Linux over the same
    paths, which rules out anything whose meaning depends on the host. The
    prefix is matched case-insensitively because Windows paths are, and a drive
    letter reaches us in whichever case the tool wrote it.
    """
    path = filename.replace("\\", "/")
    prefix = root.replace("\\", "/").rstrip("/") + "/"
    if path.lower().startswith(prefix.lower()):
        return path[len(prefix) :]
    # Not under the repository: leave it as it is rather than guess, and let it
    # show up in the table so that a stray path is visible.
    return path


def main(argv: list[str]) -> int:
    if not 2 <= len(argv) <= 3:
        print(__doc__, file=sys.stderr)
        return 2

    report = Path(argv[1])
    root = argv[2] if len(argv) == 3 else os.getcwd()

    tree = ET.parse(report)

    # One file can appear as several <class> elements (one per object file that
    # compiled it), so the hit counts are merged per line before they are
    # counted: a line covered in one and not the other is covered.
    hits: dict[str, dict[int, int]] = {}

    for cls in tree.iter("class"):
        filename = cls.get("filename")
        if filename is None:
            continue
        name = relative_path(filename, root)
        cls.set("filename", name)
        lines = hits.setdefault(name, {})
        for line in cls.iter("line"):
            number = int(line.get("number", "0"))
            count = int(line.get("hits", "0"))
            lines[number] = max(lines.get(number, 0), count)

    for source in tree.iter("source"):
        source.text = "."

    tree.write(report, encoding="utf-8", xml_declaration=True)

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
