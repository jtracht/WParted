// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wparted/core/block_reader.hpp"

namespace wparted {

// File systems and content types WParted can recognise. The list mirrors the
// types GParted knows about (see docs/research/gparted-feature-inventory.md)
// plus Windows-specific ones.
enum class FsType {
    Unknown,
    Ntfs,
    Fat12,
    Fat16,
    Fat32,
    ExFat,
    ReFs,
    BitLocker,
    Ext2,
    Ext3,
    Ext4,
    Btrfs,
    Xfs,
    F2fs,
    Jfs,
    ReiserFs,
    Reiser4,
    Nilfs2,
    Udf,
    Ufs,
    Hfs,
    HfsPlus,
    Apfs,
    LinuxSwap,
    LinuxSuspend,
    Luks,
    Lvm2Pv,
    LinuxRaid,
    Zfs,
    Bcachefs,
    Minix,
};

const char* fsTypeName(FsType t) noexcept;          // "ntfs", "fat32", "linux-swap", ...
std::vector<FsType> allFsTypes();

struct FsDetection {
    FsType type = FsType::Unknown;
    std::string label;  // Empty if none or not cheaply readable.
    std::string uuid;   // Empty if none or not cheaply readable.
};

// Looks for a known on-disk signature at the start of `reader`. Only reads
// (at most a few hundred KiB from the beginning); never writes.
FsDetection detectFilesystem(BlockReader& reader);

// Capability status vocabulary used across the product (README, website, UI).
enum class Support {
    Supported,
    Limited,          // Supported with limitations
    OfflineOnly,      // Requires offline/recovery mode
    Experimental,
    ReadOnly,
    Unsupported,
    Planned,
};

const char* supportName(Support s) noexcept;

struct FsCapabilities {
    FsType type;
    Support detect;
    Support readUsage;
    Support create;
    Support grow;
    Support shrink;
    Support move;
    Support copy;
    Support check;
    Support label;
    Support uuid;
};

// The capabilities that the *current build* actually provides. This is the
// single source of truth for enabling UI actions and for documentation.
const FsCapabilities& capabilitiesFor(FsType t);

}  // namespace wparted
