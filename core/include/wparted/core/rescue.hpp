// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Data rescue (GParted "Attempt Data Rescue"): scans unallocated space for
// file system signatures of lost partitions. Read-only; the result is a list
// of candidates that the user can inspect or turn into a normal plan.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

#include "wparted/core/operations.hpp"
#include "wparted/core/partition_table.hpp"

namespace wparted {

struct RescueCandidate {
    std::uint64_t firstLba = 0;
    std::uint64_t sectorCount = 0;  // from the file system header, clipped to the gap; 0 = unknown
    FsDetection fs;
};

struct RescueOptions {
    std::uint64_t stepBytes = 1024 * 1024;  // probe every MiB boundary ...
    bool cylinderPositions = true;          // ... and the legacy 63-sector / cylinder starts
    std::size_t maxCandidates = 64;
    const std::atomic<bool>* cancel = nullptr;
    std::function<void(std::uint64_t done, std::uint64_t total)> progress;
};

std::vector<RescueCandidate> rescueScan(BlockReader& reader, const DiskLayout& layout, const RescueOptions& options = {});

// File system size in bytes read from its own header (0 if unknown).
std::uint64_t filesystemSizeBytes(BlockReader& fsStart, FsType type);

// Adds one "recreate lost partition" operation (CreatePartition with
// keepContents) per candidate whose size is known, in disk order, skipping
// candidates that overlap an earlier one. Returns the number added; `error`
// names the first candidate that could not be planned.
std::size_t planRecreate(OperationQueue& queue, const std::vector<RescueCandidate>& found, std::string& error);

}  // namespace wparted
