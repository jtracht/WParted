<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Roadmap

Milestones are planning hypotheses and are tracked on Gitea
(https://git.tracht-digital.de/jtracht/WParted/milestones). Scope per feature: [parity matrix](docs/research/gparted-parity-matrix.md).

| Phase | Version | Milestone | Content | State |
|---|---|---|---|---|
| 0 | – | Architecture Foundation | GParted analysis, parity matrix, licence review, technology decision, repository, CI definitions | Done (CI runners pending) |
| 1 | 0.1.0-alpha | Read-Only Disk Explorer | Discovery, GPT/MBR, FS detection, GUI, CLI, dark mode, logging | Implemented; release blocked on Windows runner and elevated hardware verification |
| 2 | 0.2.0-alpha | Operation Planning | Operation queue, validation engine, layout preview, undo, safety dialogs | Planned |
| 3 | 0.3.0-alpha | Basic Partition Operations | Privileged helper, create/delete partitions and tables, names, flags — VHD tests first | Planned |
| 4 | 0.4.0-alpha | Windows Filesystems | NTFS/FAT/exFAT format, check, label; NTFS shrink/grow | Planned |
| 5 | 0.5.0-alpha | Advanced Partition Management | Move, copy, block copier, progress, logs, resume concept | Planned |
| 6 | 0.6.0-alpha | Linux Filesystem Support | ext*, btrfs, xfs … via offline mode, capability checks | Planned |
| 7 | 0.7.0-alpha | Recovery & Advanced Storage | Recovery medium, BitLocker/EFI/BCD safety, dynamic disks, Storage Spaces | Planned |
| 8 | 0.8.0-beta.1 | Public Beta | UX/accessibility review, compatibility, benchmarks, security review, installer | Planned |
| 9 | 0.9.0-beta | Feature Completion | Remaining parity, legacy tests, end-to-end tests, docs | Planned |
| 10 | 1.0.0-rc | Release Candidate | Release blockers, SBOM, signing, final checks | Planned |
| 11 | 1.0.0 | Stable Release | Stable, with transparent parity status | Planned |
