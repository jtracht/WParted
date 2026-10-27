// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wparted/core/partition_table.hpp"

namespace wparted::win {

// A Windows volume and the disk extent it lives on.
struct VolumeInfo {
    std::wstring guidPath;                 // \\?\Volume{...}\ form
    std::vector<std::wstring> mountPoints; // "C:\", "D:\mnt\x\", ...
    std::wstring label;
    std::wstring fileSystem;               // As reported by Windows ("NTFS", "FAT32", ...)
    bool hasUsage = false;
    std::uint64_t totalBytes = 0;
    std::uint64_t freeBytes = 0;
    std::uint32_t diskNumber = 0;
    std::uint64_t startingOffset = 0;
    std::uint64_t extentLength = 0;
    int extentCount = 0;                   // > 1 means spanned/striped (dynamic disk, Storage Spaces)
    bool containsWindows = false;          // The running Windows installation lives here
    bool isPageFileVolume = false;
};

enum class Tristate { Unknown, No, Yes };

struct PhysicalDisk {
    std::uint32_t number = 0;
    std::wstring path;                     // \\.\PhysicalDriveN
    std::string vendor, product, revision, serial;
    std::string busType;
    bool removableMedia = false;
    Tristate solidState = Tristate::Unknown;  // From the seek penalty property
    Tristate writeProtected = Tristate::Unknown;
    std::uint64_t sizeBytes = 0;
    std::uint32_t logicalSectorSize = 512;
    std::uint32_t physicalSectorSize = 0;  // 0 = unknown

    bool rawReadable = false;              // Opened with read access (needs elevation)
    std::string accessError;               // Why raw access failed, if it did
    bool layoutAvailable = false;
    DiskLayout layout;                     // Parsed by WParted from raw sectors
    bool osLayoutCompared = false;
    std::vector<VolumeInfo> volumes;

    std::string displayName() const;      // "/dev/sda"-like friendly name: "Disk 0"
    std::string modelName() const;         // "Vendor Product"
    bool containsWindows() const;
};

struct DiscoveryResult {
    std::vector<PhysicalDisk> disks;
    bool elevated = false;
    double durationMs = 0;
};

bool isProcessElevated();

// Enumerates physical disks and volumes. Strictly read-only: every handle is
// opened with read access at most, and no IOCTL that modifies state is used.
DiscoveryResult discoverDisks();

// Finds the Windows volume (if any) that corresponds to a partition.
const VolumeInfo* volumeForPartition(const PhysicalDisk& disk, const PartitionEntry& p);

std::string narrow(const std::wstring& w);
std::wstring widen(const std::string& s);

}  // namespace wparted::win
