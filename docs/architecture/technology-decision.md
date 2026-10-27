<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Technology decision (ADR-0001)

Status: **Accepted** (2026-10-27). Revisit when Windows 7/8.1 support is dropped.

## Context

WParted must run on Windows 7 SP1 through Windows 11 (x86, x64; ARM64 when feasible), start fast,
use little memory, avoid runtime dependencies, and be maintainable for years. It manipulates disks,
so the core logic must be testable without hardware.

## Options evaluated

| Criterion | Win32 + GDI/Direct2D (C++17) | Qt 5.15 LTS | Qt 6.x | WinUI 3 / Windows App SDK | .NET (WPF/WinForms) |
|---|---|---|---|---|---|
| Windows 7 SP1 | ✔ (all APIs used are Win7 or resolved dynamically) | ✔ (last line supporting Win7; commercial LTS only after 2023) | ✘ (Qt 6 requires Windows 10) | ✘ (Win10 1809+) | ✔ only with .NET Framework 4.8 |
| Windows 11 / ARM64 | ✔ | ✔ x64; ARM64 limited | ✔ | ✔ | ✔ |
| Runtime dependencies | none (static CRT, system DLLs) | ~20–30 MB Qt DLLs | ~30 MB | Windows App SDK runtime | .NET runtime |
| Cold/warm start | very fast (measured: window + scan < 0.2 s CPU) | fast | fast | moderate | moderate |
| Idle RAM | ~2 MB private / ~16 MB WS (measured) | 30–60 MB | 40–80 MB | 60+ MB | 40+ MB |
| Accessibility | MSAA/UIA via common controls; custom controls need providers | good (UIA bridge) | good | excellent | good |
| HiDPI / dark mode | manual (per-monitor v2 manifest, DWM attribute, owner-draw) | built in | built in | built in | partial |
| Licence | Windows SDK only | LGPL-3 (dynamic linking) / commercial | LGPL-3 / commercial | MIT | MIT |
| Maintainability | more UI code by hand | very good | very good | good | good |

Measurements: Win32 prototype of this repository on Windows 11 Pro 26300, Samsung PM981 NVMe,
Release x64 build, process sampled 3 s after start (see `docs/developer/testing.md#benchmarks`).
Qt/WinUI numbers are typical published figures, not measured here, and are only used for relative
comparison.

## Decision

1. **C++17, CMake, MSVC v143**, static CRT (`/MT`), `_WIN32_WINNT=0x0601`.
2. **Native Win32 UI** (common controls v6, GDI double-buffered custom disk map). Direct2D was
   considered for the disk map; GDI is sufficient for a handful of rectangles, available everywhere
   (including Windows PE), and avoids D2D device-loss handling. Direct2D can be introduced behind
   the same control interface later if profiling shows a need.
3. **One UI for all Windows versions.** Newer features (dark title bar, per-monitor DPI APIs,
   `SystemParametersInfoForDpi`) are loaded with `GetProcAddress` and degrade gracefully. No separate
   legacy/modern front ends are needed, so no logic is duplicated.
4. **Platform-independent core** (`core/`, no Windows headers) with all parsing, layout,
   validation and (later) operation planning. It builds and tests on Linux CI as well.
5. **Windows platform layer** (`platforms/windows/storage-api`) for discovery and, later, the
   privileged executor.
6. **Privilege separation** (from v0.3): unelevated GUI, elevated helper (`wparted-helper.exe`) started
   on demand via UAC, named-pipe IPC with a restrictive DACL, server-side validation of a declarative
   operation plan. See `security-architecture.md`.
7. **Linux file system modification** only in an optional offline/recovery environment
   (ADR to follow in WP-070).

8. **Two compilers, one code base** (amended 2026-10-27, ADR-0001a). The Gitea instance has exactly
   one Actions runner - a Linux Docker runner (`ubuntu-latest`) - and no Windows runner will be added.
   Therefore:
   - **Release binaries** are cross-compiled on that runner with **MinGW-w64 GCC** (posix threads),
     linked fully static against `msvcrt.dll`, which exists on every Windows version (keeps Windows 7
     in reach). CI asserts that the EXEs import only Windows system DLLs.
   - Unit tests, the CLI end-to-end test and a GUI start-up smoke test run under **Wine** in the same
     job. Wine is not Windows: it proves the binaries load and the logic works, not that real disk access
     behaves like on Windows. Real-Windows checks stay a manual release gate.
   - **MSVC** remains supported for local development (`tools/build.ps1`) and as a second compiler that
     catches portability issues. Both compilers must stay warning-free.
   - Trade-off: GCC has no equivalent of MSVC `/guard:cf`; ASLR/DEP/high-entropy-VA are enabled via
     linker flags.

## Consequences

- No third-party runtime DLLs → minimal supply-chain surface; the SBOM lists the statically linked
  mingw-w64 CRT/winpthreads and GCC runtime libraries, whose licence texts ship in `licenses/`.
- UI work is more manual (owner-drawn buttons, dark theme quirks). Known gaps are tracked
  (menu bar theming, UIA provider for the disk map).
- **Toolchain risk for Windows 7:** release builds use MinGW-w64 against `msvcrt.dll` (present on
  Windows 7); local MSVC 14.4x builds still target Windows 7, but Microsoft no longer tests this. Binaries must be verified on real Windows 7 SP1 /
  8.1 VMs before the release notes claim support (WP-015). Until then the support matrix lists them
  as "build targets, runtime unverified".
