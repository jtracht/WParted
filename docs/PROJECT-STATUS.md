<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Project status

State of the `main` branch on 2026-11-01, version 0.8.0-beta.1 (beta by maintainer decision; it does not mean every item below has passed). Windows 11 is the only target. The percentage in
`status/progress.json` counts work packages; it says nothing about GParted parity on real disks.

## Finished

- **Read:** disks, GPT/MBR, 31 file-system signatures, volumes; checked with administrator rights on real
  hardware against Windows' own view.
- **Plan:** every GParted-style operation, future layout, undo, validation, plan files.
- **Execute on disk image files** (raw, fixed-size VHD) through the helper: tables, partitions, moves, copies,
  FAT, exFAT, NTFS, ext2/3/4 including moving data when shrinking; resume; recreating lost partitions. Results
  are checked in CI with independent tools.

## Built, not yet checked by a person

| What | Check |
|---|---|
| Writing to attached virtual disks (UAC, volume locks) | `docs/verification/WParted-Pruefcheckliste.pdf`; no agent runs these writes |
| Mount and unmount | same |
| High contrast and other display scaling with the new design | same |
| `chkdsk` on NTFS that WParted created, shrank or moved | same |

## Not finished

| What | Why it is open | What it needs |
|---|---|---|
| **Writing to real disks** | Locked by a build option (`WPARTED_ENABLE_REAL_DISK_WRITES`) | The checklist passed, a **human security review**, then a build with the option |
| Recovery medium (WP-070) | Script exists, never run | Windows ADK, a test machine; needs real disks unlocked to be useful |
| ARM64 runtime (WP-015) | Built, never run | An ARM64 device |
| BitLocker states (WP-071) | Only "not encrypted" observed | A test machine with BitLocker |
| Code signing (WP-009) | Binaries are unsigned | A certificate |
| btrfs shrink, XFS shrink | Relocating btrfs chunks; XFS has no shrink design. Refused today. | Further development (agent can do it, low priority) |
| Other GParted file systems | f2fs, jfs, reiser, nilfs2, UDF and others: detection only | Further development (agent can do it, low priority) |

**Decision: no own file-system repair.** A repair written here could destroy data that `chkdsk` or `e2fsck` would
save, so WParted only checks read-only and points to those tools.

Report problems through https://github.com/jtracht/WParted/issues.
