<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Windows support matrix

Only results that were actually observed are marked as verified. Everything else is an open check.

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
