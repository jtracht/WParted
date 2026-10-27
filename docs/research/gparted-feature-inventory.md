<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# GParted feature inventory

Source of truth: GParted source at commit `d6e8360` (see `gparted-architecture.md`), cross-checked
with https://gparted.org/features.php. Capabilities below were extracted from each
`<fs>::get_filesystem_support()` implementation; they are *conditional* on the listed external
tools being installed.

## 1. Device and partition table level

| ID | Function | Notes |
|---|---|---|
| G-DEV-01 | Scan/refresh all devices | `Refresh Devices` (Ctrl+R) |
| G-DEV-02 | Select device | Device combo + `GParted > Devices` menu |
| G-DEV-03 | Device information | Model, serial, size, path, table type, heads/sectors/cylinders, sector size |
| G-DEV-04 | Create partition table | `Device > Create Partition Table`; types offered by libparted (msdos, gpt, and others such as aix, amiga, bsd, dvh, loop, mac, pc98, sun, atari) |
| G-DEV-05 | Attempt data rescue | Searches for lost file systems (uses `gpart`); rescue is read-only |
| G-DEV-06 | Sector sizes | 512, 1024, 2048, 4096 and larger |
| G-DEV-07 | Device types | HDD (SATA/IDE/SCSI), SSD, NVMe, USB, SD, hardware/BIOS/Linux software RAID, device-mapper |

## 2. Partition operations (pending-operation model)

| ID | Function | Operation class |
|---|---|---|
| G-OP-01 | New partition (primary/extended/logical, size, alignment, FS, label, name) | `OperationCreate` |
| G-OP-02 | Delete partition | `OperationDelete` |
| G-OP-03 | Resize/move (graphical slider + numeric fields: free before, new size, free after) | `OperationResizeMove` |
| G-OP-04 | Copy and paste a partition (into unallocated space or over a partition) | `OperationCopy` |
| G-OP-05 | Format to file system | `OperationFormat` |
| G-OP-06 | Check (and repair) file system; grows FS to fill partition | `OperationCheck` |
| G-OP-07 | Label file system | `OperationLabelFileSystem` |
| G-OP-08 | New UUID | `OperationChangeUUID` |
| G-OP-09 | Name partition (GPT and other tables supporting names) | `OperationNamePartition` |
| G-OP-10 | Manage flags (boot, esp, hidden, lba, lvm, raid, swap, msftres, msftdata, diag, bios_grub, legacy_boot, irst, prep, chromeos_kernel, bls_boot, linux-home, no_automount, ...) | applied immediately via libparted (not queued) |
| G-OP-11 | Undo last operation (Ctrl+Z) | queue |
| G-OP-12 | Clear all operations | queue |
| G-OP-13 | Apply all operations (Ctrl+Enter) with confirmation and progress dialog | `Dialog_Progress` |
| G-OP-14 | Save operation details as HTML | `OperationDetail` |
| G-OP-15 | Alignment: MiB (default), cylinder, none | `PartitionAlignment` |
| G-OP-16 | Operation merging (consecutive ops on the same partition) | `Win_GParted::merge_operation` |
| G-OP-17 | Calibration before each step (re-read real boundaries) | `GParted_Core::calibrate_partition` |

## 3. Content / volume management

| ID | Function | Notes |
|---|---|---|
| G-VOL-01 | Mount / unmount file system | `Partition > Mount on / Unmount` |
| G-VOL-02 | Swapon / swapoff | linux-swap |
| G-VOL-03 | Open / close LUKS encryption | passphrase dialog, `PasswordRAMStore` (locked memory) |
| G-VOL-04 | Activate / deactivate LVM volume group | lvm2 pv |
| G-VOL-05 | Partition information dialog | usage graph, FS, label, UUID, path, flags, sector range, LUKS/LVM details |
| G-VOL-06 | Show used/unused space | via FS tools or `statvfs` |
| G-VOL-07 | Busy detection (mounted, swap active, LUKS open, VG active, RAID member) | |
| G-VOL-08 | Warnings on partitions (e.g. FS larger than partition, unreadable FS) | yellow warning icon |

## 4. File system capability matrix (GParted, from source)

Legend: ✔ supported (offline), ⓞ also/only online (while mounted/active), L = via libparted, — not supported.

| FS | Detect | Usage | Create | Grow | Shrink | Move | Copy | Check | Label | UUID | Tools |
|---|---|---|---|---|---|---|---|---|---|---|---|
| bcachefs | ✔ | ⓞ | ✔ | ✔ⓞ | — | ✔ | ✔ | ✔ | ✔ⓞ | read | bcachefs |
| btrfs | ✔ | ✔ⓞ | ✔ | ✔ⓞ | ✔ⓞ | ✔ | ✔ | ✔ | ✔ⓞ | ✔ | btrfs-progs |
| exfat | ✔ | ✔ | ✔ | — | — | ✔ | ✔ | ✔ | ✔ | ✔ | exfatprogs |
| ext2/3/4 | ✔ | ✔ⓞ | ✔ | ✔ⓞ | ✔ | ✔ | ✔ | ✔ | ✔ⓞ | ✔ | e2fsprogs |
| f2fs | ✔ | ✔ | ✔ | ✔ | — | ✔ | ✔ | ✔ | — | — | f2fs-tools |
| fat16/32 | ✔ | ✔ | ✔ | L | L | ✔ | ✔ | ✔ | ✔ | ✔ | dosfstools, mtools |
| hfs | ✔ | L | ✔ | — | L | ✔ | ✔ | — | — | — | hfsutils |
| hfs+ | ✔ | L | ✔ | — | L | ✔ | ✔ | ✔ | — | — | hfsprogs |
| jfs | ✔ | ✔ⓞ | ✔ | ✔ (mounted) | — | ✔ | ✔ | ✔ | ✔ | ✔ | jfsutils |
| linux-swap | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | — | ✔ | ✔ | util-linux |
| luks | ✔ | ✔ⓞ | — | ✔ⓞ | ⓞ | ✔ | ✔ | — | — | — | cryptsetup, dmsetup |
| lvm2 pv | ✔ | ✔ | ✔ | ✔ⓞ | ✔ⓞ | ✔ | — | ✔ | — | — | lvm2 |
| minix | ✔ | ⓞ | ✔ | — | — | ✔ | ✔ | ✔ | — | — | util-linux |
| nilfs2 | ✔ | ✔ | ✔ | ✔ (mounted) | ✔ (mounted) | ✔ | ✔ | — | ✔ | ✔ | nilfs-utils |
| ntfs | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ (ntfsclone) | ✔ | ✔ | ✔ | ntfs-3g/ntfsprogs |
| reiser4 | ✔ | ✔ | ✔ | — | — | ✔ | ✔ | ✔ | — | read | reiser4progs |
| reiserfs | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | reiserfsprogs |
| udf | ✔ | ✔ⓞ | ✔ | — | — | ✔ | ✔ | — | ✔ | ✔ | udftools |
| xfs | ✔ | ✔ | ✔ | ✔ (mounted) | — | ✔ | ✔ (xfsdump) | ✔ | ✔ⓞ | ✔ | xfsprogs, xfsdump |
| apfs, bitlocker, refs, ufs, zfs, linux-raid, linux-suspend | ✔ | — | — | — | — | ✔ | ✔ | — | — | — | detection (blkid); move/copy by block copy |

Notes: ext grow/shrink use `resize2fs`; NTFS resize uses `ntfsresize`; FAT resize uses
`libparted-fs-resize`; copy for FS without a dedicated tool uses GParted's internal block copier.

## 5. User interface inventory

- **Main window:** menu bar (GParted, Edit, View, Device, Partition, Help), toolbar (New, Delete,
  Resize/Move, Copy, Paste, Undo, Apply), device combo box, disk graphic, partition list
  (Partition, Name, File System, Mount Point, Label, Size, Used, Unused, Flags), collapsible pending
  operations pane, status bar.
- **View menu:** Device Information side pane, Pending Operations, File System Support dialog.
- **Dialogs:** New Partition, Resize/Move, Paste (copy), Partition Information, Manage Flags,
  Label File System, Name Partition, Create Partition Table, Apply progress with details, LUKS
  passphrase, File System Support, About.
- **Keyboard shortcuts (verified in `Win_GParted.cc`):** Ctrl+Enter apply, Insert new, Delete delete;
  stock accelerators for refresh/quit/undo/copy/paste/help.
- **Context menu:** right-click on a partition in the graphic or list shows the Partition menu.

## 6. Known limitations relevant to WParted

- Online operations depend on kernel support; many operations require unmounting.
- No resize for exFAT; XFS cannot shrink; copy is unavailable for LVM PVs.
- Flag changes are applied immediately rather than queued.
- GParted runs entirely as root; any UI bug runs with full privileges.
- Open issues are tracked at https://gitlab.gnome.org/GNOME/gparted/-/issues; WParted's parity
  matrix only claims behaviour verified in the referenced source snapshot.
