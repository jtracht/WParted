<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Project status

State of WParted as of v0.5.0-alpha (2026-11-01). The percentage in `status/progress.json` counts work
packages; it says nothing about GParted parity on real disks.

## Finished and released

- **Read:** disks, GPT/MBR, 31 file-system signatures, volumes; checked with administrator rights on real
  hardware (read-only) against Windows' own view.
- **Plan:** every GParted-style operation, future layout, undo, validation, plan files.
- **Execute on disk image files** (raw and fixed-size VHD), through a separate helper process: tables,
  partitions, moves, copies, FAT, exFAT, NTFS, ext2/3/4 as listed in the user guide; resume after an
  interruption; recreating lost partitions. Results are checked in CI with independent tools.
- **Project:** CI and releases on GitHub, documentation in English and German, SBOM and checksums.

## Not finished

| What | Why it is open | What it needs |
|---|---|---|
| **Writing to physical disks** | Deliberately not implemented; the most dangerous path of a partition manager | Elevated helper (UAC), VHDX gate on attached disks, **human security review** (ADR-0002) |
| Recovery/offline mode (WP-070) | Concept only (ADR-0003) | Building and testing a recovery medium; needed to change the running Windows |
| Mount/unmount (WP-044) | Changes the live system, so it belongs to the physical write path | As above |
| Windows 7–10, ARM64 runtime (WP-015) | Only build targets; never run | VMs or devices |
| High-contrast mode (WP-017) | Implemented, not checked visually | A short manual check |
| BitLocker states (WP-071) | Only "not encrypted" observed | Test machine with BitLocker |
| Code signing (WP-009) | Binaries are unsigned | A certificate |
| File systems | No data relocation when shrinking NTFS/exFAT/ext; NTFS repair; XFS/btrfs resize; other GParted file systems; Windows' chkdsk has not checked NTFS created by WParted | Further development, chkdsk run |

Open work is tracked in https://git.tracht-digital.de/jtracht/WParted-PM/issues.
