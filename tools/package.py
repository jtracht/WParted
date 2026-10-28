#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Packages a build directory into a portable ZIP and appends its SHA-256.

Works on Windows (MSVC builds) and on Linux (MinGW cross builds, smoke test
through Wine):
  package.py --build-dir build/mingw-x64 --arch x64 [--emulator wine] [--out dist]
"""
import argparse
import hashlib
import re
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOCS = ["LICENSE", "README.md", "CHANGELOG.md", "THIRD_PARTY_NOTICES.md", "SECURITY.md"]
# File names state the targeted platform range; verification status is in
# docs/compatibility/windows-support-matrix.md.
PLATFORM = {"x64": "win7plus-x64", "x86": "win7plus-x86", "arm64": "win11-arm64"}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True, type=Path)
    ap.add_argument("--arch", required=True, choices=PLATFORM)
    ap.add_argument("--emulator", default="", help="e.g. wine, for the smoke test on Linux")
    ap.add_argument("--out", default=ROOT / "dist", type=Path)
    ap.add_argument("--license-file", action="append", default=[], metavar="NAME=PATH",
                    help="extra licence text shipped as licenses/NAME (statically linked runtimes)")
    args = ap.parse_args()

    header = (args.build_dir / "apps/wparted-gui/wparted_version.h").read_text(encoding="utf-8")
    version = re.search(r'WPARTED_VERSION_STRING "([^"]+)"', header).group(1)
    exes = {"wparted.exe": args.build_dir / "apps/wparted-gui/wparted.exe",
            "wparted-cli.exe": args.build_dir / "apps/wparted-cli/wparted-cli.exe",
            "wparted-helper.exe": args.build_dir / "apps/wparted-cli/wparted-helper.exe"}
    for name, path in exes.items():
        if not path.is_file():
            print(f"missing build output: {path}", file=sys.stderr)
            return 1

    # Smoke test: the packaged CLI must start and report the expected version.
    cmd = ([args.emulator] if args.emulator else []) + [str(exes["wparted-cli.exe"]), "version"]
    reported = subprocess.run(cmd, capture_output=True, text=True).stdout.strip().splitlines()
    if not reported or reported[-1].strip() != version:
        print(f"CLI smoke test failed: expected {version}, got {reported!r}", file=sys.stderr)
        return 1

    args.out.mkdir(parents=True, exist_ok=True)
    name = f"WParted-v{version}-{PLATFORM[args.arch]}-portable"
    zip_path = args.out / f"{name}.zip"
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for arcname, path in exes.items():
            z.write(path, f"{name}/{arcname}")
        for doc in DOCS:
            z.write(ROOT / doc, f"{name}/{doc}")
        for spec in args.license_file:
            lic_name, lic_path = spec.split("=", 1)
            z.write(lic_path, f"{name}/licenses/{lic_name}")
    digest = hashlib.sha256(zip_path.read_bytes()).hexdigest()
    with (args.out / f"WParted-v{version}-checksums.sha256").open("a", encoding="ascii") as f:
        f.write(f"{digest}  {zip_path.name}\n")
    print(f"packaged {zip_path} ({digest})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
