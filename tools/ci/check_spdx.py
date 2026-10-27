#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verifies that every source/script file carries an SPDX license identifier."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CHECKED = {".cpp", ".hpp", ".h", ".py", ".ps1", ".cmake", ".in", ".yml", ".yaml"}
NAMES = {"CMakeLists.txt"}
SKIP_DIRS = {"build", ".git", "node_modules"}


def main() -> int:
    missing = []
    for path in sorted(ROOT.rglob("*")):
        if not path.is_file() or any(part in SKIP_DIRS for part in path.relative_to(ROOT).parts):
            continue
        if path.suffix not in CHECKED and path.name not in NAMES:
            continue
        head = path.read_text(encoding="utf-8", errors="replace")[:600]
        if "SPDX-License-Identifier:" not in head:
            missing.append(path.relative_to(ROOT).as_posix())
    for m in missing:
        print(f"missing SPDX identifier: {m}")
    print(f"SPDX check: {len(missing)} file(s) without identifier")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
