#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Cross-builds the Windows binaries on Linux (MinGW-w64), runs all tests
# through Wine and optionally packages the portable ZIP.
#
#   tools/ci/build-mingw.sh <x64|x86> [--package] [--no-deps]
#
# Used by .gitea/workflows/*.yml on the Linux Docker runner and locally via
#   docker run --rm -v "$PWD:/src" -w /src docker.gitea.com/runner-images:ubuntu-latest \
#       tools/ci/build-mingw.sh x64 --package
set -euo pipefail

ARCH="${1:?usage: build-mingw.sh <x64|x86> [--package] [--no-deps]}"
shift
PACKAGE=0
DEPS=1
for a in "$@"; do
  case "$a" in
    --package) PACKAGE=1 ;;
    --no-deps) DEPS=0 ;;
    *) echo "unknown option $a" >&2; exit 2 ;;
  esac
done

case "$ARCH" in
  x64) TOOLCHAIN=cmake/toolchains/mingw-x86_64.cmake ;;
  x86) TOOLCHAIN=cmake/toolchains/mingw-i686.cmake ;;
  *) echo "unsupported arch $ARCH" >&2; exit 2 ;;
esac

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

if [ "$DEPS" -eq 1 ]; then
  export DEBIAN_FRONTEND=noninteractive
  $SUDO dpkg --add-architecture i386
  $SUDO apt-get update -qq
  $SUDO apt-get install -y -qq --no-install-recommends \
    cmake ninja-build python3 zip \
    binutils-mingw-w64 g++-mingw-w64-x86-64-posix g++-mingw-w64-i686-posix \
    wine wine64 wine32:i386 >/dev/null
fi

# Quiet, non-interactive Wine prefix (no Mono/Gecko installer prompts).
export WINEDEBUG=-all
export WINEDLLOVERRIDES="mscoree,mshtml="
export WINEPREFIX="${WINEPREFIX:-$HOME/.wine-wparted-$ARCH}"
if [ "$ARCH" = x86 ]; then export WINEARCH=win32; fi
wineboot --init >/dev/null 2>&1 || true

BUILD="build/mingw-$ARCH"
cmake -S . -B "$BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
  -DCMAKE_BUILD_TYPE=Release \
  -DWPARTED_WARNINGS_AS_ERRORS=ON
cmake --build "$BUILD"
ctest --test-dir "$BUILD" --output-on-failure

# Prove the binaries only import Windows system DLLs (no MinGW runtime DLLs).
OBJDUMP="$( [ "$ARCH" = x64 ] && echo x86_64 || echo i686 )-w64-mingw32-objdump"
for exe in "$BUILD/apps/wparted-gui/wparted.exe" "$BUILD/apps/wparted-cli/wparted-cli.exe"; do
  imports="$($OBJDUMP -p "$exe" | awk '/DLL Name:/ {print tolower($3)}' | sort -u | tr '\n' ' ')"
  echo "$(basename "$exe") imports: $imports"
  for dll in $imports; do
    case "$dll" in
      kernel32.dll|user32.dll|gdi32.dll|advapi32.dll|shell32.dll|ole32.dll|comctl32.dll|comdlg32.dll|msvcrt.dll|uxtheme.dll|dwmapi.dll|shlwapi.dll|oleaut32.dll|oleacc.dll|api-ms-win-*) ;;
      *) echo "unexpected runtime dependency: $dll" >&2; exit 1 ;;
    esac
  done
done

# Record the toolchain for the SBOM.
TRIPLE="$( [ "$ARCH" = x64 ] && echo x86_64 || echo i686 )-w64-mingw32"
{
  echo "gcc=$($TRIPLE-g++-posix -dumpfullversion)"
  echo "mingw-w64=$(dpkg-query -W -f='${Version}' mingw-w64-common 2>/dev/null || echo unknown)"
} > "$BUILD/toolchain.txt"

if [ "$PACKAGE" -eq 1 ]; then
  # The static runtimes (mingw-w64 CRT, winpthreads, libgcc/libstdc++) are
  # shipped with their upstream copyright/licence texts.
  python3 tools/package.py --build-dir "$BUILD" --arch "$ARCH" --emulator wine \
    --license-file mingw-w64-runtime-and-winpthreads.txt=/usr/share/doc/mingw-w64-common/copyright \
    --license-file gcc-runtime-libraries.txt=/usr/share/doc/gcc-mingw-w64-base/copyright
fi
echo "OK: $BUILD"
