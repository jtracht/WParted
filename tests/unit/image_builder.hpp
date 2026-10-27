// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Builds synthetic disk images in memory for parser tests. Nothing here
// touches a real device.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "wparted/core/types.hpp"

namespace wptest {

class Image {
public:
    Image(std::uint64_t bytes, std::uint32_t sectorSize) : data(static_cast<std::size_t>(bytes), 0), ss(sectorSize) {}

    std::vector<std::uint8_t> data;
    std::uint32_t ss;

    std::uint64_t sectors() const { return data.size() / ss; }
    std::uint8_t* at(std::uint64_t off) { return data.data() + static_cast<std::size_t>(off); }
    std::uint8_t& b(std::uint64_t off) { return data[static_cast<std::size_t>(off)]; }

    void put16(std::uint64_t off, std::uint16_t v) { b(off) = v & 0xFF; b(off + 1) = v >> 8; }
    void put32(std::uint64_t off, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b(off + i) = static_cast<std::uint8_t>(v >> (8 * i));
    }
    void put64(std::uint64_t off, std::uint64_t v) {
        for (int i = 0; i < 8; ++i) b(off + i) = static_cast<std::uint8_t>(v >> (8 * i));
    }
    void putStr(std::uint64_t off, const char* s, std::size_t n) { std::memcpy(at(off), s, n); }

    // MBR entry `slot` (0..3) in the sector at `sectorLba`.
    void mbrEntry(std::uint64_t sectorLba, int slot, std::uint8_t type, std::uint32_t start, std::uint32_t count,
                  bool boot = false) {
        std::uint64_t e = sectorLba * ss + 446 + 16 * slot;
        b(e) = boot ? 0x80 : 0x00;
        b(e + 4) = type;
        put32(e + 8, start);
        put32(e + 12, count);
        b(sectorLba * ss + 510) = 0x55;
        b(sectorLba * ss + 511) = 0xAA;
    }

    struct GptPart {
        const char* typeGuid;
        std::uint64_t first, last;
        const char* name;
        std::uint64_t attrs;
    };

    // Writes protective MBR, primary + backup GPT with valid CRCs.
    void gpt(const std::vector<GptPart>& parts, std::uint32_t numEntries = 128) {
        const std::uint64_t total = sectors();
        mbrEntry(0, 0, 0xEE, 1, static_cast<std::uint32_t>(std::min<std::uint64_t>(total - 1, 0xFFFFFFFFu)));
        const std::uint32_t entrySize = 128;
        const std::uint64_t arraySectors = (numEntries * entrySize + ss - 1) / ss;
        std::vector<std::uint8_t> arr(static_cast<std::size_t>(numEntries) * entrySize, 0);
        for (std::size_t i = 0; i < parts.size(); ++i) {
            std::uint8_t* e = arr.data() + i * entrySize;
            wparted::Guid t, u;
            wparted::Guid::parse(parts[i].typeGuid, t);
            wparted::Guid::parse("11111111-2222-3333-4444-5555555555" + hex2(static_cast<int>(i)), u);
            std::memcpy(e, t.bytes.data(), 16);
            std::memcpy(e + 16, u.bytes.data(), 16);
            for (int k = 0; k < 8; ++k) {
                e[32 + k] = static_cast<std::uint8_t>(parts[i].first >> (8 * k));
                e[40 + k] = static_cast<std::uint8_t>(parts[i].last >> (8 * k));
                e[48 + k] = static_cast<std::uint8_t>(parts[i].attrs >> (8 * k));
            }
            for (std::size_t c = 0; parts[i].name[c] && c < 36; ++c) e[56 + 2 * c] = static_cast<std::uint8_t>(parts[i].name[c]);
        }
        const std::uint32_t arrayCrc = wparted::crc32(arr.data(), arr.size());
        const std::uint64_t firstUsable = 2 + arraySectors, lastUsable = total - 2 - arraySectors;
        auto header = [&](std::uint64_t my, std::uint64_t alt, std::uint64_t entriesLba) {
            std::uint64_t o = my * ss;
            std::memset(at(o), 0, ss);
            putStr(o, "EFI PART", 8);
            put32(o + 8, 0x00010000);
            put32(o + 12, 92);
            put64(o + 24, my);
            put64(o + 32, alt);
            put64(o + 40, firstUsable);
            put64(o + 48, lastUsable);
            wparted::Guid dg;
            wparted::Guid::parse("DEADBEEF-0000-4000-8000-0123456789AB", dg);
            std::memcpy(at(o + 56), dg.bytes.data(), 16);
            put64(o + 72, entriesLba);
            put32(o + 80, numEntries);
            put32(o + 84, entrySize);
            put32(o + 88, arrayCrc);
            put32(o + 16, wparted::crc32(at(o), 92));
            std::memcpy(at(entriesLba * ss), arr.data(), arr.size());
        };
        header(1, total - 1, 2);
        header(total - 1, 1, total - 1 - arraySectors);
    }

    static std::string hex2(int v) {
        const char* d = "0123456789ABCDEF";
        return std::string{d[(v >> 4) & 15], d[v & 15]};
    }
};

// Signature writers, placed at byte offset `base`.
inline void writeNtfs(Image& im, std::uint64_t base) {
    im.b(base) = 0xEB; im.b(base + 1) = 0x52; im.b(base + 2) = 0x90;
    im.putStr(base + 3, "NTFS    ", 8);
    im.put64(base + 0x48, 0x1122334455667788ull);
    im.b(base + 510) = 0x55; im.b(base + 511) = 0xAA;
}

inline void writeFat32(Image& im, std::uint64_t base, const char* label) {
    im.b(base) = 0xEB; im.b(base + 1) = 0x58; im.b(base + 2) = 0x90;
    im.putStr(base + 3, "MSDOS5.0", 8);
    im.put16(base + 11, 512);
    im.b(base + 13) = 8;
    im.put16(base + 14, 32);
    im.b(base + 16) = 2;
    im.put16(base + 17, 0);
    im.put16(base + 19, 0);
    im.put16(base + 22, 0);
    im.put32(base + 32, 1048576);  // 512 MiB in 512-byte sectors
    im.put32(base + 36, 1024);
    im.b(base + 66) = 0x29;
    im.put32(base + 67, 0xABCD1234);
    im.putStr(base + 71, label, 11);
    im.putStr(base + 82, "FAT32   ", 8);
    im.b(base + 510) = 0x55; im.b(base + 511) = 0xAA;
}

inline void writeExt4(Image& im, std::uint64_t base, const char* label) {
    std::uint64_t sb = base + 1024;
    im.put16(sb + 0x38, 0xEF53);
    im.put32(sb + 0x5C, 0x0004);   // has_journal
    im.put32(sb + 0x60, 0x0040);   // extents
    for (int i = 0; i < 16; ++i) im.b(sb + 0x68 + i) = static_cast<std::uint8_t>(0xA0 + i);
    im.putStr(sb + 0x78, label, std::strlen(label));
}

inline void writeSwap(Image& im, std::uint64_t base) { im.putStr(base + 4096 - 10, "SWAPSPACE2", 10); }

}  // namespace wptest
