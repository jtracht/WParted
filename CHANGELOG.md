<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Changelog

All notable changes are documented here. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
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
  ext2/3/4 and Linux swap label and UUID (ext4 keeps metadata checksums valid via csum_seed). Every operation is checked in CI
  with dosfstools, mtools, ntfs-3g (ntfsresize --check), exfatprogs, e2fsprogs and util-linux.
- Used space of FAT and NTFS partitions on images (list columns; validation refuses shrinking below the
  data). Validation checks labels per file system and maximum FAT sizes.
- Accessibility: the disk map exposes its segments through MSAA.
- Disks managed by Windows (dynamic disks, Storage Spaces) are marked and never planned on.
- Labels of NTFS, exFAT and F2FS (and the F2FS UUID) are read; detection is checked in CI against
  images made by the real mkfs tools, with blkid as reference.

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
