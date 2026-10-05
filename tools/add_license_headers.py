#!/usr/bin/env python3
"""Add the repo's SPDX licence header to every first-party source file.

Run from the repo root:  python3 tools/add_license_headers.py [--check]

Idempotent: a file that already carries SPDX-License-Identifier is left alone.
Puts the header after a shebang line, and after a Python module docstring, so
neither the interpreter nor `python3 -m` is disturbed.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

HOLDER = "controllercustom@myyahoo.com"
MARKER = "SPDX-License-Identifier: MIT"

# Files whose first-party status is not established, so they must never receive an
# MIT tag. scripts/avenv.sh is vendored verbatim from the arduino-virtual-env skill,
# which carries no licence statement; it has a PROVENANCE header instead and is
# excluded here so a future run cannot stamp MIT over it.
EXCLUDE = {"scripts/avenv.sh"}

# C/C++/ino/headers get slashes; Python and shell get hashes.
SUFFIXES = {
    ".ino": "//", ".cpp": "//", ".h": "//", ".hpp": "//",
    ".py": "#", ".sh": "#", ".properties": "#",
}


def files() -> list[Path]:
    out = subprocess.run(
        ["git", "ls-files"], capture_output=True, text=True, check=True
    ).stdout.split()
    return [
        Path(f)
        for f in out
        if Path(f).suffix in SUFFIXES
        and Path(f).name != "LICENSE"
        and f not in EXCLUDE
    ]


def insert_index(text: str, path: Path) -> int:
    """Line index at which the header should be inserted.

    Conventions followed:
      * a shebang stays on line 1;
      * shell/properties get the header immediately after the shebang, above the
        file's descriptive comment block, so the SPDX tag is the first thing a
        tool reading the top of the file sees;
      * Python gets it *after* the module docstring, because comments are not
        statements and would not break __doc__ -- but a header wedged between the
        shebang and the docstring reads as though the docstring were detached.
    """
    lines = text.splitlines(keepends=True)
    i = 0
    if lines and lines[0].startswith("#!"):
        i = 1

    if path.suffix == ".py" and i < len(lines):
        stripped = lines[i].lstrip()
        if stripped.startswith(('"""', "'''")):
            quote = stripped[:3]
            if stripped.count(quote) >= 2 and stripped.rstrip().endswith(quote):
                i += 1
            else:
                i += 1
                while i < len(lines) and quote not in lines[i]:
                    i += 1
                i += 1
            while i < len(lines) and not lines[i].strip():
                i += 1
    return i


def header_for(path: Path) -> list[str]:
    c = SUFFIXES[path.suffix]
    return [
        f"{c} SPDX-License-Identifier: MIT",
        f"{c} Copyright (c) 2026 {HOLDER}",
        f"{c} Part of Mozzi-tinyusb. See LICENSE at the repository root.",
    ]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="report files needing a header; do not modify")
    args = ap.parse_args()

    todo, skipped = [], []
    for path in files():
        text = path.read_text()
        if MARKER in text:
            skipped.append(str(path))
            continue
        todo.append(path)
        if args.check:
            continue
        lines = text.splitlines(keepends=True)
        idx = insert_index(text, path)
        body = "".join(lines[idx:])
        nl = "\n" if body.endswith("\n") or not body else "\n"
        new = "".join(l if l.endswith("\n") else l + "\n"
                      for l in header_for(path))
        path.write_text("".join(lines[:idx]) + new + nl + body)

    if args.check:
        for p in todo:
            print(f"missing header: {p}")
        print(f"{len(todo)} need a header, {len(skipped)} already have one")
        return 1 if todo else 0
    for p in todo:
        print(f"  header added: {p}")
    print(f"{len(todo)} updated, {len(skipped)} already had SPDX")
    return 0


if __name__ == "__main__":
    sys.exit(main())