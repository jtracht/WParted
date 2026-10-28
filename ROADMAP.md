<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Roadmap

Milestones are planning hypotheses and are tracked on Gitea
(https://git.tracht-digital.de/jtracht/WParted/milestones). Scope per feature: [parity matrix](docs/research/gparted-parity-matrix.md).

| Phase | Version | Milestone | Content | State |
|---|---|---|---|---|
| 0 | – | Architecture Foundation | GParted analysis, parity matrix, licence review, technology decision, repository, CI definitions | Done (CI runners pending) |
| 1 | 0.1.0-alpha | Read-Only Disk Explorer | Discovery, GPT/MBR, FS detection, GUI, CLI, dark mode, logging | Released 2026-10-27 (pre-release); elevated hardware verification open (WP-014) |
| 2 | 0.2.0-alpha | Operation Planning | Operation queue, validation engine, layout preview, undo, safety dialogs; ahead of plan: execution on disk image files | Released 2026-10-28 (pre-release) |
| 3 | 0.3.0-alpha | Basic Partition Operations | Privileged helper, create/delete partitions and tables, names, flags — VHD tests first | On image files done (v0.2/v0.3); helper channel prototype in v0.4.0-alpha (image files only); elevated helper and physical disks open |
| 4 | 0.4.0-alpha | Windows Filesystems | NTFS/FAT/exFAT format, check, label; NTFS shrink/grow | On image files: FAT complete, exFAT create/grow/shrink/label, NTFS create/grow/shrink/check (read-only)/label; repair and chkdsk verification open |
| 5 | 0.5.0-alpha | Advanced Partition Management | Move, copy, block copier, progress, logs, resume concept | On image files: raw move/copy, extended partition moves, journal with resume after crashes (fault-injection tested) done; physical disks open |
| 6 | 0.6.0-alpha | Linux Filesystem Support | ext*, btrfs, xfs … via offline mode, capability checks | On image files (released in v0.3.0-alpha): ext2/3/4 create, grow, shrink (free tail), read-only check, labels/UUIDs; ext data relocation, XFS/btrfs resize open |
| 7 | 0.7.0-alpha | Recovery & Advanced Storage | Recovery medium, BitLocker/EFI/BCD safety, dynamic disks, Storage Spaces | Managed disks refused; BitLocker state, boot manager and BCD offset rules in development builds; recovery medium open |
| 8 | 0.8.0-beta.1 | Public Beta | UX/accessibility review, compatibility, benchmarks, security review, installer | Planned |
| 9 | 0.9.0-beta | Feature Completion | Remaining parity, legacy tests, end-to-end tests, docs | Planned |
| 10 | 1.0.0-rc | Release Candidate | Release blockers, SBOM, signing, final checks | Planned |
| 11 | 1.0.0 | Stable Release | Stable, with transparent parity status | Planned |
