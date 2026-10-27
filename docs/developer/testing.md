<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Testing

## Test layers

| Layer | Location | Runs on | What it proves |
|---|---|---|---|
| Unit (core) | `tests/unit/test_core.cpp` | any OS | GPT/MBR parsing, CRC, GUIDs, UTF-16, flags, FS signatures, layout gaps, JSON, fuzzing |
| End-to-end CLI | `tests/integration/cli_image_roundtrip.cmake` | Windows | CLI parses a synthetic GPT image, emits the expected JSON, and leaves the image byte-identical |
| Wine execution | `tools/ci/build-mingw.sh`, `tools/ci/gui-smoke-wine.sh` | Linux runner | Cross-built EXEs load and pass all tests under Wine; GUI creates its window and finishes discovery; EXEs import only Windows system DLLs |
| Static guards | `tools/ci/check_readonly.py`, `check_spdx.py` | any | No write APIs in production code; licence headers |
| Manual hardware | `docs/compatibility/windows-support-matrix.md` | Windows VMs / test PCs | Real discovery per Windows version |
| Image write tests (from v0.3) | planned, WP-031 | Linux runner (native + Wine) | Every layout write is first executed by the core against image files through a `BlockWriter` abstraction and verified by re-parsing |
| VHDX integration (from v0.3) | `tests/integration/vhd/` (planned, WP-031) | Manual, elevated Windows machine (no Windows runner exists) | Windows IOCTL write path on attached VHDX with read-back verification; mandatory before a write feature leaves `Experimental` |

Synthetic images are built in memory by `tests/unit/image_builder.hpp` (protective MBR, primary and
backup GPT with valid CRCs, EBR chains, FS signatures). **No test touches a physical disk.**

## Running

```powershell
.\tools\build.ps1 -Arch x64     # includes ctest
build\x64-release\tests\wparted_core_tests.exe
```

## Current results (2026-10-27)

| Build | Where | core_unit_tests | cli_image_roundtrip | GUI |
|---|---|---|---|---|
| MSVC x64 Release | Windows 11 host | PASS (30/30) | PASS | starts (manual) |
| MSVC x86 Release | Windows 11 host | PASS (30/30) | PASS | - |
| MinGW x64 (CI build) | Wine in runner image | PASS | PASS | smoke PASS |
| MinGW x86 (CI build) | Wine in runner image | PASS | PASS | smoke PASS |
| MinGW x64/x86 (CI build) | **real Windows 11** | - | CLI `list`/`--json` OK | starts, discovery OK, clean exit |
| Native GCC (core) | Linux runner image | PASS | - | - |

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
