<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Windows support matrix

Only results that were actually observed are marked as verified. Everything else is an open check.

## v0.3.0-alpha (execution on image files only)

| Windows | Arch | Builds | Tests | GUI | Writes | Evidence / open check |
|---|---|---|---|---|---|---|
| Windows 11 Pro (26300) | x64 | ✔ MSVC + MinGW | ✔ 88/88 unit + CLI E2E; `wparted-cli` created and grew NTFS, ext4 and exFAT on image files (check after every write), deleted a partition and recreated it from the rescue scan | ✔ resume dialog after a crashed move, rescue dialog → *Add to the plan* | image files only ✔ | Local MSVC runs 2026-10-28, unelevated; boot manager / page file marks seen in `wparted-cli list` |
| Windows 11 | x86 build on x64 | ✔ MSVC + MinGW | ✔ 88/88 unit | – | image files only | Local MSVC run 2026-10-28 |
| Wine 9 (Ubuntu 24.04, CI) | x64 / x86 | ✔ MinGW | ✔ unit + CLI E2E | ✔ start-up smoke | image files only ✔ | Every CI run; Wine is not Windows |
| Linux CI (reference tools) | – | ✔ GCC (core) | ✔ e2fsck, dumpe2fs, debugfs, ntfsfix, ntfsls, ntfsresize, ntfs-3g, fsck.exfat, fsck.fat, mtools, sgdisk, sfdisk, blkid; fault injection | – | image files | Every CI run |
| Windows 11 | ARM64 | ✔ llvm-mingw cross build (warnings as errors, machine type and imports checked) | ☐ not run | ☐ | ☐ | No ARM64 Windows or emulator — WP-015 |
| Windows 10 / 8.x / 7 SP1 | x64 / x86 | expected | – | ☐ | ☐ | VM test — WP-015 |

**Elevated, real hardware (2026-10-28, Windows 11 Pro x64, administrator, read-only, run by the
maintainer with `tools/verify/elevated-readonly.ps1`):** NVMe SSD with GPT (ESP, MSR, Windows, WinRE)
and USB SSD with msdos/exFAT. Partition tables read by WParted with no diagnostics; every partition
offset equal to Windows' own view; ESP marked as boot manager, `C:` as Windows with page file;
BitLocker state "not encrypted" for `C:` and `E:`, matching `manage-bde -status`; read-only rescue
scan on both disks (one leftover 1 MiB FAT12 file system found at the end of the USB SSD); Windows'
view of disks, partitions and volumes identical before and after; discovery 83 ms.

BitLocker states other than "not encrypted" have not been observed on real hardware. NTFS
volumes created by WParted have not been checked by Windows' chkdsk (attaching an image needs
administrator rights).

## v0.2.0-alpha (planning; execution on image files only)

| Windows | Arch | Builds | Tests | GUI | Writes | Evidence / open check |
|---|---|---|---|---|---|---|
| Windows 11 Pro (26300) | x64 | ✔ MSVC + MinGW | ✔ 81/81 unit + CLI E2E incl. `apply-plan` | ✔ planning, apply to image file, rescue, information dialogs, managed-disk marker, MSAA (checked with an MSAA client), DE locale | image files only ✔ | Local MSVC runs 2026-10-27/28, unelevated |
| Windows 11 Pro (26300), published release ZIP | x64 | ✔ release pipeline (MinGW) | ✔ CLI from the published ZIP: `version`, `fs-support`, `apply-plan` dry run and `--execute` (FAT32 create on a GPT test image, read back), device path `\\.\PhysicalDrive99` refused | ☐ | image files only ✔ | Checked 2026-10-28 after publication, unelevated; checksums matched |
| Wine 9 (Ubuntu 24.04, CI) | x64 / x86 | ✔ MinGW | ✔ unit + CLI E2E | ✔ start-up smoke | image files only ✔ | Every CI run; Wine is not Windows |
| Linux CI (reference tools) | – | ✔ GCC (core) | ✔ results of WParted checked with sgdisk, sfdisk, fsck.fat, mtools, fsck.exfat, ntfs-3g, e2fsck, xfs_repair, btrfs check, blkid | – | image files | Every CI run |
| Windows 10 / 8.x / 7 SP1 | x64 / x86 | expected | – | ☐ | ☐ | VM test — WP-015 |
| Windows 11 | ARM64 | ✔ llvm-mingw cross build in CI (development builds after 0.2; warnings as errors, machine type and imports checked) | ☐ not run | ☐ | ☐ | No ARM64 Windows or emulator available — WP-015 |

Physical disks are never written by this version (CI-enforced read-only guard). The release binaries
are tested under Wine only before publication (maintainer decision for alphas).

## v0.1.0-alpha (read-only)

| Windows | Arch | Builds | Unit tests | GUI starts | Elevated disk read | Evidence / open check |
|---|---|---|---|---|---|---|
| Windows 11 Pro (26300) | x64 | ✔ MSVC + MinGW | ✔ 30/30 + CLI E2E | ✔ MSVC and MinGW builds (light + dark, DE locale) | ☐ not yet verified | Local run 2026-10-27 (unelevated: volumes-only mode verified) — WP-014 |
| Windows 11 | x86 build on x64 (WOW64) | ✔ MSVC + MinGW | ✔ 30/30 + CLI E2E | ✔ MinGW build | ☐ | Local run 2026-10-27 — WP-015 |
| Windows 11 | ARM64 | ✘ no ARM64 toolchain in CI | – | – | – | Possible via llvm-mingw (aarch64) — WP-015 |
| Wine 9 (Ubuntu 24.04, CI) | x64 / x86 | ✔ MinGW | ✔ | ✔ smoke | n/a | Every CI run; Wine is not Windows |
| Windows 10 22H2 | x64 / x86 | expected | – | ☐ | ☐ | VM test — WP-015 |
| Windows 8.1 | x64 / x86 | expected | – | ☐ | ☐ | VM test — WP-015 |
| Windows 8 | x64 / x86 | expected | – | ☐ | ☐ | VM test — WP-015 |
| Windows 7 SP1 | x64 / x86 | expected (`_WIN32_WINNT=0x0601`, static CRT) | – | ☐ | ☐ | VM test; MSVC 14.4x no longer officially tested for Win7 — WP-015 |

## API usage per Windows version

| Feature | API | Minimum | Fallback |
|---|---|---|---|
| Elevation check | `GetTokenInformation(TokenElevation)` | Vista | – |
| Sector sizes | `StorageAccessAlignmentProperty` | Vista | geometry bytes/sector |
| SSD detection | `StorageDeviceSeekPenaltyProperty` | 7 | "unknown" |
| Per-monitor DPI | manifest `PerMonitorV2`, `GetDpiForWindow`, `SystemParametersInfoForDpi` | 10 1607/1703 | system DPI (`dpiAware=true`) |
| Dark title bar | `DwmSetWindowAttribute(20/19)` | 10 1809 | light title bar |
| Dark controls | `SetWindowTheme("DarkMode_*")` | 10 1809 | classic theme colours |
| Task dialogs | `TaskDialogIndirect` (comctl32 v6) | Vista | – |
| DLL search hardening | `SetDefaultDllDirectories` | 8 / 7 + KB2533623 | skipped |

## Windows 7 / 8.x legacy notice

Windows 7 and 8.x no longer receive security updates. WParted keeps them as build targets because
partition tools are often used on old machines, but:
- support is best effort and limited to read-only functionality until verified,
- no modifying operation will be enabled for legacy systems without dedicated VM test evidence,
- users should prefer running WParted from a supported Windows or the recovery medium.
