#!/usr/bin/env python3
"""Check that (), {} and [] balance in C++ sources.

A crude stand-in for a compiler, for the sources that only the Windows CI job
ever compiles — `src/server/platform/service_win.cpp` and anything else behind
`#ifdef _WIN32`. Development happens on Linux, where those files are not built
at all, so an unbalanced parenthesis in them survives every local check and
fails ten minutes later in CI.

It is not a parser: it strips comments and string and character literals, then
counts. That is enough to catch the mistake it exists for, and it will not catch
anything subtler. The Windows job remains the real check.

Usage:
    python3 tests/check_brackets.py [files...]

With no arguments it checks every .cpp/.hpp under src/ and tests/.
"""

import re
import sys
from pathlib import Path

PAIRS = {"(": ")", "{": "}", "[": "]"}

ROOT = Path(__file__).resolve().parent.parent


def strip_literals(source: str) -> str:
    """Remove comments and string/char literals, line by line.

    Line by line because a greedy string-literal pattern applied to a whole file
    happily matches across newlines, from one quote to some later one, and eats
    the brackets in between — which is how a checker like this gets it wrong.
    """
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.S)
    out = []
    for line in source.split("\n"):
        line = re.sub(r"//.*", "", line)
        line = re.sub(r'"(\\.|[^"\\])*"', '""', line)
        line = re.sub(r"'(\\.|[^'\\])*'", "''", line)
        out.append(line)
    return "\n".join(out)


def check(path: Path) -> bool:
    text = strip_literals(path.read_text(encoding="utf-8"))
    ok = True
    for opening, closing in PAIRS.items():
        if text.count(opening) != text.count(closing):
            print(
                f"{path}: {text.count(opening)} '{opening}' vs {text.count(closing)} '{closing}'",
                file=sys.stderr,
            )
            ok = False
    return ok


def main() -> None:
    if len(sys.argv) > 1:
        paths = [Path(a) for a in sys.argv[1:]]
    else:
        paths = sorted(
            p
            for directory in ("src", "tests")
            for p in (ROOT / directory).rglob("*")
            if p.suffix in (".cpp", ".hpp")
        )

    failed = [p for p in paths if not check(p)]
    if failed:
        sys.exit(f"Unbalanced brackets in {len(failed)} file(s).")
    print(f"OK: brackets balance in {len(paths)} file(s).")


if __name__ == "__main__":
    main()
