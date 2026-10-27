#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Read-only guard for the v0.1/v0.2 release line.

Fails if any production source requests write access to a device, opens a
file for writing or uses a modifying storage API. A line may opt out only
with an explicit `readonly-guard: allow` comment - used for user-chosen
report/plan/log files and for the image-file executor (core/src/executor.cpp,
which refuses device paths) - and every exemption is printed for reviewers.

This check is a safety net, not a proof. It is removed per API once the
operation engine with privilege separation (issue WP-020 and later) lands.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCAN_DIRS = ["core", "platforms", "apps"]
EXTENSIONS = {".cpp", ".hpp", ".h", ".c", ".cc"}

FORBIDDEN = [
    (r"\bGENERIC_WRITE\b", "write access"),
    (r"\bGENERIC_ALL\b", "full access"),
    (r"\bFILE_GENERIC_WRITE\b", "write access"),
    (r"\bFILE_WRITE_DATA\b", "write access"),
    (r"\bWriteFile(Ex)?\s*\(", "WriteFile"),
    (r"\bIOCTL_DISK_SET_\w+", "partition table modification"),
    (r"\bIOCTL_DISK_CREATE_DISK\b", "partition table creation"),
    (r"\bIOCTL_DISK_DELETE_DRIVE_LAYOUT\b", "partition table deletion"),
    (r"\bIOCTL_DISK_GROW_PARTITION\b", "partition resize"),
    (r"\bIOCTL_DISK_UPDATE_PROPERTIES\b", "partition table refresh"),
    (r"\bFSCTL_LOCK_VOLUME\b", "volume lock"),
    (r"\bFSCTL_DISMOUNT_VOLUME\b", "volume dismount"),
    (r"\bFSCTL_EXTEND_VOLUME\b", "volume extend"),
    (r"\bFSCTL_SHRINK_VOLUME\b", "volume shrink"),
    (r"\bDeleteVolumeMountPoint\w*\b", "mount point removal"),
    (r"\bSetVolumeMountPoint\w*\b", "mount point change"),
    (r"\bSetVolumeLabel\w*\b", "label change"),
    (r"\bfopen(_s)?\s*\([^;]*\"[^\"]*[wa+][^\"]*\"", "file opened for writing"),
    (r"\b_w?fsopen\s*\([^;]*\"[^\"]*[wa+][^\"]*\"", "file opened for writing"),
    (r"\b(std::)?ofstream\b", "file opened for writing"),
]
ALLOW = "readonly-guard: allow"
# Test helpers legitimately create synthetic image files.
EXEMPT_PATHS = {"tests"}


def main() -> int:
    violations, exemptions = [], []
    for d in SCAN_DIRS:
        for path in sorted((ROOT / d).rglob("*")):
            if path.suffix not in EXTENSIONS or not path.is_file():
                continue
            rel = path.relative_to(ROOT).as_posix()
            if rel.split("/")[0] in EXEMPT_PATHS:
                continue
            for no, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                code = line.split("//", 1)[0] if ALLOW not in line else line
                for pattern, what in FORBIDDEN:
                    if re.search(pattern, code):
                        (exemptions if ALLOW in line else violations).append(f"{rel}:{no}: {what}: {line.strip()}")
    # The log file is opened for appending by design (application log, not a disk).
    violations = [v for v in violations if not v.startswith("core/src/log.cpp")]
    for e in exemptions:
        print(f"EXEMPT  {e}")
    for v in violations:
        print(f"VIOLATION  {v}")
    print(f"read-only guard: {len(violations)} violation(s), {len(exemptions)} reviewed exemption(s)")
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
