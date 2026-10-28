# SPDX-License-Identifier: GPL-2.0-or-later
# Cross-compile Windows on ARM64 binaries on Linux with llvm-mingw (clang,
# UCRT). The results cannot be run in CI (no ARM64 Windows or emulator).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR ARM64)
set(WPARTED_MINGW_TRIPLE aarch64-w64-mingw32)

set(CMAKE_C_COMPILER ${WPARTED_MINGW_TRIPLE}-clang)
set(CMAKE_CXX_COMPILER ${WPARTED_MINGW_TRIPLE}-clang++)
set(CMAKE_RC_COMPILER ${WPARTED_MINGW_TRIPLE}-windres)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
