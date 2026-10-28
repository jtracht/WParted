// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// File-system tools on in-memory partitions (WP-040, WP-041, WP-043, WP-060).
// The same operations are checked against dosfstools, mtools, exfatprogs,
// ntfs-3g and e2fsprogs in CI (tools/ci/fs-tools-check.sh).
#include <cstring>

#include "image_builder.hpp"
#include "test_framework.hpp"
#include "wparted/core/executor.hpp"
#include "wparted/core/fs_tools.hpp"

using namespace wparted;
using wptest::Image;

namespace {

constexpr std::uint64_t MiB = 1024 * 1024;

FsDetection detect(Image& im) {
    MemoryBlockReader r(im.data, im.ss);
    return detectFilesystem(r);
}

fstools::CheckReport checkFat(Image& im) {
    MemoryBlockDevice dev(im.data, im.ss);
    fstools::CheckReport rep;
    std::string err;
    if (!fstools::check(dev, detect(im).type, rep, err)) rep.errors.push_back(err);
    for (const auto& e : rep.errors) std::printf("    check: %s\n", e.c_str());
    rep.ok = rep.errors.empty();
    return rep;
}

// Minimal FAT access for the tests (independent of the implementation).
struct FatView {
    Image& im;
    std::uint32_t bps, spc, rsvd, nfats, fatSz, rootEnt, rootClus;
    bool fat32;
    explicit FatView(Image& i) : im(i) {
        const std::uint8_t* b = im.data.data();
        bps = le16(b + 11);
        spc = b[13];
        rsvd = le16(b + 14);
        nfats = b[16];
        rootEnt = le16(b + 17);
        fat32 = le16(b + 22) == 0;
        fatSz = fat32 ? le32(b + 36) : le16(b + 22);
        rootClus = fat32 ? le32(b + 44) : 0;
    }
    std::uint64_t totSec() const {
        const std::uint8_t* b = im.data.data();
        return le16(b + 19) ? le16(b + 19) : le32(b + 32);
    }
    std::uint64_t dataStart() const { return rsvd + std::uint64_t{nfats} * fatSz + (rootEnt * 32 + bps - 1) / bps; }
    std::uint64_t clusterOff(std::uint32_t c) const { return (dataStart() + std::uint64_t{c - 2} * spc) * bps; }
    std::uint32_t clusters() const { return static_cast<std::uint32_t>((totSec() - dataStart()) / spc); }
    std::uint32_t get(std::uint32_t c) const {
        const std::uint8_t* f = im.data.data() + std::uint64_t{rsvd} * bps;
        return fat32 ? le32(f + 4 * c) & 0x0FFFFFFF : le16(f + 2 * c);
    }
    void set(std::uint32_t c, std::uint32_t v) {
        for (std::uint32_t i = 0; i < nfats; ++i) {
            const std::uint64_t f = (rsvd + std::uint64_t{i} * fatSz) * bps;
            if (fat32) im.put32(f + 4 * c, v);
            else im.put16(f + 2 * c, static_cast<std::uint16_t>(v));
        }
    }
    std::uint64_t rootOff() const { return fat32 ? clusterOff(rootClus) : (rsvd + std::uint64_t{nfats} * fatSz) * bps; }
    // Adds file "DATA.BIN" made of `chain` clusters filled with a pattern.
    void addFile(const std::vector<std::uint32_t>& chain) {
        const std::uint32_t eoc = fat32 ? 0x0FFFFFFF : 0xFFFF;
        for (std::size_t i = 0; i < chain.size(); ++i) {
            set(chain[i], i + 1 < chain.size() ? chain[i + 1] : eoc);
            for (std::uint32_t k = 0; k < spc * bps; k += 4)
                im.put32(clusterOff(chain[i]) + k, static_cast<std::uint32_t>(0x5A000000u | (i << 12) | k));
        }
        std::uint64_t e = rootOff();
        while (im.b(e) != 0) e += 32;
        im.putStr(e, "DATA    BIN", 11);
        im.b(e + 11) = 0x20;
        if (fat32) im.put16(e + 20, static_cast<std::uint16_t>(chain[0] >> 16));
        im.put16(e + 26, static_cast<std::uint16_t>(chain[0] & 0xFFFF));
        im.put32(e + 28, static_cast<std::uint32_t>(chain.size() * spc * bps));
    }
    // Follows DATA.BIN and verifies the pattern.
    bool fileIntact(std::size_t clustersExpected) const {
        std::uint64_t e = rootOff();
        while (im.b(e) != 0 && std::memcmp(&im.data[e], "DATA    BIN", 11) != 0) e += 32;
        if (im.b(e) == 0) return false;
        std::uint32_t c = (fat32 ? std::uint32_t{le16(&im.data[e + 20])} << 16 : 0u) | le16(&im.data[e + 26]);
        for (std::size_t i = 0; i < clustersExpected; ++i) {
            if (c < 2 || c >= clusters() + 2) return false;
            for (std::uint32_t k = 0; k < spc * bps; k += 4)
                if (le32(&im.data[clusterOff(c) + k]) != (0x5A000000u | (i << 12) | k)) return false;
            c = get(c);
        }
        return fat32 ? c >= 0x0FFFFFF8 : c >= 0xFFF8;
    }
};

}  // namespace

TEST("fstools: FAT16 and FAT32 are created, detected and pass the check") {
    for (FsType t : {FsType::Fat16, FsType::Fat32}) {
        Image im(64 * MiB, 512);
        MemoryBlockDevice dev(im.data, 512);
        std::string err;
        REQUIRE(fstools::create(dev, t, "Wparted", 2048, err));
        const auto d = detect(im);
        CHECK(d.type == t);
        CHECK_EQ(d.label, std::string("WPARTED"));
        const auto rep = checkFat(im);
        CHECK(rep.ok);
        CHECK(rep.warnings.empty());
        CHECK(t == FsType::Fat16 || rep.usedBytes > 0);  // the FAT32 root directory uses a cluster
        CHECK(rep.totalBytes > 60 * MiB);
    }
    Image tiny(8 * MiB, 512);
    MemoryBlockDevice dev(tiny.data, 512);
    std::string err;
    CHECK(!fstools::create(dev, FsType::Fat32, "", 0, err));  // FAT32 needs at least ~33 MiB
    CHECK(!fstools::labelProblem(FsType::Fat32, "a.b").empty());
    CHECK(!fstools::labelProblem(FsType::Fat32, "TWELVE CHARS").empty());
    CHECK(fstools::labelProblem(FsType::Ntfs, "Daten \xC3\xA4").empty());
}

TEST("fstools: ext2/3/4 are created, grown and shrunk, and pass the check after each step") {
    for (FsType t : {FsType::Ext2, FsType::Ext3, FsType::Ext4}) {
        Image im(320 * MiB, 512);
        MemoryBlockDevice all(im.data, 512);
        RegionDevice part(all, 0, 64 * MiB);
        std::string err;
        REQUIRE(fstools::create(part, t, "wp-ext", 0, err));
        auto checked = [&](BlockReader& r) {
            fstools::CheckReport rep;
            std::string e;
            const bool ok = fstools::check(r, t, rep, e);
            for (const auto& x : rep.errors) std::printf("    %s: %s\n", fsTypeName(t), x.c_str());
            for (const auto& x : rep.warnings) std::printf("    %s (warning): %s\n", fsTypeName(t), x.c_str());
            return ok && rep.ok && rep.warnings.empty();
        };
        const FsDetection d = detectFilesystem(part);
        CHECK(d.type == t);
        CHECK_EQ(d.label, std::string("wp-ext"));
        CHECK(checked(part));
        const std::uint64_t used = fstools::usedBytes(part, t);
        CHECK(used > 0 && used < 64 * MiB);

        RegionDevice big(all, 0, 300 * MiB);  // 1 group -> 3 groups (group 1 holds a backup)
        REQUIRE(fstools::resize(big, t, 300 * MiB, err));
        CHECK(checked(big));
        CHECK(fstools::usedBytes(big, t) >= used);
        CHECK(detectFilesystem(big).type == t);
        CHECK(fstools::shrinkProblem(big, t, 130 * MiB).empty());
        REQUIRE(fstools::resize(big, t, 130 * MiB, err));
        RegionDevice mid(all, 0, 130 * MiB);
        CHECK(checked(mid));
        // ext3/4: the journal lies behind 4 MiB, so that shrink is refused before
        // anything is written; ext2 has only its root directories there.
        CHECK(fstools::shrinkProblem(mid, t, 4 * MiB).empty() == (t == FsType::Ext2));
    }
    Image tiny(4 * MiB, 512);
    MemoryBlockDevice dev(tiny.data, 512);
    std::string err;
    CHECK(!fstools::create(dev, FsType::Ext4, "", 0, err));  // no room for a journal
    CHECK(fstools::create(dev, FsType::Ext2, "", 0, err));   // ext2 needs none
}

TEST("fstools: exFAT is created with a valid boot region checksum") {
    Image im(64 * MiB, 512);
    MemoryBlockDevice dev(im.data, 512);
    std::string err;
    REQUIRE(fstools::create(dev, FsType::ExFat, "Neu", 2048, err));
    const auto d = detect(im);
    CHECK(d.type == FsType::ExFat);
    CHECK_EQ(d.label, std::string("Neu"));
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < 11 * 512; ++i)
        if (i != 106 && i != 107 && i != 112) sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + im.data[i];
    CHECK_EQ(le32(&im.data[11 * 512]), sum);
    CHECK(std::memcmp(&im.data[0], &im.data[12 * 512], 12 * 512) == 0);  // backup region identical
    CHECK_EQ(le64(&im.data[64]), 2048u);                                 // partition offset
    REQUIRE(fstools::setLabel(dev, FsType::ExFat, "Umbenannt", err));
    CHECK_EQ(detect(im).label, std::string("Umbenannt"));
}

TEST("fstools: FAT32 shrink relocates a file behind the new end") {
    Image im(128 * MiB, 512);
    MemoryBlockDevice dev(im.data, 512);
    std::string err;
    REQUIRE(fstools::create(dev, FsType::Fat32, "SHRINK", 2048, err));
    FatView v(im);
    const std::uint32_t n = v.clusters();
    v.addFile({n - 5, n - 3, 10, n + 1});  // mixed: two at the very end
    REQUIRE(checkFat(im).ok);
    REQUIRE(fstools::resize(dev, FsType::Fat32, 64 * MiB, err));
    FatView after(im);
    CHECK_EQ(after.totSec(), 64 * MiB / 512);
    CHECK(after.fileIntact(4));
    CHECK(checkFat(im).ok);
    CHECK(checkFat(im).warnings.empty());  // FSInfo free count updated too
}

TEST("fstools: FAT16 grow enlarges the FAT and moves the data area") {
    Image im(64 * MiB, 512);
    MemoryBlockDevice mem(im.data, 512);
    RegionDevice old(mem, 0, 16 * MiB);  // created on the first 16 MiB
    std::string err;
    REQUIRE(fstools::create(old, FsType::Fat16, "GROW", 2048, err));
    FatView v(im);
    const std::uint32_t fatBefore = v.fatSz;
    v.addFile({2, 3, v.clusters() + 1});
    REQUIRE(checkFat(im).ok);
    RegionDevice grown(mem, 0, 48 * MiB);
    REQUIRE(fstools::resize(grown, FsType::Fat16, 48 * MiB, err));
    FatView after(im);
    CHECK(after.fatSz > fatBefore);
    CHECK_EQ(after.totSec(), 48 * MiB / 512);
    CHECK(after.fileIntact(3));
    CHECK(checkFat(im).ok);
}

TEST("fstools: the check finds cross-links and damaged chains") {
    Image im(64 * MiB, 512);
    MemoryBlockDevice dev(im.data, 512);
    std::string err;
    REQUIRE(fstools::create(dev, FsType::Fat32, "", 2048, err));
    FatView v(im);
    v.addFile({10, 11});
    v.set(11, 10);  // loop back into itself
    fstools::CheckReport rep;
    REQUIRE(fstools::check(dev, FsType::Fat32, rep, err));
    CHECK(!rep.ok);
    CHECK(!fstools::resize(dev, FsType::Fat32, 48 * MiB, err));  // damaged file systems are not resized
}

TEST("fstools: labels and serials for FAT, exFAT, NTFS, ext4 and swap") {
    std::string err, uuid;
    {
        Image im(64 * MiB, 512);
        MemoryBlockDevice dev(im.data, 512);
        REQUIRE(fstools::create(dev, FsType::Fat16, "OLD", 0, err));
        REQUIRE(fstools::setLabel(dev, FsType::Fat16, "New Name", err));
        CHECK_EQ(detect(im).label, std::string("NEW NAME"));
        REQUIRE(fstools::setNewUuid(dev, FsType::Fat16, uuid, err));
        CHECK_EQ(detect(im).uuid, uuid);
        CHECK(checkFat(im).ok);
    }
    {  // exFAT: boot region checksum must be rewritten (main and backup)
        Image im(8 * MiB, 512);
        im.putStr(3, "EXFAT   ", 8);
        im.put32(80, 128);    // FAT at sector 128
        im.put32(88, 256);    // heap at sector 256
        im.put32(92, 1000);   // clusters
        im.put32(96, 4);      // root directory cluster
        im.b(108) = 9;
        im.b(109) = 3;
        im.b(510) = 0x55;
        im.b(511) = 0xAA;
        im.put32(128 * 512 + 4 * 4, 0xFFFFFFFFu);  // root: one cluster
        MemoryBlockDevice dev(im.data, 512);
        REQUIRE(fstools::setLabel(dev, FsType::ExFat, "Stick", err));
        REQUIRE(fstools::setNewUuid(dev, FsType::ExFat, uuid, err));
        const auto d = detect(im);
        CHECK_EQ(d.label, std::string("Stick"));
        CHECK_EQ(d.uuid, uuid);
        std::uint32_t sum = 0;
        for (std::size_t i = 0; i < 11 * 512; ++i)
            if (i != 106 && i != 107 && i != 112) sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + im.data[i];
        CHECK_EQ(le32(&im.data[11 * 512]), sum);
        CHECK_EQ(le32(&im.data[23 * 512]), sum);
        CHECK(std::memcmp(&im.data[0], &im.data[12 * 512], 512) == 0);
    }
    {  // NTFS: $Volume in $MFT and $MFTMirr, update sequence protected
        Image im(8 * MiB, 512);
        im.putStr(3, "NTFS    ", 8);
        im.put16(11, 512);
        im.b(13) = 8;
        im.put64(0x28, 16383);
        im.put64(0x30, 4);
        im.put64(0x38, 8);
        im.b(0x40) = 0xF6;
        im.b(510) = 0x55;
        im.b(511) = 0xAA;
        for (std::uint64_t base : {4ull * 4096, 8ull * 4096}) {
            const std::uint64_t rec = base + 3 * 1024;
            im.putStr(rec, "FILE", 4);
            im.put16(rec + 4, 0x30);
            im.put16(rec + 6, 3);
            im.put16(rec + 0x30, 1);
            im.put16(rec + 510, 1);
            im.put16(rec + 1022, 1);
            im.put16(rec + 0x14, 0x38);
            im.put32(rec + 0x18, 0x68);   // bytes in use
            im.put32(rec + 0x1C, 1024);   // allocated
            im.put32(rec + 0x38, 0x60);
            im.put32(rec + 0x3C, 0x20);
            im.put32(rec + 0x48, 4);
            im.put16(rec + 0x4C, 0x18);
            im.put16(rec + 0x50, 'A');
            im.put16(rec + 0x52, 'B');
            im.put32(rec + 0x58, 0xFFFFFFFFu);
        }
        MemoryBlockDevice dev(im.data, 512);
        REQUIRE(fstools::setLabel(dev, FsType::Ntfs, "Windows Daten", err));
        REQUIRE(fstools::setNewUuid(dev, FsType::Ntfs, uuid, err));
        const auto d = detect(im);
        CHECK_EQ(d.label, std::string("Windows Daten"));
        CHECK_EQ(d.uuid, uuid);
        CHECK(std::memcmp(&im.data[4 * 4096 + 3 * 1024], &im.data[8 * 4096 + 3 * 1024], 1024) == 0);  // mirror equal
    }
    {  // ext4 with metadata_csum: superblock checksum and csum_seed
        Image im(16 * MiB, 512);
        wptest::writeExt4(im, 0, "old");
        im.put32(1024 + 0x04, 4096);   // blocks
        im.put32(1024 + 0x18, 2);      // 4 KiB blocks
        im.put32(1024 + 0x20, 32768);  // blocks per group
        im.put32(1024 + 0x64, 0x400);  // metadata_csum
        im.b(1024 + 0x175) = 1;        // crc32c
        const std::array<std::uint8_t, 16> oldUuid = [&] {
            std::array<std::uint8_t, 16> a{};
            std::memcpy(a.data(), &im.data[1024 + 0x68], 16);
            return a;
        }();
        MemoryBlockDevice dev(im.data, 512);
        REQUIRE(fstools::setLabel(dev, FsType::Ext4, "home", err));
        REQUIRE(fstools::setNewUuid(dev, FsType::Ext4, uuid, err));
        const auto d = detect(im);
        CHECK_EQ(d.label, std::string("home"));
        CHECK_EQ(d.uuid, uuid);
        CHECK((le32(&im.data[1024 + 0x60]) & 0x2000) != 0);  // csum_seed enabled
        CHECK_EQ(le32(&im.data[1024 + 0x270]), fstools::crc32c(0xFFFFFFFFu, oldUuid.data(), 16));
        CHECK_EQ(le32(&im.data[1024 + 0x3FC]), fstools::crc32c(0xFFFFFFFFu, &im.data[1024], 0x3FC));
    }
    {
        Image im(1 * MiB, 512);
        wptest::writeSwap(im, 0);
        MemoryBlockDevice dev(im.data, 512);
        REQUIRE(fstools::setLabel(dev, FsType::LinuxSwap, "swap1", err));
        REQUIRE(fstools::setNewUuid(dev, FsType::LinuxSwap, uuid, err));
        CHECK_EQ(detect(im).label, std::string("swap1"));
        CHECK_EQ(detect(im).uuid, uuid);
    }
    {  // XFS v5: label in every AG superblock, sb_crc = ~crc32c, meta_uuid keeps the old UUID
        Image im(16 * MiB, 512);
        for (std::uint64_t ag : {0ull, 1ull}) {
            const std::uint64_t o = ag * 1024 * 4096;
            im.putStr(o, "XFSB", 4);
            const std::uint8_t bs[4] = {0, 0, 0x10, 0};  // 4096 big endian
            std::memcpy(&im.data[o + 4], bs, 4);
            const std::uint8_t agb[4] = {0, 0, 0x04, 0};  // 1024 blocks per AG
            std::memcpy(&im.data[o + 84], agb, 4);
            im.b(o + 91) = 2;   // agcount 2
            im.b(o + 101) = 5;  // version 5
            im.b(o + 102) = 2;  // sector size 512 (big endian 0x0200)
            for (int i = 0; i < 16; ++i) im.b(o + 32 + i) = static_cast<std::uint8_t>(0x30 + i);
        }
        MemoryBlockDevice dev(im.data, 512);
        REQUIRE(fstools::setLabel(dev, FsType::Xfs, "xfsdata", err));
        REQUIRE(fstools::setNewUuid(dev, FsType::Xfs, uuid, err));
        CHECK_EQ(detect(im).label, std::string("xfsdata"));
        CHECK_EQ(detect(im).uuid, uuid);
        for (std::uint64_t o : {0ull, 1024ull * 4096}) {
            std::vector<std::uint8_t> sb(im.data.begin() + static_cast<std::ptrdiff_t>(o),
                                         im.data.begin() + static_cast<std::ptrdiff_t>(o + 512));
            const std::uint32_t stored = le32(&sb[224]);
            sb[224] = sb[225] = sb[226] = sb[227] = 0;
            CHECK_EQ(stored, ~fstools::crc32c(0xFFFFFFFFu, sb.data(), 512));
            CHECK_EQ(im.b(o + 248), 0x30);  // sb_meta_uuid = old UUID
            CHECK((im.b(o + 219) & 0x04) != 0);
        }
    }
    {  // btrfs: csum over [0x20, 4096), metadata_uuid keeps the old fsid
        Image im(8 * MiB, 512);
        const std::uint64_t o = 0x10000;
        im.putStr(o + 0x40, "_BHRfS_M", 8);
        im.put64(o + 0x30, o);
        for (int i = 0; i < 16; ++i) im.b(o + 0x20 + i) = static_cast<std::uint8_t>(0x50 + i);
        MemoryBlockDevice dev(im.data, 512);
        REQUIRE(fstools::setLabel(dev, FsType::Btrfs, "pool", err));
        REQUIRE(fstools::setNewUuid(dev, FsType::Btrfs, uuid, err));
        CHECK_EQ(detect(im).label, std::string("pool"));
        CHECK_EQ(detect(im).uuid, uuid);
        CHECK_EQ(le32(&im.data[o]), ~fstools::crc32c(0xFFFFFFFFu, &im.data[o + 0x20], 4096 - 0x20));
        CHECK_EQ(im.b(o + 0x23B), 0x50);
        CHECK((le64(&im.data[o + 0xBC]) & (1ull << 10)) != 0);
    }
    const std::uint8_t text[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK_EQ(fstools::crc32c(0xFFFFFFFFu, text, 9) ^ 0xFFFFFFFFu, 0xE3069283u);  // standard CRC32C check value
}
