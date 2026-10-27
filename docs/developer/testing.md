<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Testing

## Test layers

| Layer | Location | Runs on | What it proves |
|---|---|---|---|
| Unit (core) | `tests/unit/test_core.cpp` | any OS | GPT/MBR parsing, CRC, GUIDs, UTF-16, flags, FS signatures, layout gaps, JSON, fuzzing |
| End-to-end CLI | `tests/integration/cli_image_roundtrip.cmake` | Windows | CLI parses a synthetic GPT image, emits the expected JSON, and leaves the image byte-identical |
| Static guards | `tools/ci/check_readonly.py`, `check_spdx.py` | any | No write APIs in production code; licence headers |
| Manual hardware | `docs/compatibility/windows-support-matrix.md` | Windows VMs / test PCs | Real discovery per Windows version |
| VHD integration (from v0.3) | `tests/integration/vhd/` (planned, WP-031) | Windows runner | Write operations on attached VHDX with read-back verification |

Synthetic images are built in memory by `tests/unit/image_builder.hpp` (protective MBR, primary and
backup GPT with valid CRCs, EBR chains, FS signatures). **No test touches a physical disk.**

## Running

```powershell
.\tools\build.ps1 -Arch x64     # includes ctest
build\x64-release\tests\wparted_core_tests.exe
```

## Current results (2026-10-27, Windows 11 x64 host)

| Build | core_unit_tests | cli_image_roundtrip |
|---|---|---|
| x64 Release | PASS (30/30) | PASS |
| x86 Release | PASS (30/30) | PASS |

## Benchmarks

Reference system: Windows 11 Pro 10.0.26300, Samsung PM981 1 TB NVMe + SanDisk Portable SSD (USB),
x64 Release build, unelevated.

| Metric | Method | Result |
|---|---|---|
| Discovery time | `wparted-cli list` (reported) | 2–3 ms for 2 disks (volumes-only mode) |
| GUI private memory 3 s after start | `Get-Process` PrivateMemorySize64 | 2.2 MB |
| GUI working set | `Get-Process` WorkingSet64 | 15.7 MB |
| GUI CPU time to idle | `TotalProcessorTime` 3 s after start | 0.11 s |
| Executable size | `wparted.exe` x64 | 467 KiB (static CRT) |

Elevated discovery (raw table reads) has not been benchmarked yet (WP-018).
