<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# WParted user guide (v0.4.0-alpha)

## What is WParted?

WParted is a free, open-source partition manager for Windows. Version 0.1 is a **read-only disk
explorer**: it shows your disks, partition tables, file systems and volumes in a GParted-style view.
It **does not change anything** on your disks.

## Installation and first start

1. Download `WParted-v0.1.0-alpha-win7plus-x64-portable.zip` (or `-x86` for 32-bit Windows) from the
   Gitea releases page and verify the SHA-256 checksum from `…-checksums.sha256`.
2. Extract the ZIP to any folder. No installation or runtime is required.
3. Run `wparted.exe`.

Without administrator rights WParted shows only the volumes Windows has mounted, and the regions
between them as **unknown (hatched)**. To read the real partition tables, click
**Restart as Administrator** in the yellow banner (or *WParted → Restart as Administrator*).

## The main window

| Area | Purpose |
|---|---|
| Toolbar | Refresh, and the partition actions known from GParted. Actions that are not available yet are greyed out; hover for an explanation. |
| Device selector | Choose the disk to display. `[Windows]` marks the disk containing the running Windows. |
| Disk map | Each box is a partition, sized proportionally. Border colour = file system, yellow fill = used space. Grey = unallocated, hatched = unknown. Use ←/→ to move, Enter for details. |
| Partition list | Partition, name, file system, mount point, label, size, used, unused, flags and status. Double-click or Enter shows details; right-click opens the partition menu. |
| Details pane | Properties of the selected partition or free space, and why an action is not available. |
| Pending operations | The planned changes in order, with risk and validation status (development builds). |
| Status line | Scan result, warnings found in the partition table, and whether WParted runs with administrator rights. |

## Planning operations

Development builds can **plan** changes the GParted way. Nothing is written: this version cannot
execute operations on physical disks, and *Apply* only shows the checks. On **disk image files**
plans can be applied (see below).

1. Select a partition or unallocated space in the disk map or the list.
2. Choose an action: *New*, *Delete*, *Resize/Move*, *Copy*/*Paste*, *Format to*, *Name Partition*,
   *Label File System*, *New UUID*, *Manage Flags*, *Check*, or *Device → Create Partition Table*.
   Dialogs show a preview of the resulting layout; sizes are entered in MiB, alignment is 1 MiB by
   default (*Align to: None* skips rounding up to a MiB boundary).
3. The disk map shows the **future layout**; *Pending operations* lists every step. *Undo* (Ctrl+Z)
   removes the last one, *Edit → Clear All Operations* removes all.
4. *Apply* (Ctrl+Enter) validates the plan and shows a summary with risks and blocking errors
   (for example system partitions, busy or encrypted volumes, partitions smaller than their data,
   overlaps, MBR limits). From there you can **export the plan** (JSON) or **save details (HTML)**.
5. *Edit → Save Plan* (Ctrl+S) stores the plan. It records the disk identity (serial, size, table
   GUID/signature), so a plan cannot silently be applied to a different disk.

To try this without touching a real disk, use *WParted → Open Disk Image* (Ctrl+O) with a raw image
file (`.img`).

## Applying a plan to a disk image file

For an opened image file, *Apply* validates the plan for the image and, if it passes, offers
**Apply to image file…**. A second confirmation names the file; there is no automatic undo, so copy
the file first if you may need its current state. WParted then

- writes each operation and **reads the partition table back** before the next one (the first
  mismatch stops the plan),
- moves and copies partitions as raw blocks (any file system, same size),
- clears old file system signatures at the start of new partitions,
- shows every step in the result and re-reads the image.

Possible on images:

| | create | grow | shrink | move / copy | check | label | new UUID |
|---|---|---|---|---|---|---|---|
| FAT16 / FAT32 | ✓ | ✓ | ✓ (data is relocated) | ✓ | ✓ read-only | ✓ | ✓ |
| NTFS | ✓ simplified system files¹ | ✓ | ✓ if the space behind the new end is free | ✓ | ✓ read-only | ✓ | ✓ |
| exFAT | ✓ | ✓ while the FAT has room² | ✓ if the space behind the new end is free | ✓ | – | ✓ | ✓ |
| ext2 / ext3 / ext4 | ✓ without metadata checksums | ✓ within the descriptor blocks | ✓ if the space behind the new end is free | ✓ | ✓ read-only | ✓ | ✓ |
| Linux swap | – | – | – | ✓ | – | ✓ | ✓ |
| any other | – | – | – | move ✓, copy where safe | – | – | – |

Also: new partition tables, new or deleted partitions, flags and names. Moves and copies are raw block
operations of the same size; a copy into a larger FAT partition grows the copied file system. Plans
with steps WParted cannot do are refused before anything is written, as are shrinks of NTFS volumes
with data behind the new end and file systems with errors. Physical disks are never written by this
version.

¹ Verified with ntfs-3g; Windows' chkdsk has not checked it yet and may complete `$Secure`, `$Extend`
and the up-case table. ² exFAT volumes WParted creates leave room to grow.

Extended partitions can be moved too (their boundaries; the logical partitions
stay where they are).

### Interrupted executions

WParted keeps a journal next to the image (`<image>.wparted-journal`) while it applies a plan. If the
program, the computer or the power fails, or you cancel during a move, the journal shows where it
stopped:

- **between operations** or **during a block move or copy**: WParted can continue exactly there.
  The GUI offers *Resume* when you open the image again; on the command line run
  `wparted-cli resume <image>` to see the state and `--execute` to continue. Until then, the data of
  an interrupted move is split between the old and the new position – do not use the image.
- **during a step that changes data in place** (file-system resize or creation, writing the table):
  this cannot be resumed; WParted says so and the image may be inconsistent. Restore it from your copy.

*Discard the journal* (or `--discard`) deletes only the journal, never the image. Details:
[failure modes](../../architecture/failure-modes.md).

## Data rescue

*Device → Attempt Data Rescue* searches the unallocated space of the selected disk or image for file
systems that no partition covers any more (for example after a partition was deleted). It checks
1 MiB steps and old cylinder boundaries, runs in the background and only reads.

The result lists start sector, file system, size and label, and whether the start is MiB-aligned.
**Add to the plan** recreates the partitions exactly around the found file systems (also at non-MiB
starts). Nothing inside them is written; after the table is written each file system is detected and
checked again. Review the plan, keep a copy, then apply it (image files) – the command line does the
same with `wparted-cli rescue-scan-image <image> --plan rescue.json` and `apply-plan`. Scanning a
physical disk needs administrator rights and has not been tried on real hardware yet.

## Useful functions

- **View → Device Information**: model, serial, interface, sector sizes, SSD, write protection, table
  type, GPT header health and all diagnostics.
- **Partition → Information**: sector range, alignment (1 MiB and physical sector), GUIDs, attributes,
  flags, mount points and usage.
- **View → File System Support**: what this build can do for each file system.
- **Device → Export Report**: saves a JSON report of all disks (useful for bug reports).
- **View → Theme**: System, Light or Dark.
- **Tools → Open Log Folder**: `%LOCALAPPDATA%\WParted\logs`.

## Command line

```text
wparted-cli list [--json]                       list disks, partitions, volumes
wparted-cli inspect-image disk.img [--sector-size 4096] [--json]
wparted-cli fs-support [--json]
wparted-cli rescue-scan-image disk.img [--sector-size 4096] [--json]
wparted-cli apply-plan disk.img plan.json [--execute] [--log details.html]   check, or apply to the image
wparted-cli apply-plan disk.img plan.json --execute --helper  the same through wparted-helper.exe (prototype)
wparted-cli resume disk.img [--execute | --discard]          show or continue an interrupted execution
wparted-cli rescue-scan-image disk.img --plan rescue.json      plan recreating the partitions found
wparted-cli rescue-scan 0                                      read-only rescue scan of disk 0 (administrator)
wparted-cli version
```

Run from an administrator prompt to include partition tables. Exit code of `inspect-image` is 1 if the
table has errors.

## Frequently asked questions

**Is WParted free?** Yes, GPL-2.0-or-later.
**Can WParted resize or delete partitions?** Not yet. Development builds can plan and validate these
operations, but not execute them. Execution will be enabled only after it passes tests on virtual disks.
**Does WParted support Windows 7?** Windows 7 SP1 is a build target; it has not yet been verified on a
real Windows 7 system.
**Can WParted read Linux partitions?** It detects ext2/3/4, btrfs, XFS, swap, LUKS, LVM and more, and
shows their labels where possible. Changing them will require the planned offline mode.
**Is it related to GParted?** No. WParted follows GParted's workflow but is an independent project.
**Where do I report bugs?** https://git.tracht-digital.de/jtracht/WParted/issues — attach the exported
report if possible.

## Troubleshooting

| Symptom | Cause / action |
|---|---|
| "Partition tables cannot be read" banner | Restart as administrator. |
| Warning "sector size mismatch" | The USB enclosure reports a different sector size than the one used when the disk was partitioned. Do not repartition the disk through this enclosure. |
| Warning "backup GPT header invalid" | Common after cloning to a larger disk. WParted does not repair it in this version. |
| Warning "Windows reports partitions that differ" | Refresh. If it persists, report it with the exported JSON. |
