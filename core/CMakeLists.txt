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
    src/log.cpp)
target_include_directories(wparted_core PUBLIC include)
