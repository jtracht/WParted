<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Changelog

All notable changes are documented here. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- **Interrupted executions can be resumed.** A journal next to the image records the plan and the
  progress; after a crash, power loss or cancel during a block move or between operations, WParted
  continues exactly there (GUI: *Resume* when the image is opened; CLI: `wparted-cli resume`).
  Steps that change data in place are reported, not resumed. Tested in CI by killing the process in
  the middle of an overlapping move and resuming (e2fsck, sgdisk and payload hashes).
- **ext2/ext3/ext4 on image files**: create, grow, shrink (when the space behind the new end is free)
  and a read-only check, implemented natively. Grow and shrink also handle file systems made by
  mke2fs with its default features (flex_bg, 64bit, metadata_csum, resize_inode). Verified in CI with
  e2fsck, dumpe2fs, blkid and debugfs.
- Extended partitions can be moved (their boundaries; the EBR chain is rewritten).
- BitLocker state as reported by Windows (administrators only), the boot-manager volume and the page
  file are shown; new validation rules `op.bitlocker_active`, `op.bitlocker_unknown`,
  `op.bitlocker_recovery` and `op.bcd_offset` (moving the start of the Windows or boot-manager
  partition on msdos disks breaks booting).
- CI builds Windows on ARM64 binaries with llvm-mingw (not run: no ARM64 Windows in CI).

### Changed
- Image files are flushed to disk (`_commit`/`fsync`), not only to the C library buffer.

### Known limitations
- ext: no data relocation when shrinking; growing beyond the existing group descriptor blocks, and
  meta_bg, bigalloc, sparse_super2, MMP and external journals are refused. File systems created by
  WParted have no metadata checksums.
- The BitLocker state is only available to administrators; it has not been checked on a machine with
  BitLocker. ARM64 binaries have not been run.

## [0.2.0-alpha] – 2026-10-28

Second pre-release: GParted-style planning for every disk, and **execution on disk image files**.
Physical disks are still never written.

### Added
- GParted-style planning GUI: new, delete, resize/move, copy/paste, format, label, name, flags, new
  partition table; future-layout preview, pending operations, undo, Apply summary with safety checks,
  plan export/import with disk identity checks. Fluent-inspired redesign with one workflow from
  selection to apply; English and German throughout.
- Data rescue scan (read-only): searches unallocated space at MiB steps and legacy cylinder boundaries
  for lost file systems and reports start sector, type, size and label. `wparted-cli rescue-scan-image`
  and GUI *Device → Attempt Data Rescue…* (background thread). Recovery itself is not automated; the
  device scan has so far only been run on image files, not on real disks.
- Operation log model (plan → steps with status) with JSON and escaped HTML export; *Save details
  (HTML)…* in the Apply summary.
- Validation refuses plans on disks managed by Windows (dynamic disks, Storage Spaces, spanned volumes).
- Validation diagnostics carry the operation number and their parameters; the GUI shows operations,
  risks and validation messages in the UI language (English/German).
- High-contrast mode: the GUI uses the system colours.
- Project website (EN/DE) generated from the repository, checked in CI; not deployed yet.
- **Plans can be applied to disk image files** (GUI: *Apply to image file*; CLI: `apply-plan`, dry run
  by default): partition tables, create/delete, move and copy as raw blocks, resize of unformatted
  partitions, flags, names. Every step is read back and compared with the plan. Results are checked in
  CI with sgdisk, sfdisk, e2fsck and debugfs. Physical disks are still never written.
- **File-system operations on image files**, implemented natively from the on-disk specifications:
  FAT16/32 create, grow, shrink (with data relocation), read-only check, label and serial; NTFS grow,
  shrink (when the space behind the new end is free), label and serial; exFAT create, label and serial;
  ext2/3/4, XFS, btrfs and Linux swap label and UUID (metadata checksums stay valid via csum_seed,
  meta_uuid and metadata_uuid); copies of Linux file systems get a new UUID. Every operation is checked
  in CI with dosfstools, mtools, ntfs-3g (ntfsresize --check), exfatprogs, e2fsprogs, xfsprogs,
  btrfs-progs and util-linux.
- Used space of FAT and NTFS partitions on images (list columns; validation refuses shrinking below the
  data). Validation checks labels per file system and maximum FAT sizes.
- Accessibility: the disk map exposes its segments through MSAA.
- `wparted-cli fs-support` and *View → File System Support* list what can be changed on image files.
- Disks managed by Windows (dynamic disks, Storage Spaces) are marked and never planned on.
- Labels of NTFS, exFAT and F2FS (and the F2FS UUID) are read; detection is checked in CI against
  images made by the real mkfs tools, with blkid as reference.

### Security
- Physical disks are never written: the read-only guard in CI rejects any device write API and any file
  opened for writing outside 7 reviewed exemptions (report/plan/log files chosen by the user and the
  image-file executor, which refuses device paths and opens images exclusively).

### Known limitations
- **Only disk image files can be changed.** On physical disks WParted plans and validates, but does not
  execute (the privileged helper of ADR-0002 and a human security review come first).
- The binaries attached to the release were **not run on a real Windows machine before publication**
  (maintainer decision for alpha releases). They passed all tests and a GUI start-up smoke test under
  Wine in the release pipeline; an equivalent MSVC build was tested on Windows 11 x64 (unelevated),
  including planning and applying plans to image files in the GUI.
- Administrator mode is not verified on real hardware; Windows 7 to 10 and ARM64 are not verified;
  the binaries are unsigned.
- Not yet possible: creating or checking NTFS; resizing ext2/3/4, XFS or btrfs; moving an extended
  partition; repairing file systems (the FAT check is read-only). Such plans are refused before
  anything is written. There is no automatic rollback if writing to an image fails half-way.
- High-contrast mode maps to the system colours but has not been checked visually.

## [0.1.0-alpha] – 2026-10-27

First public pre-release: a **read-only** disk explorer. It cannot modify disks.

### Added
- Platform-independent core: GPT parser (CRC validation, backup fallback, sector-size mismatch
  detection), MBR parser with extended/logical chain and loop protection, whole-device file systems,
  overlap/bounds diagnostics.
- File system signature detection for 31 types with label/UUID extraction where cheap.
- Layout view model with unallocated gaps (≥ 1 MiB) and nested logical partitions.
- Windows discovery: physical disks, device properties, sector sizes, SSD/removable/write-protect,
  volumes with mount points and usage, Windows volume, cross-check with `IOCTL_DISK_GET_DRIVE_LAYOUT_EX`.
- Native Win32 GUI: GParted-style menus, toolbar, disk map, partition list, pending-operations pane,
  device and partition information, file system support overview, JSON report export, light/dark/system
  theme, per-monitor DPI, English and German UI, restart-as-administrator.
- `wparted-cli`: `list`, `inspect-image`, `fs-support`, `version`, JSON output.
- Tests: 30 unit tests incl. fuzzing; end-to-end CLI test that verifies images stay unmodified.
- CI tooling: read-only guard, SPDX check, portable packaging, CycloneDX SBOM.
- Release builds are cross-compiled on the Linux CI runner with MinGW-w64 (fully static, only Windows
  system DLLs incl. `msvcrt.dll`); unit, end-to-end and GUI smoke tests run under Wine. Toolchain
  licence texts ship in `licenses/`.

### Security
- The build contains no code path that writes to disks (enforced in CI).

### Known limitations
- The binaries attached to the release were not run on a real Windows machine before publication
  (maintainer decision). They passed all tests and a GUI start-up smoke test under Wine in the release
  pipeline; an equivalent build (same toolchain, commit 8385354) was run on Windows 11 x64 unelevated.
- **Administrator mode is not yet verified on real hardware.** Reading raw partition tables (WParted →
  Restart as Administrator, or an elevated `wparted-cli list`) is covered by tests on synthetic disk
  images and under Wine, but has not been checked against real disks (tracked in WP-014). Without
  administrator rights WParted shows only Windows volumes; unknown regions are hatched, never shown as free.
- Verified on Windows 11 x64 (x64 and x86 builds, without administrator rights). Windows 7, 8.1 and 10
  are build targets but have not been run yet (WP-015). No ARM64 build.
- Not code-signed; Windows SmartScreen may warn. Verify the SHA-256 checksums.
- No installer yet (portable ZIP only). Dark mode leaves the menu bar light; the disk map has no
  screen-reader provider yet (the partition list exposes the same data).
