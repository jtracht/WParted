// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "wparted/core/block_reader.hpp"
#include "wparted/core/filesystem.hpp"
#include "wparted/core/types.hpp"

namespace wparted {

enum class TableType {
    None,      // No partition table; possibly a whole-device file system.
    Mbr,       // "msdos" in GParted terms
    Gpt,
    Unknown,   // Something is there but could not be interpreted safely.
};

const char* tableTypeName(TableType t) noexcept;  // "none", "msdos", "gpt", "unknown"

enum class PartitionKind { Primary, Extended, Logical, WholeDevice };

const char* partitionKindName(PartitionKind k) noexcept;

enum class Severity { Info, Warning, Error };

struct Diagnostic {
    Diagnostic() = default;
    Diagnostic(Severity s, std::string c, std::string m, int op = 0, std::vector<std::string> a = {})
        : severity(s), code(std::move(c)), message(std::move(m)), operation(op), args(std::move(a)) {}

    Severity severity = Severity::Info;
    std::string code;     // Stable identifier, e.g. "gpt.primary_header_crc".
    std::string message;  // Human-readable English text.
    // Structured form for translations (validation diagnostics): 1-based
    // operation number (0 = whole plan/layout) and the values the message
    // was built from, in the order documented per code in validation.cpp.
    int operation = 0;
    std::vector<std::string> args;
};

struct PartitionEntry {
    int number = 0;                 // 1-based as shown to users (GPT slot+1 / MBR numbering)
    PartitionKind kind = PartitionKind::Primary;
    std::uint64_t firstLba = 0;
    std::uint64_t sectorCount = 0;  // Length in logical sectors

    // MBR specific
    std::uint8_t mbrType = 0;
    bool mbrBootable = false;

    // GPT specific
    Guid typeGuid;
    Guid uniqueGuid;
    std::uint64_t gptAttributes = 0;
    std::string name;               // GPT partition name (UTF-8)

    // Content detected inside the partition
    FsDetection fs;

    // Stable identity while planning operations (assigned by OperationQueue;
    // 0 = not part of a plan). Not stored on disk.
    std::uint32_t planId = 0;

    std::uint64_t lastLba() const noexcept { return sectorCount ? firstLba + sectorCount - 1 : firstLba; }
};

struct DiskLayout {
    TableType type = TableType::None;
    std::uint32_t sectorSize = 512;
    std::uint64_t diskBytes = 0;

    // MBR
    std::uint32_t mbrSignature = 0;

    // GPT
    Guid diskGuid;
    std::uint64_t firstUsableLba = 0;
    std::uint64_t lastUsableLba = 0;
    bool gptPrimaryValid = false;
    bool gptBackupValid = false;

    // Name of a recognised but unsupported table format ("mac", "bsd", "sun",
    // "dvh", "amiga", "aix"); empty otherwise. Only set with TableType::Unknown.
    std::string foreignTable;

    std::vector<PartitionEntry> partitions;   // Primary/extended/logical in on-disk order
    std::vector<Diagnostic> diagnostics;

    std::uint64_t totalSectors() const noexcept { return sectorSize ? diskBytes / sectorSize : 0; }
    bool hasErrors() const noexcept;
};

// Parses the partition table found on `reader`. Pure read-only analysis.
// If `detectContents` is set, each partition is probed for a file system.
DiskLayout parseDiskLayout(BlockReader& reader, bool detectContents = true);

// Human-readable partition type description ("EFI System", "Microsoft basic data", ...).
std::string partitionTypeDescription(const DiskLayout& layout, const PartitionEntry& p);

// Flags in GParted vocabulary ("boot", "esp", "msftres", "hidden", "lba", ...).
std::vector<std::string> partitionFlags(const DiskLayout& layout, const PartitionEntry& p);

// True for partitions that must never be modified by the desktop application
// without an offline strategy (ESP, MSR, recovery, dynamic disk, Storage Spaces).
bool isSystemCriticalType(const DiskLayout& layout, const PartitionEntry& p);

}  // namespace wparted
