<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Building WParted

## Requirements

| Tool | Version | Notes |
|---|---|---|
| Visual Studio 2022 or Build Tools | 17.x, MSVC v143 (tested 14.44) | Workload "Desktop development with C++"; for ARM64 add "MSVC v143 – ARM64 build tools" |
| Windows SDK | 10.0.19041 or newer (tested 10.0.26100) | |
| CMake | ≥ 3.20 (tested 4.4) | `python -m pip install --user cmake` works |
| Ninja | ≥ 1.10 | `python -m pip install --user ninja` |
| Python | ≥ 3.8 | CI scripts, icon and SBOM generators |

## Build

```powershell
.\tools\build.ps1 -Arch x64                 # Release build + ctest
.\tools\build.ps1 -Arch x86
.\tools\build.ps1 -Arch x64 -Config Debug
.\tools\build.ps1 -Arch x64 -Package        # + portable ZIP and checksum in dist/
```

Outputs: `build/<arch>-<config>/apps/wparted-gui/wparted.exe`, `apps/wparted-cli/wparted-cli.exe`.

The core library and its unit tests also build on Linux with GCC/Clang (used by CI):

```sh
cmake -S . -B build/linux -G Ninja && cmake --build build/linux && ctest --test-dir build/linux
```

## Compiler settings

Static CRT (`/MT`), `/W4 /permissive- /sdl /guard:cf`, `/DYNAMICBASE /NXCOMPAT`, `/SAFESEH` on x86,
`_WIN32_WINNT=0x0601`. Warnings are treated as defects (CI fails on new warnings).
