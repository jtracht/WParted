<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Changelog

All notable changes are documented here. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [Semantic Versioning](https://semver.org/).

## [Unreleased] – 0.1.0-alpha

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
