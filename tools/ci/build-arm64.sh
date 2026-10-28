#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 The WParted contributors
#
# Cross-compiles WParted for Windows on ARM64 with llvm-mingw (WP-015). The
# toolchain archive is pinned by its SHA-256 (as published by GitHub for the
# release asset). The binaries are built with warnings as errors and their
# machine type and imports are checked; they are NOT run, because CI has no
# ARM64 Windows or emulator. Usage: tools/ci/build-arm64.sh [--package]
set -euo pipefail
cd "$(dirname "$0")/../.."
VER=20260922
NAME=llvm-mingw-$VER-ucrt-ubuntu-22.04-x86_64
SHA=bb7bb7654b33d5aa8712acb837c963b2e0c56352560c76105270a3268c665c21

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends cmake ninja-build python3 zip curl xz-utils ca-certificates >/dev/null
curl -fsSL -o "/tmp/$NAME.tar.xz" "https://github.com/mstorsjo/llvm-mingw/releases/download/$VER/$NAME.tar.xz"
echo "$SHA  /tmp/$NAME.tar.xz" | sha256sum -c -
tar -xJf "/tmp/$NAME.tar.xz" -C /opt
export PATH="/opt/$NAME/bin:$PATH"
aarch64-w64-mingw32-clang++ --version | head -1

BUILD=build/arm64
cmake -S . -B "$BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/llvm-mingw-aarch64.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DWPARTED_WARNINGS_AS_ERRORS=ON
cmake --build "$BUILD"

# ARM64 PE files that only import Windows system DLLs (UCRT is part of Windows 10+).
for exe in "$BUILD/apps/wparted-gui/wparted.exe" "$BUILD/apps/wparted-cli/wparted-cli.exe"; do
  llvm-objdump -f "$exe" | grep -qi "coff-arm64" || { echo "$exe is not an ARM64 binary" >&2; exit 1; }
  imports="$(llvm-objdump -p "$exe" | awk '/DLL Name:/ {print tolower($3)}' | sort -u | tr '\n' ' ')"
  echo "$(basename "$exe") (ARM64) imports: $imports"
  for dll in $imports; do
    case "$dll" in
      kernel32.dll|user32.dll|gdi32.dll|advapi32.dll|shell32.dll|ole32.dll|comctl32.dll|comdlg32.dll|uxtheme.dll|dwmapi.dll|shlwapi.dll|oleaut32.dll|oleacc.dll|ucrtbase.dll|api-ms-win-*) ;;
      *) echo "unexpected runtime dependency: $dll" >&2; exit 1 ;;
    esac
  done
done
echo "llvm-mingw=$VER" > "$BUILD/toolchain.txt"

if [ "${1:-}" = "--package" ]; then
  mkdir -p dist
  out="dist/WParted-arm64-untested.zip"
  rm -f "$out"
  zip -j -q "$out" "$BUILD/apps/wparted-gui/wparted.exe" "$BUILD/apps/wparted-cli/wparted-cli.exe" LICENSE README.md
  echo "packaged $out (not run on ARM64 Windows)"
fi
