// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#include "wparted/core/layout_writer.hpp"

#include <algorithm>
#include <cstring>
#include <random>

namespace wparted {

namespace {

constexpr std::uint32_t kGptEntries = 128;
constexpr std::uint32_t kGptEntrySize = 128;

void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void put64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

// MBR entry with "LBA only" CHS markers.
void mbrEntry(std::uint8_t* e, bool boot, std::uint8_t type, std::uint64_t start, std::uint64_t count) {
    std::memset(e, 0, 16);
    e[0] = boot ? 0x80 : 0x00;
    e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;
    e[4] = type;
    e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
    put32(e + 8, static_cast<std::uint32_t>(start));
    put32(e + 12, static_cast<std::uint32_t>(count));
}

std::vector<std::uint8_t> bootSector(const DiskLayout& l, const SerializeOptions& o) {
    std::vector<std::uint8_t> s(l.sectorSize, 0);
    if (o.originalSector0.size() >= 440) std::memcpy(s.data(), o.originalSector0.data(), 440);  // keep boot code
    put32(&s[440], l.mbrSignature);
    s[510] = 0x55;
    s[511] = 0xAA;
    return s;
}

bool inAnyPartition(const DiskLayout& l, std::uint64_t lba) {
    for (const auto& p : l.partitions)
        if (lba >= p.firstLba && lba <= p.lastLba()) return true;
    return false;
}

bool serializeGpt(const DiskLayout& l, const SerializeOptions& o, std::vector<SectorWrite>& out, std::string& err) {
    const std::uint32_t ss = l.sectorSize;
    const std::uint64_t total = l.totalSectors();
    const std::uint64_t arrayBytes = static_cast<std::uint64_t>(kGptEntries) * kGptEntrySize;
    const std::uint64_t arraySectors = (arrayBytes + ss - 1) / ss;
    if (l.diskGuid.isZero()) { err = "GPT disk GUID not assigned"; return false; }
    if (l.firstUsableLba < 2 + arraySectors || l.lastUsableLba > total - 2 - arraySectors) {
        err = "usable area overlaps the GPT structures";
        return false;
    }
    std::vector<std::uint8_t> array(static_cast<std::size_t>(arraySectors * ss), 0);
    for (const auto& p : l.partitions) {
        if (p.kind != PartitionKind::Primary || p.number < 1 || p.number > static_cast<int>(kGptEntries)) {
            err = "invalid GPT partition number " + std::to_string(p.number);
            return false;
        }
        if (p.uniqueGuid.isZero() || p.typeGuid.isZero()) { err = "partition GUIDs not assigned"; return false; }
        std::uint8_t* e = &array[static_cast<std::size_t>(p.number - 1) * kGptEntrySize];
        if (!std::all_of(e, e + kGptEntrySize, [](std::uint8_t b) { return b == 0; })) {
            err = "duplicate GPT partition number " + std::to_string(p.number);
            return false;
        }
        std::memcpy(e, p.typeGuid.bytes.data(), 16);
        std::memcpy(e + 16, p.uniqueGuid.bytes.data(), 16);
        put64(e + 32, p.firstLba);
        put64(e + 40, p.lastLba());
        put64(e + 48, p.gptAttributes);
        if (!utf8ToUtf16le(p.name, e + 56, 36)) { err = "partition name too long"; return false; }
    }
    const std::uint32_t arrayCrc = crc32(array.data(), static_cast<std::size_t>(arrayBytes));
    auto header = [&](std::uint64_t my, std::uint64_t alt, std::uint64_t entries) {
        std::vector<std::uint8_t> h(ss, 0);
        std::memcpy(h.data(), "EFI PART", 8);
        put32(&h[8], 0x00010000);
        put32(&h[12], 92);
        put64(&h[24], my);
        put64(&h[32], alt);
        put64(&h[40], l.firstUsableLba);
        put64(&h[48], l.lastUsableLba);
        std::memcpy(&h[56], l.diskGuid.bytes.data(), 16);
        put64(&h[72], entries);
        put32(&h[80], kGptEntries);
        put32(&h[84], kGptEntrySize);
        put32(&h[88], arrayCrc);
        put32(&h[16], crc32(h.data(), 92));
        return h;
    };
    std::vector<std::uint8_t> mbr = bootSector(l, o);
    for (int i = 0; i < 4; ++i) std::memset(&mbr[446 + 16 * i], 0, 16);
    mbrEntry(&mbr[446], false, 0xEE, 1, std::min<std::uint64_t>(total - 1, 0xFFFFFFFFull));
    out.push_back({0, mbr});
    out.push_back({1, header(1, total - 1, 2)});
    out.push_back({2, array});
    out.push_back({total - 1 - arraySectors, array});
    out.push_back({total - 1, header(total - 1, 1, total - 1 - arraySectors)});
    return true;
}

bool serializeMbr(const DiskLayout& l, const SerializeOptions& o, std::vector<SectorWrite>& out, std::string& err) {
    const std::uint32_t ss = l.sectorSize;
    const std::uint64_t total = l.totalSectors();
    std::vector<std::uint8_t> mbr = bootSector(l, o);
    const PartitionEntry* ext = nullptr;
    std::vector<const PartitionEntry*> logical;
    for (const auto& p : l.partitions) {
        if (p.firstLba > 0xFFFFFFFFull || p.sectorCount > 0xFFFFFFFFull) {
            err = "partition #" + std::to_string(p.number) + " exceeds the msdos 2 TiB limit";
            return false;
        }
        if (p.kind == PartitionKind::Logical) {
            logical.push_back(&p);
            continue;
        }
        if (p.number < 1 || p.number > 4) { err = "invalid primary partition number"; return false; }
        std::uint8_t* e = &mbr[446 + 16 * (p.number - 1)];
        if (e[4] != 0) { err = "duplicate primary partition number"; return false; }
        mbrEntry(e, p.mbrBootable, p.mbrType, p.firstLba, p.sectorCount);
        if (p.kind == PartitionKind::Extended) ext = &p;
    }
    out.push_back({0, mbr});
    if (!logical.empty()) {
        if (!ext) { err = "logical partitions without an extended partition"; return false; }
        std::sort(logical.begin(), logical.end(), [](auto a, auto b) { return a->firstLba < b->firstLba; });
        for (std::size_t i = 0; i < logical.size(); ++i) {
            const PartitionEntry& lp = *logical[i];
            const std::uint64_t ebr = i == 0 ? ext->firstLba : lp.firstLba - 1;
            if (lp.firstLba <= ebr || ebr < ext->firstLba) { err = "no room for an extended boot record"; return false; }
            std::vector<std::uint8_t> s(ss, 0);
            mbrEntry(&s[446], lp.mbrBootable, lp.mbrType, lp.firstLba - ebr, lp.sectorCount);
            if (i + 1 < logical.size()) {
                const PartitionEntry& nx = *logical[i + 1];
                const std::uint64_t nextEbr = nx.firstLba - 1;
                mbrEntry(&s[462], false, 0x05, nextEbr - ext->firstLba, nx.lastLba() - nextEbr + 1);
            }
            s[510] = 0x55;
            s[511] = 0xAA;
            out.push_back({ebr, s});
        }
    } else if (ext) {
        // An empty extended partition still needs a valid (empty) first EBR.
        std::vector<std::uint8_t> s(ss, 0);
        s[510] = 0x55;
        s[511] = 0xAA;
        out.push_back({ext->firstLba, s});
    }
    // Remove stale GPT headers so that no tool sees two partition tables.
    for (std::uint64_t lba : {std::uint64_t{1}, total - 1})
        if (!inAnyPartition(l, lba)) out.push_back({lba, std::vector<std::uint8_t>(ss, 0)});
    return true;
}

}  // namespace

void assignNewIdentifiers(DiskLayout& l) {
    if (l.type == TableType::Gpt) {
        if (l.diskGuid.isZero()) l.diskGuid = randomGuid();
        for (auto& p : l.partitions)
            if (p.uniqueGuid.isZero()) p.uniqueGuid = randomGuid();
    } else if (l.type == TableType::Mbr && l.mbrSignature == 0) {
        std::random_device rd;
        while (l.mbrSignature == 0) l.mbrSignature = static_cast<std::uint32_t>(rd());
    }
}

bool serializeLayout(const DiskLayout& l, const SerializeOptions& o, std::vector<SectorWrite>& out, std::string& err) {
    out.clear();
    if (l.sectorSize < 512 || l.totalSectors() < 64) { err = "device too small"; return false; }
    if (l.type == TableType::Gpt) return serializeGpt(l, o, out, err);
    if (l.type == TableType::Mbr) return serializeMbr(l, o, out, err);
    err = "only gpt and msdos tables can be written";
    return false;
}

}  // namespace wparted
