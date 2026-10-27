// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#pragma once

#include <string>

#include "wparted/win/disk_discovery.hpp"

namespace wparted::win {

// Full machine-readable report of a discovery run (used by the CLI `list
// --json` command and by the GUI "Export report" action).
std::string discoveryToJson(const DiscoveryResult& r, const char* version);
std::string diskToJson(const PhysicalDisk& d);

const char* tristateName(Tristate t) noexcept;  // "yes" / "no" / "unknown"

// Maps the file system name reported by Windows to WParted's FsType.
FsType fsTypeFromWindowsName(const std::wstring& name);

// When the raw partition table cannot be read (no elevation), builds a
// display-only layout from the Windows volumes on that disk so the user still
// sees something accurate. The result is clearly marked via a diagnostic.
DiskLayout layoutFromVolumes(const PhysicalDisk& d);

}  // namespace wparted::win
