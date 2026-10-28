# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.20)

project(WParted
    VERSION 0.4.0
    DESCRIPTION "WParted - Windows Partition Editor"
    HOMEPAGE_URL "https://git.tracht-digital.de/jtracht/WParted"
    LANGUAGES CXX)

set(WPARTED_VERSION_SUFFIX "alpha" CACHE STRING "Pre-release suffix (alpha, beta.1, rc.1 or empty)")
if(WPARTED_VERSION_SUFFIX)
    set(WPARTED_VERSION_STRING "${PROJECT_VERSION}-${WPARTED_VERSION_SUFFIX}")
else()
    set(WPARTED_VERSION_STRING "${PROJECT_VERSION}")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(WPARTED_BUILD_TESTS "Build unit tests" ON)
option(WPARTED_BUILD_GUI "Build the Win32 GUI (Windows only)" ON)

if(MSVC)
    # Static CRT: no Visual C++ redistributable needed on the target machine.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /guard:cf /sdl)
    add_link_options(/guard:cf /DYNAMICBASE /NXCOMPAT)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        add_link_options(/SAFESEH)
    endif()
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

option(WPARTED_WARNINGS_AS_ERRORS "Treat compiler warnings as errors (CI)" OFF)
if(WPARTED_WARNINGS_AS_ERRORS)
    add_compile_options($<IF:$<CXX_COMPILER_ID:MSVC>,/WX,-Werror>)
endif()

if(MINGW)
    # Win32 idiom `STRUCT s{sizeof s};` zero-initialises the remaining members.
    add_compile_options(-Wno-missing-field-initializers)
    # Cross builds on the Linux CI runner. Fully static (libgcc, libstdc++,
    # winpthread) so the binaries only import Windows system DLLs, and the C
    # runtime is msvcrt.dll, which ships with every Windows version.
    add_link_options(-static -static-libgcc -static-libstdc++
                     -Wl,--dynamicbase -Wl,--nxcompat
                     $<$<CONFIG:Release>:-s>)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        add_link_options(-Wl,--high-entropy-va)
    endif()
endif()

if(WIN32)
    # Target Windows 7 SP1 as the minimum API level. Newer APIs must be
    # resolved at run time (GetProcAddress) and degrade gracefully.
    add_compile_definitions(_WIN32_WINNT=0x0601 WINVER=0x0601 NTDDI_VERSION=0x06010000
                            UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

add_subdirectory(core)

if(WIN32)
    add_subdirectory(platforms/windows/storage-api)
    add_subdirectory(apps/wparted-cli)
    add_subdirectory(apps/wparted-helper)
    if(WPARTED_BUILD_GUI)
        add_subdirectory(apps/wparted-gui)
    endif()
endif()

if(WPARTED_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
