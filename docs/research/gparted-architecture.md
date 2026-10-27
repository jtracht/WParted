<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# GParted architecture analysis

Reference snapshot: `https://github.com/GNOME/gparted`, commit `d6e8360e9c07d0143e5e5682b766f24be06a3187`
(2026-10-11), `configure.ac` version `1.8.1-git`. Analysed on 2026-10-27.

This document describes how GParted is built so that WParted can reach functional parity
with its *own* architecture. No GParted source code is copied into WParted (see
[licensing](#licensing-observations)).

## Size and layout

| Area | Location | Notes |
|---|---|---|
| Application sources | `src/*.cc` (77 files, ~27.6k lines) | Largest: `GParted_Core.cc` (4,730), `Win_GParted.cc` (3,753) |
| Public headers | `include/*.h` | One header per class |
| File system back ends | `src/<fs>.cc` | 19 classes deriving from `FileSystem` |
| Unit tests | `tests/test_*.cc` | Googletest (vendored in `lib/gtest`) |
| Translations | `po/*.po` | 69 languages (gettext) |
| User manual | `help/<lang>/` | Mallard/DocBook, GFDL-1.2 (`COPYING-DOCS`) |
| Privilege escalation | `gparted.in`, `org.gnome.gparted.policy.in.in` | Shell wrapper + polkit (`pkexec`) |
| Build | autotools (`configure.ac`, `Makefile.am`) | Requires gtkmm-3, libparted ≥ 2.2, libparted-fs-resize |

## Layered structure

```
Win_GParted (GTK main window, menus, toolbar, operation list)
  ├── Dialog_* (New, Resize/Move, Copy, Info, Flags, Label, Name, Disklabel, Progress, Features)
  ├── DrawingAreaVisualDisk (proportional disk graphic)  /  TreeView_Detail (partition list)
  └── HBoxOperations (pending operation list)
GParted_Core (device scan, operation execution, calibration, alignment)
  ├── libparted (partition table read/write, FAT/HFS resize via libparted-fs-resize)
  ├── FileSystem subclasses (ext2, ntfs, xfs, btrfs, ...) -> external CLI tools
  ├── *_Info caches (Mount_Info, FS_Info/blkid, LVM2_Info, LUKS_Info, SWRaid_Info, DMRaid, BCache_Info)
  └── CopyBlocks (block-level copy/move for file systems without their own tool)
Operation* (Create, Delete, ResizeMove, Copy, Format, Check, LabelFileSystem, ChangeUUID, NamePartition)
  └── OperationDetail (hierarchical, timestamped execution log; exported as HTML)
```

Key observations:

1. **Model objects.** `Device` holds a `PartitionVector` of `Partition` objects (with `PartitionLUKS`
   for encrypted content). Unallocated space is materialised as partitions of type
   `TYPE_UNALLOCATED`, which keeps the UI simple: both the graphic and the list iterate one vector.
   WParted mirrors this idea with `Segment`/`Row` (see `core/include/wparted/core/layout_view.hpp`).
2. **Capability model.** `FileSystem::get_filesystem_support()` returns an `FS` struct with one
   `Support` value per action (`NONE`, `GPARTED`, `LIBPARTED`, `EXTERNAL`) and separate online
   variants (`online_grow`, `online_shrink`, `online_read`, `online_write_label`). Support depends on
   which external programs are found in `PATH` at start-up. WParted adopts the *per-action* model but
   uses a richer, user-facing status vocabulary (`Supported`, `Limited`, `OfflineOnly`, ...) and ties it
   to tested capability, not to tool presence.
3. **Operation queue.** Operations are created in the UI, appended to a vector, and *merged* where
   possible (e.g. consecutive resize operations on the same partition). The visual layout shows the
   result of all pending operations. Nothing touches the disk until "Apply".
4. **Execution and calibration.** `GParted_Core::apply_operation_to_disk()` calls
   `calibrate_partition()` before every operation to re-read the real partition boundaries and
   detect changes since planning. This is GParted's main TOCTOU defence and WParted must do the
   equivalent (re-identify device and partition by stable IDs before each step).
5. **Resize/move order.** Shrink: check FS → shrink FS → shrink partition. Grow: grow partition →
   check FS → grow FS. Move uses either a file-system tool or `CopyBlocks` with a rollback attempt
   for the copied range on failure. These orderings are safety invariants for WParted.
6. **Alignment.** Three modes (`ALIGN_MEBIBYTE` default, `ALIGN_CYLINDER`, `ALIGN_STRICT`). MiB
   alignment also reserves the first MiB of the disk.
7. **Busy detection.** Mounted file systems, active swap, open LUKS mappings, active LVM volume
   groups and RAID members are "busy" and restrict operations; some actions (e.g. online grow) are
   allowed only while busy.
8. **Logging.** `OperationDetail` forms a tree of steps with status and elapsed time; the user can
   save it as HTML. WParted logs to `%LOCALAPPDATA%\WParted\logs` and will export a structured
   (JSON) operation log.
9. **Privilege model.** The whole GUI runs as root via `pkexec`. WParted deliberately differs: the GUI
   runs unelevated, modifying operations will run in a separate validated privileged process.

## Device discovery in GParted

- Device list from `/proc/partitions` (`Proc_Partitions_Info`) plus libparted probing, with
  device-mapper/RAID/bcache awareness.
- File system identification primarily via `blkid` (`FS_Info`), falling back to GParted's own
  signature checks and libparted.
- Usage (used/unused) via file-system tools (`dumpe2fs`, `ntfsinfo`, `xfs_db`, ...) or `statvfs`
  when mounted.

On Windows none of these mechanisms exist; see `windows-portability-analysis.md`.

## Testing approach

Googletest unit tests cover helper classes (`BlockSpecial`, `PipeCapture`, `PasswordRAMStore`,
`OperationDetail`), file-system support detection (`test_SupportedFileSystems` creates loop devices
and exercises each back end) and signature erasure. Many tests require root and loop devices;
`exclude_loopdev_tests.sh` disables them in restricted CI. WParted follows the same split: pure unit
tests run anywhere; device tests run only against image files or VHDs on dedicated runners.

## Licensing observations

- Program code: GPL-2.0-or-later (`COPYING`). Documentation: GFDL-1.2 (`COPYING-DOCS`).
- WParted is released under GPL-2.0-or-later. This keeps the option to reuse GParted code later
  (with attribution) open, but **no GParted code is reused at the moment**; all WParted code is
  original and derived from public specifications (UEFI, Microsoft FAT, on-disk format docs).
- The GParted manual must not be copied (GFDL is compatible with redistribution but imposes
  invariant-section/notice rules and would mix licences); WParted documentation is written from
  scratch.
- GNOME/GParted logos are not used.
