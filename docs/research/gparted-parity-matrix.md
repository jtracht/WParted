<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# GParted parity matrix

Binding scope definition for WParted. Every GParted function verified in
`gparted-feature-inventory.md` has a row. Status values: `Done` (implemented and tested in the
stated environment), `Implemented` (code + automated tests, platform verification pending),
`In progress`, `Planned`, `Offline only` (planned for the recovery environment), `Won't do` (with reason).

Issue keys (`WP-nnn`) appear in the title of the matching Gitea issue at
https://git.tracht-digital.de/jtracht/WParted/issues. Risk: L/M/H/C = Low/Medium/High/Critical.
Priority: P0 (blocking) … P3 (nice to have). Windows compatibility lists the *target*; verified
versions are tracked in `docs/compatibility/windows-support-matrix.md`.

Last updated: 2026-10-27 (v0.1.0-alpha development).

## Devices and tables

| ID | GParted function | Description | Original reference | Windows implementation | Windows target | FS | Risk | Prio | Status | Test evidence | Issue | Release |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| F-001 | Refresh devices | Rescan all devices, non-blocking | `Win_GParted::menu_gparted_refresh_devices` | Worker thread + `discoverDisks()` | 7–11 | – | L | P0 | Implemented | Manual run Win11 x64 (2026-10-27) | WP-012 | 0.1.0 |
| F-002 | Device list / selection | Combo + Devices menu | `Win_GParted::refresh_combo_devices` | Combo box + `WParted > Devices` | 7–11 | – | L | P0 | Implemented | Manual run Win11 x64 | WP-013 | 0.1.0 |
| F-003 | Device information | Model, serial, size, sector sizes, table | `Win_GParted` device info pane | `IOCTL_STORAGE_QUERY_PROPERTY`, geometry, alignment, seek penalty | 7–11 | – | L | P0 | Implemented | Manual run Win11 x64 | WP-012 | 0.1.0 |
| F-004 | Read MBR (msdos) incl. extended/logical | Parse primary + EBR chain | libparted | `core/src/partition_table.cpp` | any | – | L | P0 | Implemented | `tests/unit/test_core.cpp` (mbr: 4 cases, fuzz) | WP-010 | 0.1.0 |
| F-005 | Read GPT | Header/array CRC, backup fallback | libparted | `core/src/partition_table.cpp` | any | – | L | P0 | Implemented | `test_core.cpp` (gpt: 8 cases, fuzz) | WP-010 | 0.1.0 |
| F-006 | Other table types (mac, bsd, sun, amiga, dvh, pc98, aix, atari, loop) | Read/create via libparted | libparted | Detection as `unknown` only | 7–11 | – | M | P3 | Planned | – | WP-050 | 0.9.0 |
| F-007 | Create partition table | New msdos/gpt label, erases layout | `Dialog_Disklabel` | `IOCTL_DISK_CREATE_DISK` on dismounted disk | 7–11 | – | C | P1 | Planned | – | WP-030 | 0.3.0 |
| F-008 | Sector sizes 512/1K/2K/4K | All calculations in logical sectors | `Device::sector_size` | `StorageAccessAlignmentProperty` | 7–11 | – | M | P0 | Implemented | `test_core.cpp` (4K GPT, size mismatch) | WP-010 | 0.1.0 |
| F-009 | Attempt data rescue | Scan for lost file systems | `Win_GParted::activate_attempt_rescue_data` (gpart) | Read-only signature scan over unallocated space | 7–11 | all | M | P2 | Planned | – | WP-051 | 0.7.0 |
| F-010 | Whole-device file system (no table) | Show FS on unpartitioned device | `GParted_Core::set_device_from_disk` | `TableType::None` + `WholeDevice` | any | all | L | P1 | Implemented | `test_core.cpp` (superfloppy) | WP-010 | 0.1.0 |

## Partition operations

| ID | GParted function | Description | Original reference | Windows implementation | Windows target | FS | Risk | Prio | Status | Test evidence | Issue | Release |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| F-020 | Pending operation queue | Plan without touching disk | `Win_GParted::Add_Operation` | `core/operations` model | any | – | M | P0 | Planned | – | WP-020 | 0.2.0 |
| F-021 | Undo last / clear all | Queue editing | `Win_GParted::activate_undo` | queue | any | – | L | P0 | Planned | – | WP-020 | 0.2.0 |
| F-022 | Operation merging | Merge consecutive resize/format ops | `Win_GParted::merge_operation` | queue | any | – | M | P2 | Planned | – | WP-020 | 0.2.0 |
| F-023 | Future layout preview | Graphic shows planned state | `DrawingAreaVisualDisk` | model replay | any | – | L | P0 | Planned | – | WP-022 | 0.2.0 |
| F-024 | Apply with progress + details | Execute queue, log tree | `Dialog_Progress`, `OperationDetail` | privileged helper, JSON log | 7–11 | – | C | P0 | Planned | – | WP-023 | 0.3.0 |
| F-025 | Calibration before each step | Re-read boundaries, abort on change | `GParted_Core::calibrate_partition` | re-identify disk (serial, GUID, signature) + re-parse | 7–11 | – | C | P0 | Planned | – | WP-021 | 0.3.0 |
| F-026 | New partition | Primary/extended/logical, size, alignment | `Dialog_Partition_New`, `OperationCreate` | `IOCTL_DISK_SET_DRIVE_LAYOUT_EX` | 7–11 | – | H | P0 | Planned | – | WP-030 | 0.3.0 |
| F-027 | Delete partition | | `OperationDelete` | `IOCTL_DISK_SET_DRIVE_LAYOUT_EX` | 7–11 | – | H | P0 | Planned | – | WP-030 | 0.3.0 |
| F-028 | Resize/move partition | Partition boundaries | `Dialog_Partition_Resize_Move` | layout IOCTL + FS-specific steps | 7–11 | per FS | C | P0 | Planned | – | WP-041 | 0.5.0 |
| F-029 | Copy/paste partition | Block copy into free space | `OperationCopy`, `CopyBlocks` | raw block copier with verify | 7–11 | per FS | C | P1 | Planned | – | WP-042 | 0.5.0 |
| F-030 | Format to | Create FS | `OperationFormat` | VDS/Storage API (Win), offline tools (Linux FS) | 7–11 | per FS | H | P0 | Planned | – | WP-040 | 0.4.0 |
| F-031 | Check / repair | FS check | `OperationCheck` | `chkdsk` via documented API (NTFS/FAT), offline tools | 7–11 | per FS | M | P1 | Planned | – | WP-040 | 0.4.0 |
| F-032 | Label file system | | `OperationLabelFileSystem` | `SetVolumeLabel` (Win FS) | 7–11 | per FS | L | P1 | Planned | – | WP-040 | 0.4.0 |
| F-033 | New UUID | | `OperationChangeUUID` | NTFS serial / GPT GUID | 7–11 | per FS | M | P2 | Planned | – | WP-043 | 0.5.0 |
| F-034 | Name partition (GPT) | | `OperationNamePartition` | layout IOCTL | 7–11 | – | L | P1 | Planned | – | WP-030 | 0.3.0 |
| F-035 | Manage flags | boot/esp/hidden/lba/... | `DialogManageFlags` | type GUID / attribute bits / MBR type | 7–11 | – | H | P1 | Planned (read: Implemented) | `test_core.cpp` (flag mapping) | WP-032 | 0.3.0 |
| F-036 | Alignment MiB/cylinder/none | | `PartitionAlignment` | validation engine | any | – | M | P0 | Planned (alignment check display: Implemented) | Partition info dialog | WP-021 | 0.2.0 |
| F-037 | Save details (HTML) | Export log | `Dialog_Progress::save_details` | JSON + HTML export | any | – | L | P2 | Planned | – | WP-024 | 0.5.0 |

## Content and volumes

| ID | GParted function | Description | Original reference | Windows implementation | Windows target | FS | Risk | Prio | Status | Test evidence | Issue | Release |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| F-040 | FS detection | Identify content | `FS_Info` (blkid) | `detectFilesystem()` (31 types) | any | all | L | P0 | Implemented | `test_core.cpp` (21 signatures) | WP-011 | 0.1.0 |
| F-041 | Used/unused space | | FS tools / statvfs | `GetDiskFreeSpaceEx` (mounted Windows FS) | 7–11 | ntfs, fat, exfat, refs, udf | L | P1 | Implemented (limited) | Manual run Win11 x64 | WP-012 | 0.1.0 |
| F-042 | Mount / unmount | | `Win_GParted::activate_mount_partition` | `SetVolumeMountPoint` / `DeleteVolumeMountPoint` | 7–11 | Win FS | M | P2 | Planned | – | WP-044 | 0.5.0 |
| F-043 | Busy detection | mounted/in use | `GParted_Core::is_busy` | volume mount points, page file, Windows volume, open handles | 7–11 | – | H | P0 | In progress (Windows volume + mount state) | Manual run | WP-021 | 0.2.0 |
| F-044 | Partition information | Detailed dialog | `Dialog_Partition_Info` | TaskDialog (v0.1) | 7–11 | all | L | P0 | Implemented | Manual run Win11 x64 | WP-013 | 0.1.0 |
| F-045 | LUKS open/close | | `luks.cc`, `PasswordRAMStore` | detection only; offline mode | – | luks | H | P3 | Offline only | – | WP-070 | 0.7.0 |
| F-046 | LVM VG activate/deactivate | | `LVM2_Info` | detection only; offline mode | – | lvm2 | H | P3 | Offline only | – | WP-070 | 0.7.0 |
| F-047 | swapon/swapoff | | `linux_swap.cc` | n/a on Windows | – | swap | – | – | Won't do (no Linux kernel) | – | – | – |

## File systems (per action, see also `capabilitiesFor()` in `core/src/filesystem.cpp`)

| ID | FS | GParted actions | Windows strategy | Risk | Prio | Status (this build) | Issue | Release |
|---|---|---|---|---|---|---|---|---|
| F-100 | ntfs | all | Native: format (VDS/Storage API), shrink/extend (FSCTL), check (chkdsk API), label | C | P0 | Detect: Implemented | WP-040 | 0.4.0 |
| F-101 | fat16/fat32 | all (resize via libparted) | Native format/label/check; resize needs own implementation or offline | H | P0 | Detect: Implemented | WP-040 | 0.4.0 |
| F-102 | exfat | create, check, label, uuid, move, copy | Native format/label/check | H | P1 | Detect: Implemented | WP-040 | 0.4.0 |
| F-103 | ext2/3/4 | all | Offline (e2fsprogs); detection + label/UUID read native | H | P1 | Detect: Implemented | WP-060 | 0.6.0 |
| F-104 | btrfs | all | Offline | H | P2 | Detect: Implemented | WP-060 | 0.6.0 |
| F-105 | xfs | create, grow, check, label, uuid, copy | Offline | H | P2 | Detect: Implemented | WP-060 | 0.6.0 |
| F-106 | f2fs, jfs, reiserfs, reiser4, nilfs2, minix | per inventory | Offline | H | P3 | Detect: Experimental | WP-060 | 0.6.0 |
| F-107 | hfs, hfs+ | create, shrink, check (hfs+) | Offline | H | P3 | Detect: Experimental | WP-060 | 0.6.0 |
| F-108 | linux-swap | create, resize, label, uuid | Native (write swap header) – simple format | M | P2 | Detect: Implemented | WP-060 | 0.6.0 |
| F-109 | luks, lvm2 pv, linux-raid, bcachefs, zfs, apfs, ufs, linux-suspend | detect, move, copy | Detect natively; move/copy via block copier | H | P3 | Detect: Implemented/Experimental | WP-042 | 0.5.0 |
| F-110 | udf | create, label, uuid, move, copy | Native format (Win FS) | M | P3 | Detect: Experimental | WP-040 | 0.4.0 |
| F-111 | refs (Windows) | detect (GParted) | Native detection; resize via Storage API on supported editions | H | P3 | Detect: Implemented | WP-040 | 0.4.0 |
| F-112 | bitlocker | detect, move, copy | Detect; never decrypt; move/copy blocked unless unlocked state verified | C | P1 | Detect: Implemented | WP-071 | 0.7.0 |

## Windows-specific additions (not in GParted)

| ID | Function | Status | Issue | Release |
|---|---|---|---|---|
| F-200 | Unelevated UI + restart elevated | Implemented | WP-013 | 0.1.0 |
| F-201 | Volume mapping (drive letters, mount points, Windows volume) | Implemented | WP-012 | 0.1.0 |
| F-202 | System-critical partition protection (ESP, MSR, WinRE, LDM, Storage Spaces) | Detection implemented; enforcement planned | WP-021 | 0.2.0 |
| F-203 | Cross-check own parse vs. Windows layout | Implemented | WP-012 | 0.1.0 |
| F-204 | JSON diagnostics report export (GUI + CLI) | Implemented | WP-013 | 0.1.0 |
| F-205 | Read-only CLI incl. disk image analysis | Implemented | WP-016 | 0.1.0 |
| F-206 | Dark/light/system theme, per-monitor DPI | Implemented | WP-013 | 0.1.0 |
| F-207 | Dynamic disks / Storage Spaces handling | Detection of types; no modification | WP-072 | 0.7.0 |
| F-208 | BCD / boot entry safety checks | Planned | WP-071 | 0.7.0 |
