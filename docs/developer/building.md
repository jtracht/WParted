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

### Release build (as in CI): MinGW-w64 cross build on Linux

```sh
tools/ci/build-mingw.sh x64 --package   # or x86; needs Ubuntu 24.04 (or the runner image)
```

Toolchains: `cmake/toolchains/mingw-x86_64.cmake`, `mingw-i686.cmake`. Tests run through Wine
(`CMAKE_CROSSCOMPILING_EMULATOR`). Output: `build/mingw-<arch>/`, packages in `dist/`.

### Portable core on Linux

The core library and its unit tests also build natively on Linux with GCC/Clang (used by CI):

```sh
cmake -S . -B build/linux -G Ninja && cmake --build build/linux && ctest --test-dir build/linux
```

## Compiler settings

- MSVC: static CRT (`/MT`), `/W4 /permissive- /sdl /guard:cf`, `/DYNAMICBASE /NXCOMPAT`, `/SAFESEH` on x86.
- MinGW-w64: `-Wall -Wextra -Wpedantic`, fully static (`-static`), `--dynamicbase --nxcompat`
  (+ `--high-entropy-va` on x64), stripped Release binaries; C runtime `msvcrt.dll`.
- Both: `_WIN32_WINNT=0x0601`; `-DWPARTED_WARNINGS_AS_ERRORS=ON` in CI.
