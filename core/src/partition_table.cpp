// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Read-only MBR ("msdos") and GPT parser. Implemented from the UEFI
// specification (chapter 5, GUID Partition Table) and the documented PC MBR /
// EBR layout. It performs strict validation and reports every inconsistency
// as a Diagnostic instead of guessing or "repairing" anything.
#include "wparted/core/partition_table.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

namespace wparted {

const char* tableTypeName(TableType t) noexcept {
    switch (t) {
        case TableType::None: return "none";
        case TableType::Mbr: return "msdos";
        case TableType::Gpt: return "gpt";
        case TableType::Unknown: return "unknown";
    }
    return "unknown";
}

const char* partitionKindName(PartitionKind k) noexcept {
    switch (k) {
        case PartitionKind::Primary: return "primary";
        case PartitionKind::Extended: return "extended";
        case PartitionKind::Logical: return "logical";
        case PartitionKind::WholeDevice: return "whole-device";
    }
    return "primary";
}

bool DiskLayout::hasErrors() const noexcept {
    for (const auto& d : diagnostics)
        if (d.severity == Severity::Error) return true;
    return false;
}

namespace {

constexpr std::uint64_t kMaxGptArrayBytes = 4u * 1024 * 1024;
constexpr int kMaxLogicalPartitions = 256;

void diag(DiskLayout& l, Severity s, const char* code, std::string msg) {
    l.diagnostics.push_back({s, code, std::move(msg)});
}

bool isExtendedType(std::uint8_t t) { return t == 0x05 || t == 0x0F || t == 0x85; }

struct GptHeader {
    std::uint64_t myLba = 0, alternateLba = 0, firstUsable = 0, lastUsable = 0, entriesLba = 0;
    std::uint32_t numEntries = 0, entrySize = 0, entriesCrc = 0;
    Guid diskGuid;
    std::vector<std::uint8_t> entries;
};

// Validates the header at `lba` and its partition entry array. On failure,
// `why` names the first failed check.
bool readGptHeader(BlockReader& r, std::uint64_t lba, std::uint32_t sectorSize, std::uint64_t totalSectors,
                   GptHeader& h, std::string& why) {
    std::vector<std::uint8_t> sec(sectorSize);
    if (!r.read(lba * sectorSize, sec.data(), sec.size())) { why = "unreadable"; return false; }
    if (std::memcmp(sec.data(), "EFI PART", 8) != 0) { why = "missing signature"; return false; }
    std::uint32_t headerSize = le32(&sec[12]);
    if (headerSize < 92 || headerSize > sectorSize) { why = "invalid header size"; return false; }
    std::uint32_t storedCrc = le32(&sec[16]);
    std::memset(&sec[16], 0, 4);
    if (crc32(sec.data(), headerSize) != storedCrc) { why = "header CRC mismatch"; return false; }
    h.myLba = le64(&sec[24]);
    h.alternateLba = le64(&sec[32]);
    h.firstUsable = le64(&sec[40]);
    h.lastUsable = le64(&sec[48]);
    h.diskGuid = Guid::fromDisk(&sec[56]);
    h.entriesLba = le64(&sec[72]);
    h.numEntries = le32(&sec[80]);
    h.entrySize = le32(&sec[84]);
    h.entriesCrc = le32(&sec[88]);
    if (h.myLba != lba) { why = "header LBA does not match its location"; return false; }
    if (h.firstUsable > h.lastUsable || h.lastUsable >= totalSectors) { why = "usable range outside device"; return false; }
    if (h.entrySize < 128 || (h.entrySize % 8) != 0) { why = "invalid entry size"; return false; }
    std::uint64_t arrayBytes = static_cast<std::uint64_t>(h.numEntries) * h.entrySize;
    if (h.numEntries == 0 || arrayBytes > kMaxGptArrayBytes) { why = "implausible entry count"; return false; }
    std::uint64_t arraySectors = (arrayBytes + sectorSize - 1) / sectorSize;
    if (h.entriesLba >= totalSectors || arraySectors > totalSectors - h.entriesLba) { why = "entry array outside device"; return false; }
    h.entries.resize(static_cast<std::size_t>(arrayBytes));
    if (!r.read(h.entriesLba * sectorSize, h.entries.data(), h.entries.size())) { why = "entry array unreadable"; return false; }
    if (crc32(h.entries.data(), h.entries.size()) != h.entriesCrc) { why = "entry array CRC mismatch"; return false; }
    return true;
}

void parseGpt(BlockReader& r, DiskLayout& l) {
    const std::uint32_t ss = l.sectorSize;
    const std::uint64_t total = l.totalSectors();
    GptHeader primary, backup;
    std::string whyPrimary, whyBackup;
    l.gptPrimaryValid = readGptHeader(r, 1, ss, total, primary, whyPrimary);
    l.gptBackupValid = total > 2 && readGptHeader(r, total - 1, ss, total, backup, whyBackup);

    if (!l.gptPrimaryValid && !l.gptBackupValid) {
        l.type = TableType::Unknown;
        diag(l, Severity::Error, "gpt.no_valid_header",
             "Protective MBR found but neither the primary (" + whyPrimary + ") nor the backup (" + whyBackup +
                 ") GPT header is valid.");
        return;
    }
    if (!l.gptPrimaryValid)
        diag(l, Severity::Error, "gpt.primary_invalid",
             "Primary GPT header is invalid (" + whyPrimary + "); using the backup header. No repair was attempted.");
    if (!l.gptBackupValid)
        diag(l, Severity::Warning, "gpt.backup_invalid",
             "Backup GPT header at the end of the device is invalid (" + whyBackup +
                 "). This is common after a disk image was copied to a larger device.");

    const GptHeader& h = l.gptPrimaryValid ? primary : backup;
    if (l.gptPrimaryValid && l.gptBackupValid &&
        (primary.entries != backup.entries || primary.diskGuid != backup.diskGuid))
        diag(l, Severity::Error, "gpt.primary_backup_mismatch",
             "Primary and backup GPT disagree. Using the primary copy for display only.");
    if (l.gptPrimaryValid && primary.alternateLba != total - 1)
        diag(l, Severity::Warning, "gpt.backup_location",
             "Primary GPT header does not point to the last sector for its backup copy.");

    l.type = TableType::Gpt;
    l.diskGuid = h.diskGuid;
    l.firstUsableLba = h.firstUsable;
    l.lastUsableLba = h.lastUsable;

    for (std::uint32_t i = 0; i < h.numEntries; ++i) {
        const std::uint8_t* e = &h.entries[static_cast<std::size_t>(i) * h.entrySize];
        Guid type = Guid::fromDisk(e);
        if (type.isZero()) continue;
        PartitionEntry p;
        p.number = static_cast<int>(i) + 1;
        p.kind = PartitionKind::Primary;
        p.typeGuid = type;
        p.uniqueGuid = Guid::fromDisk(e + 16);
        std::uint64_t first = le64(e + 32), last = le64(e + 40);
        p.gptAttributes = le64(e + 48);
        p.name = utf16leToUtf8(e + 56, 36);
        if (last < first) {
            diag(l, Severity::Error, "gpt.entry_reversed",
                 "Partition " + std::to_string(p.number) + " ends before it starts; ignored.");
            continue;
        }
        p.firstLba = first;
        p.sectorCount = last - first + 1;
        if (first < h.firstUsable || last > h.lastUsable)
            diag(l, Severity::Error, "gpt.entry_outside_usable",
                 "Partition " + std::to_string(p.number) + " lies outside the usable area of the GPT.");
        l.partitions.push_back(std::move(p));
    }
}

void parseMbrEntry(const std::uint8_t* e, std::uint8_t& boot, std::uint8_t& type, std::uint32_t& start,
                   std::uint32_t& count) {
    boot = e[0];
    type = e[4];
    start = le32(e + 8);
    count = le32(e + 12);
}

// `ext` is taken by value: logical entries are appended to l.partitions,
// which may reallocate and would invalidate a reference into it.
void parseLogicals(BlockReader& r, DiskLayout& l, const PartitionEntry ext) {
    const std::uint32_t ss = l.sectorSize;
    std::vector<std::uint8_t> sec(ss);
    std::set<std::uint64_t> visited;
    std::uint64_t ebr = ext.firstLba;
    int number = 5;
    while (true) {
        if (!visited.insert(ebr).second) {
            diag(l, Severity::Error, "mbr.ebr_loop", "Extended boot record chain contains a loop; stopped.");
            break;
        }
        if (number - 5 >= kMaxLogicalPartitions) {
            diag(l, Severity::Error, "mbr.too_many_logical", "Too many logical partitions; stopped reading the chain.");
            break;
        }
        if (ebr < ext.firstLba || ebr > ext.lastLba() || !r.read(ebr * ss, sec.data(), ss)) {
            diag(l, Severity::Error, "mbr.ebr_unreadable", "Extended boot record outside the extended partition or unreadable.");
            break;
        }
        if (sec[510] != 0x55 || sec[511] != 0xAA) {
            diag(l, Severity::Error, "mbr.ebr_signature", "Extended boot record has no 0x55AA signature.");
            break;
        }
        std::uint8_t boot, type, nboot, ntype;
        std::uint32_t start, count, nstart, ncount;
        parseMbrEntry(&sec[446], boot, type, start, count);
        parseMbrEntry(&sec[462], nboot, ntype, nstart, ncount);
        if (type != 0 && count != 0) {
            PartitionEntry p;
            p.number = number++;
            p.kind = PartitionKind::Logical;
            p.mbrType = type;
            p.mbrBootable = boot == 0x80;
            p.firstLba = ebr + start;
            p.sectorCount = count;
            if (p.firstLba < ext.firstLba || p.lastLba() > ext.lastLba())
                diag(l, Severity::Error, "mbr.logical_outside_extended",
                     "Logical partition " + std::to_string(p.number) + " extends beyond its extended partition.");
            l.partitions.push_back(p);
        }
        if (ntype == 0 || ncount == 0) break;
        if (!isExtendedType(ntype))
            diag(l, Severity::Warning, "mbr.ebr_link_type", "Unexpected type in EBR link entry; following it anyway.");
        ebr = ext.firstLba + nstart;  // Link entries are relative to the extended partition start.
    }
}

void parseMbr(BlockReader& r, DiskLayout& l, const std::uint8_t* sec0) {
    l.type = TableType::Mbr;
    l.mbrSignature = le32(sec0 + 440);
    l.firstUsableLba = 1;
    l.lastUsableLba = l.totalSectors() ? l.totalSectors() - 1 : 0;
    int extendedCount = 0;
    for (int i = 0; i < 4; ++i) {
        std::uint8_t boot, type;
        std::uint32_t start, count;
        parseMbrEntry(sec0 + 446 + 16 * i, boot, type, start, count);
        if (type == 0 || count == 0) continue;
        PartitionEntry p;
        p.number = i + 1;
        p.mbrType = type;
        p.mbrBootable = boot == 0x80;
        p.firstLba = start;
        p.sectorCount = count;
        p.kind = isExtendedType(type) ? PartitionKind::Extended : PartitionKind::Primary;
        l.partitions.push_back(p);
        if (p.kind == PartitionKind::Extended) {
            if (++extendedCount > 1)
                diag(l, Severity::Error, "mbr.multiple_extended", "More than one extended partition; only the first is read.");
            else
                parseLogicals(r, l, l.partitions.back());
        }
    }
}

std::uint32_t be32At(const std::vector<std::uint8_t>& b, std::size_t off) {
    return off + 4 <= b.size() ? be32(&b[off]) : 0;
}

// Partition table formats that libparted supports besides msdos/gpt. They are
// recognised by their published magic numbers so that WParted never mistakes
// such a disk for an empty one.
std::string detectForeignTable(BlockReader& r, std::uint32_t ss) {
    std::vector<std::uint8_t> head(16 * 512, 0);
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(r.size(), head.size()));
    if (n < 1024 || !r.read(0, head.data(), n)) return {};
    head.resize(n);
    if (head[0] == 'E' && head[1] == 'R') {  // Apple driver descriptor map, then partition map entries
        const std::uint32_t bs = static_cast<std::uint32_t>((head[2] << 8) | head[3]);
        if ((bs == 512 || bs == ss) && bs + 2 <= head.size() && head[bs] == 'P' && head[bs + 1] == 'M') return "mac";
    }
    if (be32At(head, 0) == 0x0BE5A941u) return "dvh";
    if (be32At(head, 0) == 0xC9C2D4C1u) return "aix";
    if (head.size() >= 510 && ((head[508] << 8) | head[509]) == 0xDABE) return "sun";
    for (std::size_t off : {std::size_t{512}, std::size_t{512 + 64}})
        if (off + 4 <= head.size() && le32(&head[off]) == 0x82564557u) return "bsd";
    for (std::size_t blk = 0; blk + 4 <= head.size(); blk += 512)
        if (std::memcmp(&head[blk], "RDSK", 4) == 0) return "amiga";
    return {};
}

bool validMbrEntries(const std::uint8_t* sec0, bool& anyUsed) {
    anyUsed = false;
    for (int i = 0; i < 4; ++i) {
        const std::uint8_t* e = sec0 + 446 + 16 * i;
        if (e[0] != 0x00 && e[0] != 0x80) return false;
        if (e[4] != 0 && le32(e + 12) != 0) {
            if (le32(e + 8) == 0) return false;  // A partition cannot start on the MBR itself.
            anyUsed = true;
        }
    }
    return true;
}

void checkBoundsAndOverlaps(DiskLayout& l) {
    const std::uint64_t total = l.totalSectors();
    for (const auto& p : l.partitions) {
        if (p.firstLba >= total || p.sectorCount > total - p.firstLba)
            diag(l, Severity::Error, "layout.beyond_device",
                 "Partition " + std::to_string(p.number) + " extends beyond the end of the device.");
    }
    // Overlaps among primaries/extended and among logicals (logicals are
    // expected to lie inside their extended partition).
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<const PartitionEntry*> v;
        for (const auto& p : l.partitions)
            if ((p.kind == PartitionKind::Logical) == (pass == 1)) v.push_back(&p);
        std::sort(v.begin(), v.end(), [](auto a, auto b) { return a->firstLba < b->firstLba; });
        for (std::size_t i = 1; i < v.size(); ++i)
            if (v[i]->firstLba <= v[i - 1]->lastLba())
                diag(l, Severity::Error, "layout.overlap",
                     "Partitions " + std::to_string(v[i - 1]->number) + " and " + std::to_string(v[i]->number) +
                         " overlap.");
    }
}

}  // namespace

DiskLayout parseDiskLayout(BlockReader& r, bool detectContents) {
    DiskLayout l;
    l.sectorSize = r.sectorSize();
    l.diskBytes = r.size();
    if (l.sectorSize < 512 || (l.sectorSize & (l.sectorSize - 1)) || l.diskBytes < l.sectorSize * 2ull) {
        diag(l, Severity::Error, "device.too_small", "Device is too small or reports an invalid sector size.");
        l.type = TableType::Unknown;
        return l;
    }
    std::vector<std::uint8_t> sec0(l.sectorSize);
    if (!r.read(0, sec0.data(), sec0.size())) {
        diag(l, Severity::Error, "device.read_failed", "The first sector could not be read.");
        l.type = TableType::Unknown;
        return l;
    }

    const bool bootSig = sec0[510] == 0x55 && sec0[511] == 0xAA;
    bool anyUsed = false;
    const bool entriesValid = bootSig && validMbrEntries(sec0.data(), anyUsed);
    bool protective = false;
    if (entriesValid)
        for (int i = 0; i < 4; ++i)
            if (sec0[446 + 16 * i + 4] == 0xEE) protective = true;

    if (protective) {
        parseGpt(r, l);
        if (l.type != TableType::Gpt) {
            // Wrong logical sector size is a classic cause (USB bridges that
            // translate 512 <-> 4096). Report it instead of reinterpreting.
            for (std::uint32_t alt : {512u, 4096u}) {
                std::uint8_t sig[8];
                if (alt != l.sectorSize && r.read(alt, sig, 8) && std::memcmp(sig, "EFI PART", 8) == 0)
                    diag(l, Severity::Error, "gpt.sector_size_mismatch",
                         "A GPT header exists for a " + std::to_string(alt) + "-byte sector size, but the device reports " +
                             std::to_string(l.sectorSize) + " bytes. The enclosure may translate sector sizes.");
            }
        }
    } else if (entriesValid && anyUsed) {
        parseMbr(r, l, sec0.data());
    } else {
        FsDetection fs = detectFilesystem(r);
        if (fs.type != FsType::Unknown) {
            l.type = TableType::None;
            PartitionEntry p;
            p.number = 0;
            p.kind = PartitionKind::WholeDevice;
            p.firstLba = 0;
            p.sectorCount = l.totalSectors();
            p.fs = fs;
            l.partitions.push_back(p);
            diag(l, Severity::Info, "layout.whole_device_fs",
                 std::string("No partition table; the whole device contains a ") + fsTypeName(fs.type) + " file system.");
            return l;
        }
        const std::string foreign = detectForeignTable(r, l.sectorSize);
        if (!foreign.empty()) {
            l.type = TableType::Unknown;
            l.foreignTable = foreign;
            diag(l, Severity::Info, "table.foreign",
                 "The device uses a \"" + foreign + "\" partition table. WParted can detect but not display or change it.");
            return l;
        }
        if (bootSig && entriesValid) {
            l.type = TableType::Mbr;  // Valid but empty MBR
            l.mbrSignature = le32(&sec0[440]);
            l.firstUsableLba = 1;
            l.lastUsableLba = l.totalSectors() - 1;
        } else if (bootSig) {
            l.type = TableType::Unknown;
            diag(l, Severity::Warning, "mbr.invalid_entries",
                 "Sector 0 has a boot signature but its partition entries are not valid.");
        } else {
            l.type = TableType::None;
            l.firstUsableLba = 0;
            l.lastUsableLba = l.totalSectors() - 1;
        }
        return l;
    }

    checkBoundsAndOverlaps(l);

    if (detectContents) {
        for (auto& p : l.partitions) {
            if (p.kind == PartitionKind::Extended) continue;
            std::uint64_t off = p.firstLba * l.sectorSize;
            if (off >= l.diskBytes) continue;
            std::uint64_t len = std::min<std::uint64_t>(p.sectorCount * l.sectorSize, l.diskBytes - off);
            SubRangeReader sub(r, off, len);
            p.fs = detectFilesystem(sub);
        }
    }
    return l;
}

// ---------------------------------------------------------------------------
// Partition type names and flags

namespace {

struct GptTypeName {
    const char* guid;
    const char* name;
    const char* flag;  // GParted/libparted flag implied by the type, or nullptr
    bool critical;
};

const GptTypeName kGptTypes[] = {
    {"C12A7328-F81F-11D2-BA4B-00A0C93EC93B", "EFI System", "esp", true},
    {"E3C9E316-0B5C-4DB8-817D-F92DF00215AE", "Microsoft reserved", "msftres", true},
    {"EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", "Microsoft basic data", "msftdata", false},
    {"DE94BBA4-06D1-4D40-A16A-BFD50179D6AC", "Windows recovery environment", "diag", true},
    {"5808C8AA-7E8F-42E0-85D2-E1E90434CFB3", "Windows LDM metadata (dynamic disk)", nullptr, true},
    {"AF9B60A0-1431-4F62-BC68-3311714A69AD", "Windows LDM data (dynamic disk)", nullptr, true},
    {"E75CAF8F-F680-4CEE-AFA3-B001E56EFC2D", "Windows Storage Spaces", nullptr, true},
    {"0FC63DAF-8483-4772-8E79-3D69D8477DE4", "Linux filesystem", nullptr, false},
    {"4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709", "Linux root (x86-64)", nullptr, false},
    {"44479540-F297-41B2-9AF7-D131D5F0458A", "Linux root (x86)", nullptr, false},
    {"B921B045-1DF0-41C3-AF44-4C6F280D3FAE", "Linux root (ARM64)", nullptr, false},
    {"933AC7E1-2EB4-4F13-B844-0E14E2AEF915", "Linux home", "linux-home", false},
    {"BC13C2FF-59E6-4262-A352-B275FD6F7172", "Linux extended boot", "bls_boot", false},
    {"0657FD6D-A4AB-43C4-84E5-0933C84B4F4F", "Linux swap", "swap", false},
    {"E6D6D379-F507-44C2-A23C-238F2A3DF928", "Linux LVM", "lvm", false},
    {"A19D880F-05FC-4D3B-A006-743F0F84911E", "Linux RAID", "raid", false},
    {"CA7D7CCB-63ED-4C53-861C-1742536059CC", "Linux LUKS", nullptr, false},
    {"21686148-6449-6E6F-744E-656564454649", "BIOS boot", "bios_grub", true},
    {"48465300-0000-11AA-AA11-00306543ECAC", "Apple HFS/HFS+", nullptr, false},
    {"7C3457EF-0000-11AA-AA11-00306543ECAC", "Apple APFS", nullptr, false},
    {"426F6F74-0000-11AA-AA11-00306543ECAC", "Apple boot", nullptr, true},
    {"6A898CC3-1DD2-11B2-99A6-080020736631", "ZFS / Solaris /usr", nullptr, false},
    {"516E7CB6-6ECF-11D6-8FF8-00022D09712B", "FreeBSD UFS", nullptr, false},
    {"83BD6B9D-7F41-11DC-BE0B-001560B84F0F", "FreeBSD boot", nullptr, true},
    {"FE3A2A5D-4F32-41A7-B725-ACCC3285A309", "ChromeOS kernel", "chromeos_kernel", false},
    {"9E1A2D38-C612-4316-AA26-8B49521E5A8B", "PReP boot", "prep", true},
};

const GptTypeName* findGptType(const Guid& g) {
    for (const auto& t : kGptTypes) {
        Guid tg;
        if (Guid::parse(t.guid, tg) && tg == g) return &t;
    }
    return nullptr;
}

const char* mbrTypeName(std::uint8_t t) {
    switch (t) {
        case 0x01: return "FAT12";
        case 0x04: return "FAT16 <32M";
        case 0x05: return "Extended";
        case 0x06: return "FAT16";
        case 0x07: return "NTFS/exFAT/HPFS";
        case 0x0B: return "W95 FAT32";
        case 0x0C: return "W95 FAT32 (LBA)";
        case 0x0E: return "W95 FAT16 (LBA)";
        case 0x0F: return "W95 Extended (LBA)";
        case 0x11: return "Hidden FAT12";
        case 0x14: return "Hidden FAT16 <32M";
        case 0x16: return "Hidden FAT16";
        case 0x17: return "Hidden NTFS/HPFS";
        case 0x1B: return "Hidden W95 FAT32";
        case 0x1C: return "Hidden W95 FAT32 (LBA)";
        case 0x1E: return "Hidden W95 FAT16 (LBA)";
        case 0x27: return "Windows recovery (hidden NTFS)";
        case 0x42: return "Windows dynamic disk (LDM)";
        case 0x82: return "Linux swap / Solaris";
        case 0x83: return "Linux";
        case 0x85: return "Linux extended";
        case 0x8E: return "Linux LVM";
        case 0xA5: return "FreeBSD";
        case 0xA6: return "OpenBSD";
        case 0xA9: return "NetBSD";
        case 0xAF: return "HFS/HFS+";
        case 0xEE: return "GPT protective";
        case 0xEF: return "EFI System";
        case 0xFD: return "Linux RAID autodetect";
        default: return nullptr;
    }
}

}  // namespace

std::string partitionTypeDescription(const DiskLayout& layout, const PartitionEntry& p) {
    if (p.kind == PartitionKind::WholeDevice) return "Whole device";
    if (layout.type == TableType::Gpt) {
        if (const auto* t = findGptType(p.typeGuid)) return t->name;
        return p.typeGuid.toString();
    }
    if (layout.type != TableType::Mbr) return "(not available)";
    char buf[48];
    const char* n = mbrTypeName(p.mbrType);
    std::snprintf(buf, sizeof buf, "0x%02X %s", p.mbrType, n ? n : "(unknown)");
    return buf;
}

std::vector<std::string> partitionFlags(const DiskLayout& layout, const PartitionEntry& p) {
    std::vector<std::string> f;
    if (layout.type == TableType::Gpt) {
        if (const auto* t = findGptType(p.typeGuid)) {
            if (t->flag) {
                if (std::strcmp(t->flag, "esp") == 0) f.emplace_back("boot");
                f.emplace_back(t->flag);
            }
        }
        if (p.gptAttributes & (1ull << 0)) f.emplace_back("required");
        if (p.gptAttributes & (1ull << 2)) f.emplace_back("legacy_boot");
        // Bits 60-63 are defined for Microsoft basic data partitions.
        if (p.gptAttributes & (1ull << 60)) f.emplace_back("read-only");
        if (p.gptAttributes & (1ull << 62)) f.emplace_back("hidden");
        if (p.gptAttributes & (1ull << 63)) f.emplace_back("no_automount");
    } else if (layout.type == TableType::Mbr) {
        if (p.mbrBootable) f.emplace_back("boot");
        switch (p.mbrType) {
            case 0x0C: case 0x0E: case 0x0F: case 0x1C: case 0x1E: f.emplace_back("lba"); break;
            default: break;
        }
        switch (p.mbrType) {
            case 0x11: case 0x14: case 0x16: case 0x17: case 0x1B: case 0x1C: case 0x1E: f.emplace_back("hidden"); break;
            case 0x27: f.emplace_back("diag"); break;
            case 0x82: f.emplace_back("swap"); break;
            case 0x8E: f.emplace_back("lvm"); break;
            case 0xFD: f.emplace_back("raid"); break;
            case 0xEF: f.emplace_back("esp"); break;
            default: break;
        }
    }
    return f;
}

bool isSystemCriticalType(const DiskLayout& layout, const PartitionEntry& p) {
    if (layout.type == TableType::Gpt) {
        if (const auto* t = findGptType(p.typeGuid)) return t->critical;
        return false;
    }
    return p.mbrType == 0x27 || p.mbrType == 0x42 || p.mbrType == 0xEF || p.mbrType == 0xEE;
}

}  // namespace wparted
