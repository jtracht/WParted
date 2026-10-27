# SPDX-License-Identifier: GPL-2.0-or-later
enable_language(RC)
configure_file(${PROJECT_SOURCE_DIR}/apps/wparted_version.h.in ${CMAKE_CURRENT_BINARY_DIR}/wparted_version.h @ONLY)

set(WPARTED_ICON "${CMAKE_CURRENT_SOURCE_DIR}/res/wparted.ico")
set(WPARTED_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/res/wparted.manifest")
set(WPARTED_FILE_DESCRIPTION "WParted - Windows Partition Editor")
set(WPARTED_INTERNAL_NAME "wparted")
configure_file(res/wparted.rc.in ${CMAKE_CURRENT_BINARY_DIR}/wparted.rc @ONLY)

add_executable(wparted WIN32
    src/main.cpp
    src/main_window.cpp
    src/disk_map.cpp
    src/theme.cpp
    src/strings.cpp
    src/form_dialog.cpp
    src/planner.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/wparted.rc)
target_include_directories(wparted PRIVATE src ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(wparted PRIVATE wparted_win_storage comctl32 comdlg32 shell32 ole32)
if(MSVC)
    # The manifest is embedded through the .rc file.
    target_link_options(wparted PRIVATE /MANIFEST:NO)
elseif(MINGW)
    target_link_options(wparted PRIVATE -municode)  # wWinMain entry point
endif()
