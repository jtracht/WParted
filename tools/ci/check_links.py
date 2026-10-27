#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Checks that relative links in Markdown files point to existing files."""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LINK = re.compile(r"\[[^\]]*\]\(([^)\s]+)\)")
SKIP_DIRS = {"build", ".git", "dist"}


def main() -> int:
    broken = []
    for md in sorted(ROOT.rglob("*.md")):
        if any(p in SKIP_DIRS for p in md.relative_to(ROOT).parts):
            continue
        for target in LINK.findall(md.read_text(encoding="utf-8", errors="replace")):
            if re.match(r"^[a-z]+:", target) or target.startswith("#"):
                continue
            path = target.split("#", 1)[0]
            if path and not (md.parent / path).exists():
                broken.append(f"{md.relative_to(ROOT).as_posix()}: {target}")
    for b in broken:
        print(f"broken link: {b}")
    print(f"link check: {len(broken)} broken relative link(s)")
    return 1 if broken else 0


if __name__ == "__main__":
    sys.exit(main())
