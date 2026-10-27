// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#pragma once

#include <cstdint>
#include <vector>

#include "wparted/core/partition_table.hpp"

namespace wparted {

// A contiguous region of the disk as displayed in the disk map and partition
// list: either a partition or an unallocated gap.
struct Segment {
    enum class Kind { Partition, Unallocated };
    Kind kind = Kind::Unallocated;
    int partitionIndex = -1;  // Index into DiskLayout::partitions, -1 for gaps
    std::uint64_t firstLba = 0;
    std::uint64_t sectorCount = 0;
    std::vector<Segment> children;  // Logical partitions and gaps inside an extended partition
};

// Builds the ordered top-level segments for a layout, inserting unallocated
// gaps of at least `minGapBytes` (GParted hides gaps smaller than 1 MiB).
std::vector<Segment> buildSegments(const DiskLayout& layout, std::uint64_t minGapBytes = 1024 * 1024);

}  // namespace wparted
