#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Writes a CycloneDX 1.5 SBOM for a WParted release.

WParted v0.1 has no third-party source dependencies; the binaries link the
statically built Microsoft C/C++ runtime and import Windows system DLLs.
These are recorded so that the SBOM reflects what actually ships.
Usage: generate_sbom.py <version> <output.json> [artifact ...]
"""
import hashlib
import json
import sys
import uuid
from datetime import datetime, timezone
from pathlib import Path


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    version, out = sys.argv[1], Path(sys.argv[2])
    artifacts = [Path(a) for a in sys.argv[3:]]
    bom = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "serialNumber": f"urn:uuid:{uuid.uuid4()}",
        "version": 1,
        "metadata": {
            "timestamp": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "component": {
                "type": "application",
                "bom-ref": "wparted",
                "name": "WParted",
                "version": version,
                "licenses": [{"license": {"id": "GPL-2.0-or-later"}}],
                "externalReferences": [{"type": "vcs", "url": "https://git.tracht-digital.de/jtracht/WParted"}],
            },
        },
        "components": [
            {
                "type": "library",
                "bom-ref": "msvc-crt-static",
                "name": "Microsoft Visual C++ runtime (static, /MT)",
                "version": "14.44",
                "supplier": {"name": "Microsoft"},
                "licenses": [{"license": {"name": "Microsoft Visual Studio license terms (redistributable code)"}}],
            },
            {
                "type": "platform",
                "bom-ref": "windows-system-dlls",
                "name": "Windows system libraries (kernel32, user32, gdi32, comctl32 v6, comdlg32, shell32, ole32, advapi32)",
                "supplier": {"name": "Microsoft"},
                "description": "Provided by the operating system; not redistributed.",
            },
        ],
        "dependencies": [{"ref": "wparted", "dependsOn": ["msvc-crt-static", "windows-system-dlls"]}],
    }
    for a in artifacts:
        bom["components"].append({
            "type": "file",
            "bom-ref": f"file:{a.name}",
            "name": a.name,
            "hashes": [{"alg": "SHA-256", "content": sha256(a)}],
        })
    out.write_text(json.dumps(bom, indent=2), encoding="utf-8")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
