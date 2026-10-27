<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Windows portability analysis

GParted depends on Linux kernel interfaces, libparted and ~40 external Linux tools. None of these are
available on Windows. This document maps each GParted mechanism to a Windows mechanism and rates the
risk.

## Mechanism mapping

| GParted mechanism | Windows equivalent | Availability | Risk / notes |
|---|---|---|---|
| `/proc/partitions`, libparted device probe | `\\.\PhysicalDriveN` + `IOCTL_STORAGE_QUERY_PROPERTY`, `IOCTL_DISK_GET_DRIVE_GEOMETRY_EX` | Win7+ | Query without data access works unelevated |
| Sector size | `StorageAccessAlignmentProperty` (logical + physical) | Win7+ | USB bridges may translate 512↔4096; WParted detects GPT at the "wrong" size and reports it |
| SSD detection | `StorageDeviceSeekPenaltyProperty` | Win7+ (reliable from 8) | Tri-state, shown as unknown if not reported |
| libparted table read | Own GPT/MBR parser on raw sectors (`core/src/partition_table.cpp`) + cross-check with `IOCTL_DISK_GET_DRIVE_LAYOUT_EX` | Win7+ | Raw read needs elevation |
| libparted table write | `IOCTL_DISK_SET_DRIVE_LAYOUT_EX`, `IOCTL_DISK_CREATE_DISK`, `IOCTL_DISK_UPDATE_PROPERTIES` | Win7+ | Critical: requires locked/dismounted volumes, careful ordering, verification read-back |
| `blkid` | Own signature probe (`core/src/filesystem.cpp`) | any | Read-only; tested with synthetic images |
| Mount info (`/proc/mounts`) | `FindFirstVolume`, `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS`, `GetVolumePathNamesForVolumeName` | Win7+ | Spanned volumes (dynamic disks, Storage Spaces) have >1 extent |
| `statvfs` / FS tools for usage | `GetDiskFreeSpaceEx` on mounted volumes | Win7+ | Only for volumes Windows mounts (NTFS, FAT, exFAT, ReFS, UDF) |
| `umount` / busy check | `FSCTL_LOCK_VOLUME`, `FSCTL_DISMOUNT_VOLUME`, open handle detection | Win7+ | Locking the system volume is impossible online → offline mode |
| `ntfsresize` | `FSCTL_SHRINK_VOLUME` / `FSCTL_EXTEND_VOLUME` (as used by Disk Management) or VDS/Storage Management API | Win7+ (VDS deprecated; `MSFT_Partition.Resize` Win8+) | Native, online, supported by Microsoft; shrink limited by unmovable files |
| `mkntfs`, `mkfs.fat`, exFAT | `format.com` is not scriptable safely; preferred: VDS `IVdsVolumeMF::Format` (Win7) / `MSFT_Volume.Format` (Win8+) / `fmifs.dll FormatEx` (undocumented) | Win7+ | Choose documented APIs; FormatEx only behind capability flag |
| `e2fsprogs`, `btrfs-progs`, `xfsprogs`... | Not available natively | — | Offline recovery environment (Linux based, tools under their own licences) |
| `CopyBlocks` | Own block copier on raw handles with verification | Win7+ | Must respect alignment, unbuffered I/O, progress, cancellation points |
| `pkexec` | UAC elevation of a separate helper process; IPC via named pipe with ACL | Win7+ | Helper validates every request server-side |
| LUKS/LVM/MD | Detection only on Windows | — | Modification only in offline mode |
| BitLocker | Signature `-FVE-FS-`; status via `Win32_EncryptableVolume` (WMI, admin) | Win7 Ent/Ult, 8+ Pro | Never decrypt/suspend without explicit user action |

## Platform constraints

- **Raw disk I/O** requires sector-aligned offsets/lengths and aligned buffers. WParted uses a
  page-aligned bounce buffer (`DeviceReader`).
- **System volume** cannot be dismounted while Windows runs. NTFS shrink/extend of the system volume
  is possible online through the file system itself; moving its start is not.
- **Windows 7**: no `GetDpiForWindow`, no dark mode, `LOAD_LIBRARY_SEARCH_*` only with KB2533623.
  All such APIs are resolved dynamically.
- **ARM64** Windows exposes the same storage APIs; build requires the MSVC ARM64 toolset.

## Conclusion

A native Win32 C++ implementation can cover discovery, table management and Windows-native file
systems (NTFS, FAT, exFAT, ReFS read). Linux file system *modification* requires an offline mode.
This drives the architecture decision in `docs/architecture/technology-decision.md`.
