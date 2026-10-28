<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# WParted – Windows Partition Editor

**WParted** is a free, open-source, native partition manager for Microsoft Windows (target: Windows 7 SP1
to Windows 11). Its long-term goal is functional parity with [GParted](https://gparted.org) where this is
technically safe on Windows, with a familiar layout, a modern Windows UI and strict data-safety rules.

> **Status: v0.4.0-alpha (pre-release).** WParted shows disks, partition tables, file systems and
> volumes, plans changes GParted-style for every disk and **executes plans on disk image files only**:
> partitions, FAT/exFAT/NTFS/ext2-4 file systems, data rescue, resuming after an interruption, and a
> prototype of the separate execution process (helper).
> **Physical disks are never written** by this version (see [Roadmap](ROADMAP.md)).

WParted is an independent project. It is not affiliated with or endorsed by the GParted or GNOME projects.

> **Source of truth:** development, issues and releases live on Gitea:
> https://git.tracht-digital.de/jtracht/WParted. A GitHub repository, if present, is a read-only mirror —
> please report issues on Gitea.

## Features in this build

| Area | What works today |
|---|---|
| Disk discovery | All physical disks: model, serial, bus (NVMe, SATA, USB, SD, …), size, logical/physical sector size, SSD, removable, write protection |
| Partition tables | GPT (header + entry CRC, backup header fallback, sector-size mismatch detection) and MBR including extended/logical partitions; whole-device file systems |
| File systems | Signature detection for 31 types incl. NTFS, FAT12/16/32, exFAT, ReFS, BitLocker, ext2/3/4, btrfs, XFS, F2FS, LUKS, LVM, Linux RAID, swap, HFS+, APFS, ZFS, UDF |
| Windows integration | Drive letters / mount points, used/free space, volume containing Windows, cross-check with Windows' own partition view |
| Planning | New, delete, resize/move, copy/paste, format, label, name, flags, new table; future layout, undo, validation with safety rules, plan export/import |
| Image files | Apply plans to `.img` files: tables, partitions, raw move/copy, FAT16/32 create/grow/shrink/check, exFAT create, NTFS grow/shrink, labels and UUIDs for FAT, exFAT, NTFS, ext2/3/4, XFS, btrfs, swap; every step read back and verified |
| Safety | Physical disks are only read (CI-enforced); writes go to user-chosen image files after two confirmations; unknown regions are never shown as free space |
| UI | GParted-style disk map and partition list, light/dark/system theme, per-monitor DPI, English/German |
| CLI | `wparted-cli list [--json]`, `inspect-image`, `rescue-scan-image`, `apply-plan` (dry run unless `--execute`), `fs-support` |
| Accessibility | Keyboard operation, MSAA for the disk map, high-contrast colours, screen-reader names in EN/DE |

Full capability status per file system and action: [docs/research/gparted-parity-matrix.md](docs/research/gparted-parity-matrix.md).

## Compatibility

See [docs/compatibility/windows-support-matrix.md](docs/compatibility/windows-support-matrix.md). Only
Windows 11 x64 has been run so far; Windows 7–10 are build targets awaiting VM verification.

## Installation

No installer yet. Download the portable ZIP from the
[Gitea releases](https://git.tracht-digital.de/jtracht/WParted/releases),
extract it and run `wparted.exe`. To read partition tables, choose **WParted → Restart as Administrator**.

## Building

Release binaries are cross-compiled on Linux (MinGW-w64) and tested under Wine, exactly as in CI:

```sh
tools/ci/build-mingw.sh x64 --package          # Ubuntu 24.04 or the runner image via Docker
```

Local Windows development with Visual Studio 2022, CMake ≥ 3.20, Ninja, Python 3:

```powershell
python -m pip install --user cmake ninja
.\tools\build.ps1 -Arch x64        # builds, runs unit + end-to-end tests
```

Details: [docs/developer/building.md](docs/developer/building.md).

## Documentation

- User guide: [English](docs/user/en/user-guide.md) · [Deutsch](docs/user/de/benutzerhandbuch.md)
- Architecture: [technology decision](docs/architecture/technology-decision.md), [security](docs/architecture/security-architecture.md)
- Research: [GParted architecture](docs/research/gparted-architecture.md), [feature inventory](docs/research/gparted-feature-inventory.md), [UI analysis](docs/research/gparted-ui-analysis.md), [Windows portability](docs/research/windows-portability-analysis.md)
- Design: [design system](docs/design/wparted-design-system.md)
- Process: [contributing](CONTRIBUTING.md), [testing](docs/developer/testing.md), [release process](docs/developer/release-process.md), [Gitea/CI setup](docs/developer/infrastructure-setup.md)

## Kurzfassung (Deutsch)

WParted ist ein freier, nativer Partitionsmanager für Windows mit GParted-ähnlicher Bedienung. Die
aktuelle Alpha ist **ausschließlich lesend**: Sie zeigt Datenträger, Partitionstabellen (GPT/MBR),
Dateisysteme und Volumes an, verändert aber nichts. Fehler und Wünsche bitte auf
[Gitea](https://git.tracht-digital.de/jtracht/WParted/issues) melden.

## License

WParted is licensed under the **GNU General Public License v2.0 or later** ([LICENSE](LICENSE)).
Third-party notices: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
