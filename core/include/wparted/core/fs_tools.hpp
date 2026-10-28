// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Native file-system operations for plan execution on disk image files.
// Every function works on a device that *is* the partition (RegionDevice),
// so it cannot touch bytes outside it. Implemented from the public on-disk
// format specifications (Microsoft FAT and exFAT specifications, the NTFS
// layout documented by the Linux-NTFS project, the ext4 disk layout in the
// Linux kernel documentation, the Linux swap header); no code is taken from
// GParted, dosfstools, ntfs-3g or e2fsprogs. Results are verified in CI with
// those tools (tools/ci/fs-tools-check.sh).
//
//   FAT16/FAT32   create, label, serial, check (read-only), grow, shrink
//   exFAT         label, serial
//   NTFS          label, serial, grow, shrink (if the space behind the new end is free)
//   ext2/3/4      label, UUID
//   Linux swap    label, UUID
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wparted/core/block_reader.hpp"
#include "wparted/core/filesystem.hpp"

namespace wparted {

class BlockDevice;  // executor.hpp

namespace fstools {

bool canCreate(FsType t) noexcept;
bool canLabel(FsType t) noexcept;
bool canSetUuid(FsType t) noexcept;
bool canCheck(FsType t) noexcept;
bool canResize(FsType t) noexcept;

// Empty if `label` is acceptable for `t`, otherwise the reason (length,
// characters). The same rule is used by validation and by the dialogs.
std::string labelProblem(FsType t, const std::string& label);

// Creates a new, empty file system filling the whole device. `hiddenSectors`
// is the partition start in sectors (FAT BPB field).
bool create(BlockDevice& part, FsType t, const std::string& label, std::uint64_t hiddenSectors, std::string& error);

bool setLabel(BlockDevice& part, FsType t, const std::string& label, std::string& error);

// Sets a new random UUID/serial; `uuid` receives its text form.
bool setNewUuid(BlockDevice& part, FsType t, std::string& uuid, std::string& error);

// Grows or shrinks the file system on `part` to `newBytes`. The device must
// cover both the old and the new size (shrink: the old partition; grow: the
// new partition).
bool resize(BlockDevice& part, FsType t, std::uint64_t newBytes, std::string& error);

// Empty if the file system on `part` can be shrunk to `newBytes`, otherwise
// why not (checked before a plan writes anything).
std::string shrinkProblem(BlockReader& part, FsType t, std::uint64_t newBytes);

struct CheckReport {
    bool ok = true;                     // no errors (warnings allowed)
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    std::uint64_t usedBytes = 0;
    std::uint64_t totalBytes = 0;
};

// Read-only consistency check; never repairs.
bool check(BlockReader& part, FsType t, CheckReport& report, std::string& error);

// Space in use, where it can be read cheaply (FAT); 0 = unknown.
std::uint64_t usedBytes(BlockReader& part, FsType t);

// CRC32C (Castagnoli), as used by ext4 metadata_csum; no final inversion.
std::uint32_t crc32c(std::uint32_t crc, const std::uint8_t* data, std::size_t len) noexcept;

}  // namespace fstools
}  // namespace wparted
