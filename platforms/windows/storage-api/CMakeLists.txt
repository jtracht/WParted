# SPDX-License-Identifier: GPL-2.0-or-later
add_library(wparted_win_storage STATIC src/disk_discovery.cpp src/report.cpp src/system_volumes.cpp src/pipe.cpp
                                       src/helper_client.cpp src/disk_writer.cpp)
target_include_directories(wparted_win_storage PUBLIC include)
target_link_libraries(wparted_win_storage PUBLIC wparted_core ole32 oleaut32 advapi32 shell32 uuid)
# Real (non-virtual) disks stay refused until a human security review is done.
option(WPARTED_ENABLE_REAL_DISK_WRITES "Allow writing to real disks, not only attached virtual disks" OFF)
if(WPARTED_ENABLE_REAL_DISK_WRITES)
    target_compile_definitions(wparted_win_storage PRIVATE WPARTED_ENABLE_REAL_DISK_WRITES)
endif()
# The helper client announces the WParted version in its hello message.
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/wparted_storage_version.h
     "#pragma once\n#define WPARTED_STORAGE_VERSION \"${WPARTED_VERSION_STRING}\"\n")
target_include_directories(wparted_win_storage PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
