# SPDX-License-Identifier: GPL-2.0-or-later
configure_file(${PROJECT_SOURCE_DIR}/apps/wparted_version.h.in ${CMAKE_CURRENT_BINARY_DIR}/wparted_version.h @ONLY)
add_executable(wparted-cli src/main.cpp)
target_include_directories(wparted-cli PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(wparted-cli PRIVATE wparted_win_storage)
if(MINGW)
    target_link_options(wparted-cli PRIVATE -municode)  # wmain entry point
endif()
