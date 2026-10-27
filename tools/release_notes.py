#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prints release notes for a version from CHANGELOG.md plus fixed notices.

Usage: release_notes.py <version>   (e.g. 0.1.0-alpha)
Falls back to the [Unreleased] section if the version has no own section yet.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def section(text: str, header_regex: str) -> str:
    m = re.search(header_regex + r".*?\n(.*?)(?=\n## \[|\Z)", text, re.S)
    return m.group(1).strip() if m else ""


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    version = sys.argv[1]
    changelog = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
    body = section(changelog, r"## \[" + re.escape(version) + r"\]") or section(changelog, r"## \[Unreleased\]")
    unsigned = (ROOT / "dist" / "UNSIGNED.txt").exists()
    print(f"# WParted v{version}\n")
    if "-" in version:
        print("> **Pre-release.** Read the compatibility matrix before use.\n")
    print(body + "\n")
    print("## Verification\n")
    print(f"- SHA-256 checksums: `WParted-v{version}-checksums.sha256`")
    print(f"- SBOM (CycloneDX): `WParted-v{version}-sbom.json`")
    print("- Code signing: " + ("**not signed** (no certificate configured)" if unsigned else "Authenticode-signed"))
    print("\nSupported platforms: see docs/compatibility/windows-support-matrix.md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
