# SPDX-License-Identifier: GPL-2.0-or-later
add_library(wparted_win_storage STATIC src/disk_discovery.cpp src/report.cpp src/system_volumes.cpp src/pipe.cpp
                                       src/helper_client.cpp)
target_include_directories(wparted_win_storage PUBLIC include)
target_link_libraries(wparted_win_storage PUBLIC wparted_core ole32 oleaut32)
# The helper client announces the WParted version in its hello message.
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/wparted_storage_version.h
     "#pragma once\n#define WPARTED_STORAGE_VERSION \"${WPARTED_VERSION_STRING}\"\n")
target_include_directories(wparted_win_storage PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
