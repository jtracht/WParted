// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#pragma once

#include <vector>

#include "wparted/win/disk_discovery.hpp"

namespace wparted::win {

// Sets isSystemPartition, isPageFileVolume and bitLocker (read-only queries).
void annotateSystemVolumes(std::vector<VolumeInfo>& volumes);

}  // namespace wparted::win
