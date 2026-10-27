// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Serialises a DiskLayout into the sectors of an MBR or GPT partition table.
// Output is a list of sector writes held in memory: this module performs no
// I/O. Tests apply the writes to image buffers and parse them back; a future
// privileged executor will apply them to a device after validation.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wparted/core/partition_table.hpp"

namespace wparted {

struct SectorWrite {
    std::uint64_t lba = 0;
    std::vector<std::uint8_t> data;  // a multiple of the logical sector size
};

struct SerializeOptions {
    // Existing sector 0, so that boot code (bytes 0-439) is preserved. Empty
    // for a blank device.
    std::vector<std::uint8_t> originalSector0;
};

// Gives new identifiers to everything the plan left unassigned: GPT disk
// GUID, partition GUIDs and the MBR disk signature.
void assignNewIdentifiers(DiskLayout& layout);

bool serializeLayout(const DiskLayout& layout, const SerializeOptions& options, std::vector<SectorWrite>& out,
                     std::string& error);

}  // namespace wparted
