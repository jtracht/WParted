// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// The gate in front of every write to a physical disk. Pure logic, so it is
// unit-tested without any disk. Virtual disks (attached VHD/VHDX files) are the
// test bed; real disks stay refused unless the build enables them, which needs
// a human security review first (docs/architecture/security-architecture.md).
#pragma once

#include <string>

namespace wparted {

struct DiskWriteFacts {
    bool elevated = false;
    bool virtualDisk = false;           // bus type "File-backed virtual"
    bool realDiskWritesEnabled = false; // build option WPARTED_ENABLE_REAL_DISK_WRITES
    bool holdsRunningSystem = false;    // Windows, page file or boot manager on this disk
    bool writeProtected = false;
    bool layoutReadable = false;
};

// Empty if the disk may be written, otherwise the reason.
std::string diskWriteProblem(const DiskWriteFacts& facts);

// Strict parser for "\\.\PhysicalDriveN"; nothing else is accepted.
bool parsePhysicalDrivePath(const std::string& path, unsigned& number);

// Mount points WParted accepts: a drive letter in the form "E:\".
bool isDriveLetterMountPoint(const std::string& mountPoint);
// Volume names WParted accepts: "\\?\Volume{GUID}\".
bool isVolumeGuidPath(const std::string& volume);

}  // namespace wparted
