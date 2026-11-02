<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# WParted FAQ

Answers about WParted 0.8.0-beta.1, a free partition manager for Windows 11. Auf Deutsch: [FAQ](FAQ.de.md).

## General

### What is WParted?
WParted is a free, open-source partition manager for Windows 11 with a GParted-style workflow. It shows disks and
partitions, plans changes, and applies them to disk image files and attached virtual disks.

### Is WParted free?
Yes. WParted is free of charge and licensed under the GNU GPL v2.0 or later.

### Is WParted a GParted alternative for Windows?
Yes, in its workflow, but not yet in its scope. It follows GParted's disk map, operation queue and apply step and runs
natively on Windows, while writing to real disks is still locked. It is an independent project, not part of GParted.

### Can I run GParted on Windows without a live USB?
Not GParted itself, because it needs Linux. WParted runs directly on Windows 11 and offers a similar workflow, so you can
inspect disks and plan changes without booting anything else.

### Which Windows versions does WParted support?
Windows 11 only. Windows 7, 8 and 10 are not supported or tested.

### Does WParted send data anywhere?
No. WParted has no telemetry, no automatic update check and no network use.

### Does WParted need an installer?
No. WParted is a portable ZIP: extract it and start `wparted.exe`.

## Safety

### Can WParted damage my real disks?
This version does not write to real disks, because that path is locked until a human security review is done. It writes
only to image files and attached virtual disks that you choose, after you confirm.

### Can WParted change the disk Windows is running from?
No. WParted never writes to a disk that holds the running Windows, its page file or its boot manager.

### Does WParted work with BitLocker?
WParted detects BitLocker and never changes, suspends or decrypts it. Plans that would change a protected volume are refused.

### What happens if the power fails during an operation?
An interrupted move can be resumed, because WParted keeps a journal. Steps that change data in place cannot be resumed:
WParted reports them, and you restore from a backup. There is no general undo after applying.

## Tasks

### How do I test partition changes safely on Windows?
Use a VHDX. Create and attach one in Windows, start WParted as administrator, select the virtual disk, plan the changes
and choose *Apply*. Your real disks are not touched.

### Can WParted resize NTFS partitions?
Yes, on image files and attached virtual disks. It grows and shrinks NTFS and moves data out of the removed area when
shrinking. A `$MFT` with an attribute list is moved too. WParted first simulates the whole move, so damaged records or runlists that no longer fit stop the shrink before anything changes.

### Can WParted move a partition?
Yes, on image files and attached virtual disks. It copies the data block by block, and an interrupted move can be resumed.

### Can WParted read ext4 and other Linux partitions on Windows?
It detects ext2/3/4, btrfs, XFS, swap, LUKS and LVM and shows their labels. It can create, resize and check ext2/3/4 and grow
btrfs and XFS on image files and virtual disks, but it does not let you open the files on them.

### Can WParted recover a deleted partition?
It can search free space for lost file systems and plan recreating the partitions. Scanning real disks is read-only and
needs administrator rights; recreating runs on image files and virtual disks.

### Can WParted repair a file system?
No. WParted checks read-only and leaves repairs to `chkdsk` and `e2fsck`, so that a repair never destroys data they could save.

### Does WParted have a command line?
Yes. `wparted-cli` lists disks with `--json`, inspects images, checks plans in a dry run and applies them with `--execute`.
See the [user guide](user/en/user-guide.md#command-line).

## Project

### Is WParted finished?
No, it is a beta. [Project status](PROJECT-STATUS.md) lists what is finished, what still needs a manual check, and what is open.

### Where do I report a bug?
Use [GitHub Issues](https://github.com/jtracht/WParted/issues). Security problems go through [SECURITY.md](../SECURITY.md).
