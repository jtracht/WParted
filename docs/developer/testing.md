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

Reproduce with `python tools/benchmark.py --build build/x64-release --runs 5 --image-gib 16`
(read-only; synthetic images in a temporary directory; `--json` for machine-readable output).

Baseline 2026-10-27, v0.2.0-alpha.dev (commit after 6328b35): Windows 11 Pro 10.0.26300, Samsung
PM981 1 TB NVMe + SanDisk Portable SSD (USB), MSVC x64 Release, unelevated. Times include process
start-up; median of 5 runs unless stated.

| Metric | Method | Result |
|---|---|---|
| Disk discovery (unelevated, volumes only) | `wparted-cli list --json` | 10.4 ms |
| Analyse 64 MiB GPT image (4 partitions) | `wparted-cli inspect-image` | 9.3 ms |
| Rescue scan of 16 GiB unallocated space | `wparted-cli rescue-scan-image` | 116 ms |
| GUI start until the main window exists | window polling, 1 run | 21 ms |
| GUI private memory 3 s after start | `GetProcessMemoryInfo` PrivateUsage | 2.6 MiB |
| Executable sizes | `wparted.exe` / `wparted-cli.exe` x64 | 714 KiB / 345 KiB (static CRT) |

The rescue scan reads a few probe sectors per MiB step, and the benchmark image is a sparse file
served from the cache. The figure therefore measures the scan logic, not disk throughput; on a real
disk seek latency dominates. Elevated discovery and scans of physical disks are not benchmarked: the
maintainer decided against hardware tests for now (WP-014).
