# SPDX-License-Identifier: GPL-2.0-or-later
configure_file(${PROJECT_SOURCE_DIR}/apps/wparted_version.h.in ${CMAKE_CURRENT_BINARY_DIR}/wparted_version.h @ONLY)
add_executable(wparted-helper src/main.cpp)
target_include_directories(wparted-helper PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(wparted-helper PRIVATE wparted_win_storage)
if(MINGW)
    target_link_options(wparted-helper PRIVATE -municode)  # wmain entry point
endif()
# Next to the CLI and the GUI: the helper only accepts a pipe server in its own directory.
set_target_properties(wparted-helper PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps/wparted-cli)
