# SPDX-License-Identifier: GPL-2.0-or-later
# Platform-independent core: no Windows headers allowed here, so the logic
# can be unit-tested on any host (including Linux CI containers).
add_library(wparted_core STATIC
    src/types.cpp
    src/block_reader.cpp
    src/filesystem.cpp
    src/partition_table.cpp
    src/layout_view.cpp
    src/json.cpp
    src/log.cpp
    src/json_value.cpp
    src/operations.cpp
    src/validation.cpp
    src/layout_writer.cpp
    src/rescue.cpp
    src/operation_log.cpp
    src/executor.cpp
    src/fs_fat.cpp
    src/fs_tools.cpp
    src/fs_ntfs.cpp
    src/fs_ext.cpp)
target_include_directories(wparted_core PUBLIC include)
