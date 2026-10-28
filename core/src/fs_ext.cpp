// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// ext2/ext3/ext4 on image files (WP-060), written from the on-disk format
// documentation of the Linux kernel ("ext4 Data Structures and Algorithms").
// No code from e2fsprogs is used.
//
//   create  ext2, ext3 or ext4 with 4 KiB blocks and 256-byte inodes; ext3/4
//           get a journal, ext4 extents. Deliberately without metadata
//           checksums, flex_bg and resize_inode (optional features).
//   grow    adds block groups; the new groups must fit into the existing
//           group descriptor blocks. Handles 1/2/4 KiB blocks, sparse_super,
//           flex_bg, 64bit, uninit_bg/gdt_csum, metadata_csum and resize_inode.
//   shrink  only when everything behind the new end is free (no data or
//           inodes are moved); trailing groups are removed.
//   check   read-only: superblock, descriptors, checksums, bitmaps against the
//           free counters, metadata marked in use.
// Refused: meta_bg, bigalloc, sparse_super2, MMP, external journals and file
// systems that need journal recovery.
#include <algorithm>
#include <cstring>
#include <ctime>
#include <set>
#include <vector>

#include "fs_internal.hpp"
#include "wparted/core/types.hpp"

namespace wparted::fstools::ext {
namespace {

constexpr std::uint32_t kCompatJournal = 0x4, kCompatResizeInode = 0x10, kCompatSparse2 = 0x200;
constexpr std::uint32_t kIncFiletype = 0x2, kIncRecover = 0x4, kIncJournalDev = 0x8, kIncMetaBg = 0x10,
                        kIncExtents = 0x40, kInc64 = 0x80, kIncMmp = 0x100, kIncCsumSeed = 0x2000;
constexpr std::uint32_t kRoSparse = 0x1, kRoLargeFile = 0x2, kRoHugeFile = 0x8, kRoGdtCsum = 0x10, kRoDirNlink = 0x20,
                        kRoExtraIsize = 0x40, kRoBigalloc = 0x200, kRoMetaCsum = 0x400;
constexpr std::uint16_t kInodeUninit = 0x1, kBlockUninit = 0x2, kItableZeroed = 0x4;

void put16(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

std::uint16_t crc16(std::uint16_t crc, const std::uint8_t* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) crc = (crc & 1) ? static_cast<std::uint16_t>((crc >> 1) ^ 0xA001) : static_cast<std::uint16_t>(crc >> 1);
    }
    return crc;
}

bool isPow(std::uint64_t g, std::uint64_t b) {
    if (g == 0) return false;
    while (g % b == 0) g /= b;
    return g == 1;
}

struct Fs {
    std::vector<std::uint8_t> sb;   // 1024 bytes
    std::vector<std::uint8_t> gdt;  // primary descriptor blocks
    std::uint32_t bs = 0, bpg = 0, ipg = 0, inodeSize = 0, descSize = 32, fdb = 0;
    std::uint64_t blocks = 0;
    std::uint32_t groups = 0, gdtBlocks = 0, reservedGdt = 0, itBlocks = 0;
    std::uint32_t compat = 0, incompat = 0, ro = 0;
    std::uint32_t seed = 0;

    bool wide() const { return descSize >= 64; }
    bool metaCsum() const { return (ro & kRoMetaCsum) != 0; }
    bool groupCsum() const { return (ro & (kRoMetaCsum | kRoGdtCsum)) != 0; }
    std::uint8_t* d(std::uint32_t g) { return &gdt[static_cast<std::size_t>(g) * descSize]; }
    const std::uint8_t* d(std::uint32_t g) const { return &gdt[static_cast<std::size_t>(g) * descSize]; }

    std::uint64_t get(std::uint32_t g, int lo, int hi) const {
        return le32(d(g) + lo) | (wide() ? std::uint64_t{le32(d(g) + hi)} << 32 : 0);
    }
    std::uint32_t get16(std::uint32_t g, int lo, int hi) const {
        return le16(d(g) + lo) | (wide() ? std::uint32_t{le16(d(g) + hi)} << 16 : 0);
    }
    void set(std::uint32_t g, int lo, int hi, std::uint64_t v) {
        put32(d(g) + lo, static_cast<std::uint32_t>(v));
        if (wide()) put32(d(g) + hi, static_cast<std::uint32_t>(v >> 32));
    }
    void set16(std::uint32_t g, int lo, int hi, std::uint32_t v) {
        put16(d(g) + lo, v & 0xFFFF);
        if (wide()) put16(d(g) + hi, v >> 16);
    }
    std::uint64_t blockBitmap(std::uint32_t g) const { return get(g, 0x00, 0x20); }
    std::uint64_t inodeBitmap(std::uint32_t g) const { return get(g, 0x04, 0x24); }
    std::uint64_t inodeTable(std::uint32_t g) const { return get(g, 0x08, 0x28); }
    std::uint32_t freeBlocks(std::uint32_t g) const { return get16(g, 0x0C, 0x2C); }
    std::uint32_t freeInodes(std::uint32_t g) const { return get16(g, 0x0E, 0x2E); }
    std::uint32_t usedDirs(std::uint32_t g) const { return get16(g, 0x10, 0x30); }
    std::uint16_t flags(std::uint32_t g) const { return le16(d(g) + 0x12); }

    bool backup(std::uint64_t g) const {
        if (g == 0) return true;
        if (!(ro & kRoSparse)) return true;
        return g == 1 || isPow(g, 3) || isPow(g, 5) || isPow(g, 7);
    }
    std::uint64_t first(std::uint64_t g) const { return fdb + g * bpg; }
    std::uint64_t groupBlocks(std::uint64_t g, std::uint64_t total) const {
        return std::min<std::uint64_t>(bpg, total - first(g));
    }
    // Superblock, descriptors and reserved descriptor blocks at the start of a backup group.
    std::uint32_t headBlocks(std::uint64_t g) const { return backup(g) ? 1 + gdtBlocks + reservedGdt : 0; }
    std::uint32_t descPerBlock() const { return bs / descSize; }

    std::uint64_t sbBlocks() const {
        return le32(&sb[0x04]) | ((incompat & kInc64) ? std::uint64_t{le32(&sb[0x150])} << 32 : 0);
    }
};

void setSbBlocks(Fs& f, std::uint32_t off, std::uint32_t hiOff, std::uint64_t v) {
    put32(&f.sb[off], static_cast<std::uint32_t>(v));
    if (f.incompat & kInc64) put32(&f.sb[hiOff], static_cast<std::uint32_t>(v >> 32));
}
std::uint64_t sbGet64(const Fs& f, std::uint32_t off, std::uint32_t hiOff) {
    return le32(&f.sb[off]) | ((f.incompat & kInc64) ? std::uint64_t{le32(&f.sb[hiOff])} << 32 : 0);
}

bool readSb(BlockReader& r, Fs& f, std::string& err) {
    f.sb.assign(1024, 0);
    if (r.size() < 2048 || !r.read(1024, f.sb.data(), 1024) || le16(&f.sb[0x38]) != 0xEF53) {
        err = "no ext2/3/4 superblock";
        return false;
    }
    const std::uint32_t log = le32(&f.sb[0x18]);
    if (log > 6) {
        err = "invalid ext block size";
        return false;
    }
    f.bs = 1024u << log;
    f.compat = le32(&f.sb[0x5C]);
    f.incompat = le32(&f.sb[0x60]);
    f.ro = le32(&f.sb[0x64]);
    f.bpg = le32(&f.sb[0x20]);
    f.ipg = le32(&f.sb[0x28]);
    f.fdb = le32(&f.sb[0x14]);
    f.inodeSize = le32(&f.sb[0x4C]) >= 1 ? le16(&f.sb[0x58]) : 128;
    f.descSize = (f.incompat & kInc64) ? std::max<std::uint32_t>(32, le16(&f.sb[0xFE])) : 32;
    f.blocks = f.sbBlocks();
    if (f.bpg == 0 || f.bpg > 8 * f.bs || f.ipg == 0 || f.ipg > 8 * f.bs || f.inodeSize < 128 ||
        f.inodeSize > f.bs || f.blocks <= f.fdb || f.descSize > 1024 || (f.descSize & (f.descSize - 1))) {
        err = "invalid ext superblock";
        return false;
    }
    if (f.metaCsum() && f.sb[0x175] != 1) {
        err = "unknown ext checksum type";
        return false;
    }
    f.groups = static_cast<std::uint32_t>((f.blocks - f.fdb + f.bpg - 1) / f.bpg);
    f.gdtBlocks = (f.groups + f.descPerBlock() - 1) / f.descPerBlock();
    f.reservedGdt = (f.compat & kCompatResizeInode) ? le16(&f.sb[0xCE]) : 0;
    f.itBlocks = static_cast<std::uint32_t>((std::uint64_t{f.ipg} * f.inodeSize + f.bs - 1) / f.bs);
    f.seed = (f.incompat & kIncCsumSeed) ? le32(&f.sb[0x270]) : crc32c(0xFFFFFFFFu, &f.sb[0x68], 16);
    return true;
}

bool readFs(BlockReader& r, Fs& f, std::string& err) {
    if (!readSb(r, f, err)) return false;
    f.gdt.assign(static_cast<std::size_t>(f.gdtBlocks) * f.bs, 0);
    if (!r.read((f.fdb + 1) * f.bs, f.gdt.data(), f.gdt.size())) {
        err = "the ext group descriptors cannot be read";
        return false;
    }
    return true;
}

// Features the resize code understands; everything else is refused.
std::string resizeProblem(const Fs& f) {
    if (f.incompat & kIncRecover) return "the journal needs recovery; mount the file system once or run fsck";
    if (f.incompat & (kIncMetaBg | kIncJournalDev | kIncMmp)) return "meta_bg, MMP or external journals are not supported";
    if (f.ro & kRoBigalloc) return "bigalloc is not supported";
    if (f.compat & kCompatSparse2) return "sparse_super2 is not supported";
    if (!(le16(&f.sb[0x3A]) & 1)) return "the file system was not cleanly unmounted; check it with fsck first";
    if (le32(&f.sb[0xE8]) != 0) return "the file system has orphaned inodes; check it with fsck first";
    return {};
}

std::uint16_t descChecksum(const Fs& f, std::uint32_t g, const std::uint8_t* desc) {
    std::uint8_t le[4];
    put32(le, g);
    if (f.metaCsum()) {
        static const std::uint8_t zero[2] = {0, 0};
        std::uint32_t c = crc32c(f.seed, le, 4);
        c = crc32c(c, desc, 0x1E);
        c = crc32c(c, zero, 2);
        if (f.descSize > 0x20) c = crc32c(c, desc + 0x20, f.descSize - 0x20);
        return static_cast<std::uint16_t>(c & 0xFFFF);
    }
    std::uint16_t c = crc16(0xFFFF, &f.sb[0x68], 16);
    c = crc16(c, le, 4);
    c = crc16(c, desc, 0x1E);
    if (f.descSize > 0x20) c = crc16(c, desc + 0x20, f.descSize - 0x20);
    return c;
}

void sealDesc(Fs& f, std::uint32_t g) {
    if (f.groupCsum()) put16(f.d(g) + 0x1E, descChecksum(f, g, f.d(g)));
}

void bitmapCsums(Fs& f, std::uint32_t g, const std::vector<std::uint8_t>* blockBm, const std::vector<std::uint8_t>* inodeBm) {
    if (!f.metaCsum()) return;
    if (blockBm) {
        const std::uint32_t c = crc32c(f.seed, blockBm->data(), f.bpg / 8);
        put16(f.d(g) + 0x18, c & 0xFFFF);
        if (f.descSize >= 0x3C) put16(f.d(g) + 0x38, c >> 16);
    }
    if (inodeBm) {
        const std::uint32_t c = crc32c(f.seed, inodeBm->data(), f.ipg / 8);
        put16(f.d(g) + 0x1A, c & 0xFFFF);
        if (f.descSize >= 0x3C) put16(f.d(g) + 0x3A, c >> 16);
    }
}

void setBit(std::vector<std::uint8_t>& bm, std::uint64_t i) { bm[static_cast<std::size_t>(i / 8)] |= static_cast<std::uint8_t>(1u << (i % 8)); }
void clearBit(std::vector<std::uint8_t>& bm, std::uint64_t i) { bm[static_cast<std::size_t>(i / 8)] &= static_cast<std::uint8_t>(~(1u << (i % 8))); }
bool testBit(const std::vector<std::uint8_t>& bm, std::uint64_t i) { return (bm[static_cast<std::size_t>(i / 8)] >> (i % 8)) & 1; }

// Metadata ranges (first block, count) of group g, wherever they are (flex_bg).
std::vector<std::pair<std::uint64_t, std::uint64_t>> metadata(const Fs& f, std::uint32_t g) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> m;
    if (f.headBlocks(g)) m.push_back({f.first(g), f.headBlocks(g)});
    m.push_back({f.blockBitmap(g), 1});
    m.push_back({f.inodeBitmap(g), 1});
    m.push_back({f.inodeTable(g), f.itBlocks});
    return m;
}

// The block bitmap of group g as Windows would see it on disk; uninitialised
// bitmaps (BLOCK_UNINIT) are computed from the metadata locations.
bool readBlockBitmap(BlockReader& r, const Fs& f, std::uint32_t g, std::vector<std::uint8_t>& bm) {
    bm.assign(f.bs, 0);
    if (f.groupCsum() && (f.flags(g) & kBlockUninit)) {
        const std::uint64_t lo = f.first(g), hi = lo + f.groupBlocks(g, f.blocks);
        for (std::uint32_t h = 0; h < f.groups; ++h)
            for (const auto& m : metadata(f, h))
                for (std::uint64_t b = std::max(m.first, lo); b < std::min(m.first + m.second, hi); ++b) setBit(bm, b - lo);
        for (std::uint64_t i = f.groupBlocks(g, f.blocks); i < 8ull * f.bs; ++i) setBit(bm, i);
        return true;
    }
    return r.read(f.blockBitmap(g) * f.bs, bm.data(), bm.size());
}

bool readInodeBitmap(BlockReader& r, const Fs& f, std::uint32_t g, std::vector<std::uint8_t>& bm) {
    bm.assign(f.bs, 0);
    if (f.groupCsum() && (f.flags(g) & kInodeUninit)) {
        for (std::uint64_t i = f.ipg; i < 8ull * f.bs; ++i) setBit(bm, i);
        return true;
    }
    return r.read(f.inodeBitmap(g) * f.bs, bm.data(), bm.size());
}

bool writeSuperblocks(BlockDevice& d, Fs& f, std::uint32_t groups, std::string& err) {
    for (std::uint32_t g = 0; g < groups; ++g) {
        if (!f.backup(g)) continue;
        std::vector<std::uint8_t> s = f.sb;
        put16(&s[0x5A], g);
        if (f.metaCsum()) put32(&s[0x3FC], crc32c(0xFFFFFFFFu, s.data(), 0x3FC));
        const std::uint64_t at = g == 0 ? 1024 : f.first(g) * f.bs;
        if (!d.write(at, s.data(), s.size())) {
            err = "writing an ext superblock failed";
            return false;
        }
    }
    return true;
}

bool writeDescriptors(BlockDevice& d, const Fs& f, std::uint32_t groups, std::string& err) {
    for (std::uint32_t g = 0; g < groups; ++g)
        if (f.backup(g) && !d.write((f.first(g) + 1) * f.bs, f.gdt.data(), f.gdt.size())) {
            err = "writing the ext group descriptors failed";
            return false;
        }
    return true;
}

std::uint32_t inodeChecksum(const Fs& f, std::uint32_t ino, std::vector<std::uint8_t> in) {
    put16(&in[0x7C], 0);
    const bool hi = f.inodeSize > 128 && le16(&in[0x80]) >= 4;
    if (hi) put16(&in[0x82], 0);
    std::uint8_t le[4];
    put32(le, ino);
    std::uint32_t c = crc32c(f.seed, le, 4);
    put32(le, le32(&in[0x64]));
    c = crc32c(c, le, 4);
    return crc32c(c, in.data(), f.inodeSize);
}

bool inodeIo(BlockDevice* w, BlockReader& r, const Fs& f, std::uint32_t ino, std::vector<std::uint8_t>& in, bool write) {
    const std::uint32_t g = (ino - 1) / f.ipg, idx = (ino - 1) % f.ipg;
    const std::uint64_t at = f.inodeTable(g) * f.bs + std::uint64_t{idx} * f.inodeSize;
    if (!write) {
        in.assign(f.inodeSize, 0);
        return r.read(at, in.data(), in.size());
    }
    if (f.metaCsum()) {
        const std::uint32_t c = inodeChecksum(f, ino, in);
        put16(&in[0x7C], c & 0xFFFF);
        if (f.inodeSize > 128 && le16(&in[0x80]) >= 4) put16(&in[0x82], c >> 16);
    }
    return w->write(at, in.data(), in.size());
}

// resize_inode: every reserved descriptor block of group 0 doubles as an
// indirect block that lists its copies in the backup groups. Adds (or, with
// remove, drops) the copies of groups [from, to).
bool updateResizeInode(BlockDevice& d, const Fs& f, std::uint32_t from, std::uint32_t to, bool remove, std::string& err) {
    if (!f.reservedGdt) return true;
    std::uint32_t changed = 0;
    for (std::uint32_t r = 0; r < f.reservedGdt; ++r) {
        const std::uint64_t primary = f.fdb + 1 + f.gdtBlocks + r;
        std::vector<std::uint8_t> list(f.bs);
        if (!d.read(primary * f.bs, list.data(), list.size())) {
            err = "reading the resize inode failed";
            return false;
        }
        // Slot k holds the copy in the k-th backup group after group 0.
        std::uint32_t slot = 0;
        for (std::uint32_t g = 1; g < to && slot < f.bs / 4; ++g) {
            if (!f.backup(g)) continue;
            if (g >= from) {
                put32(&list[4 * slot], remove ? 0 : static_cast<std::uint32_t>(f.first(g) + 1 + f.gdtBlocks + r));
                if (r == 0) ++changed;
            }
            ++slot;
        }
        if (!d.write(primary * f.bs, list.data(), list.size())) {
            err = "writing the resize inode failed";
            return false;
        }
    }
    if (!changed) return true;
    std::vector<std::uint8_t> in;
    if (!inodeIo(nullptr, d, f, 7, in, false)) {
        err = "reading the resize inode failed";
        return false;
    }
    const std::uint64_t per = std::uint64_t{f.reservedGdt} * (f.bs / 512) * changed;
    std::uint64_t blocks512 = le32(&in[0x1C]) | std::uint64_t{le16(&in[0x74])} << 32;
    blocks512 = remove ? blocks512 - std::min(blocks512, per) : blocks512 + per;
    put32(&in[0x1C], static_cast<std::uint32_t>(blocks512));
    put16(&in[0x74], static_cast<std::uint32_t>(blocks512 >> 32) & 0xFFFF);
    if (!inodeIo(&d, d, f, 7, in, true)) {
        err = "writing the resize inode failed";
        return false;
    }
    return true;
}

// A group needs room for its metadata plus some data; mke2fs drops smaller last groups.
std::uint64_t usableBlocks(const Fs& f, std::uint64_t total) {
    while (total > f.fdb) {
        const std::uint64_t g = (total - f.fdb - 1) / f.bpg;
        const std::uint64_t in = total - f.first(g);
        if (g == 0 || in >= f.headBlocks(g) + 2 + f.itBlocks + 50) break;
        total = f.first(g);
    }
    return total;
}

void scaleReserved(Fs& f, std::uint64_t oldBlocks, std::uint64_t newBlocks) {
    const std::uint64_t r = sbGet64(f, 0x08, 0x154);
    setSbBlocks(f, 0x08, 0x154, oldBlocks ? r * newBlocks / oldBlocks : 0);
}

std::uint64_t sbFree(const Fs& f) { return sbGet64(f, 0x0C, 0x158); }

// ---- grow ---------------------------------------------------------------------

bool grow(BlockDevice& d, Fs& f, std::uint64_t target, std::string& err) {
    const std::uint64_t oldBlocks = f.blocks;
    const std::uint32_t oldGroups = f.groups;
    const std::uint64_t newBlocks = usableBlocks(f, target);
    if (newBlocks <= oldBlocks) return true;  // nothing to add (the partition grew by less than a group needs)
    const std::uint32_t newGroups = static_cast<std::uint32_t>((newBlocks - f.fdb + f.bpg - 1) / f.bpg);
    if (newGroups > f.gdtBlocks * f.descPerBlock()) {
        err = "growing beyond " + formatSize((f.fdb + std::uint64_t{f.gdtBlocks} * f.descPerBlock() * f.bpg) * f.bs) +
              " needs additional group descriptor blocks, which WParted cannot add yet";
        return false;
    }
    std::uint64_t freeAdded = 0;
    // 1. Extend the last old group up to its full size or the new end.
    {
        const std::uint32_t g = oldGroups - 1;
        const std::uint64_t had = f.groupBlocks(g, oldBlocks), now = f.groupBlocks(g, newBlocks);
        if (now > had) {
            std::vector<std::uint8_t> bm;
            if (!readBlockBitmap(d, f, g, bm)) {
                err = "reading an ext block bitmap failed";
                return false;
            }
            for (std::uint64_t i = had; i < now; ++i) clearBit(bm, i);
            if (!(f.groupCsum() && (f.flags(g) & kBlockUninit)) && !d.write(f.blockBitmap(g) * f.bs, bm.data(), bm.size())) {
                err = "writing an ext block bitmap failed";
                return false;
            }
            f.set16(g, 0x0C, 0x2C, f.freeBlocks(g) + static_cast<std::uint32_t>(now - had));
            bitmapCsums(f, g, &bm, nullptr);
            freeAdded += now - had;
        }
    }
    // 2. New groups: their bitmaps and inode table live inside themselves.
    std::vector<std::uint8_t> zeros(static_cast<std::size_t>(f.itBlocks) * f.bs, 0);
    for (std::uint32_t g = oldGroups; g < newGroups; ++g) {
        const std::uint64_t start = f.first(g), size = f.groupBlocks(g, newBlocks);
        const std::uint64_t bb = start + f.headBlocks(g), ib = bb + 1, it = ib + 1;
        const std::uint64_t used = f.headBlocks(g) + 2 + f.itBlocks;
        std::vector<std::uint8_t> bbm(f.bs, 0), ibm(f.bs, 0);
        for (std::uint64_t i = 0; i < used; ++i) setBit(bbm, i);
        for (std::uint64_t i = size; i < 8ull * f.bs; ++i) setBit(bbm, i);
        for (std::uint64_t i = f.ipg; i < 8ull * f.bs; ++i) setBit(ibm, i);
        if (!d.write(bb * f.bs, bbm.data(), bbm.size()) || !d.write(ib * f.bs, ibm.data(), ibm.size()) ||
            !d.write(it * f.bs, zeros.data(), zeros.size())) {
            err = "writing a new ext block group failed";
            return false;
        }
        std::memset(f.d(g), 0, f.descSize);
        f.set(g, 0x00, 0x20, bb);
        f.set(g, 0x04, 0x24, ib);
        f.set(g, 0x08, 0x28, it);
        f.set16(g, 0x0C, 0x2C, static_cast<std::uint32_t>(size - used));
        f.set16(g, 0x0E, 0x2E, f.ipg);
        if (f.groupCsum()) {
            put16(f.d(g) + 0x12, kInodeUninit | kItableZeroed);
            f.set16(g, 0x1C, 0x32, f.ipg);
        }
        bitmapCsums(f, g, &bbm, &ibm);
        freeAdded += size - used;
    }
    for (std::uint32_t g = oldGroups ? oldGroups - 1 : 0; g < newGroups; ++g) sealDesc(f, g);
    // 3. Reserved descriptor copies of new backup groups, then all descriptor copies.
    if (!updateResizeInode(d, f, oldGroups, newGroups, false, err)) return false;
    if (!writeDescriptors(d, f, newGroups, err)) return false;
    // 4. Superblocks last: until here the old size is still valid.
    setSbBlocks(f, 0x04, 0x150, newBlocks);
    scaleReserved(f, oldBlocks, newBlocks);
    setSbBlocks(f, 0x0C, 0x158, sbFree(f) + freeAdded);
    put32(&f.sb[0x00], newGroups * f.ipg);
    put32(&f.sb[0x10], le32(&f.sb[0x10]) + (newGroups - oldGroups) * f.ipg);
    f.blocks = newBlocks;
    f.groups = newGroups;
    return writeSuperblocks(d, f, newGroups, err) && d.flush();
}

// ---- shrink -------------------------------------------------------------------

// Why the file system cannot end at block `newBlocks`; empty if it can.
std::string shrinkBlocker(BlockReader& r, const Fs& f, std::uint64_t newBlocks) {
    if (const std::string p = resizeProblem(f); !p.empty()) return p;
    if (newBlocks <= f.fdb + f.headBlocks(0) + 2 + f.itBlocks + 50) return "the new size is too small";
    const std::uint32_t newGroups = static_cast<std::uint32_t>((newBlocks - f.fdb + f.bpg - 1) / f.bpg);
    // Metadata of the groups that go away may lie behind the new end.
    std::set<std::uint64_t> removable;
    for (std::uint32_t g = newGroups; g < f.groups; ++g) {
        if (f.freeInodes(g) != f.ipg || f.usedDirs(g)) return "inodes are in use behind the new end";
        for (const auto& m : metadata(f, g))
            for (std::uint64_t b = m.first; b < m.first + m.second; ++b) removable.insert(b);
    }
    for (std::uint32_t g = 0; g < newGroups; ++g)
        for (const auto& m : metadata(f, g))
            if (m.first + m.second > newBlocks) return "metadata of a remaining block group lies behind the new end";
    const std::uint32_t lastKept = newGroups - 1;
    for (std::uint32_t g = lastKept; g < f.groups; ++g) {
        std::vector<std::uint8_t> bm;
        if (!readBlockBitmap(r, f, g, bm)) return "an ext block bitmap cannot be read";
        const std::uint64_t n = f.groupBlocks(g, f.blocks);
        for (std::uint64_t i = 0; i < n; ++i) {
            const std::uint64_t b = f.first(g) + i;
            if (b >= newBlocks && testBit(bm, i) && !removable.count(b)) return "data lies behind the new end (WParted does not move ext data yet)";
        }
    }
    return {};
}

bool shrink(BlockDevice& d, Fs& f, std::uint64_t target, std::string& err) {
    const std::uint64_t oldBlocks = f.blocks;
    const std::uint32_t oldGroups = f.groups;
    const std::uint64_t newBlocks = usableBlocks(f, target);
    if (const std::string p = shrinkBlocker(d, f, newBlocks); !p.empty()) {
        err = p;
        return false;
    }
    const std::uint32_t newGroups = static_cast<std::uint32_t>((newBlocks - f.fdb + f.bpg - 1) / f.bpg);
    std::uint64_t freeRemoved = 0, inodesRemoved = 0;
    for (std::uint32_t g = newGroups; g < oldGroups; ++g) {
        freeRemoved += f.freeBlocks(g);
        inodesRemoved += f.ipg;
    }
    // Metadata of removed groups that lives in remaining groups (flex_bg) becomes free.
    std::vector<std::vector<std::uint8_t>> bms(newGroups);
    std::vector<bool> dirty(newGroups, false);
    auto bitmapOf = [&](std::uint32_t g) -> std::vector<std::uint8_t>* {
        if (bms[g].empty() && !readBlockBitmap(d, f, g, bms[g])) return nullptr;
        return &bms[g];
    };
    std::vector<std::uint32_t> freedIn(newGroups, 0);
    for (std::uint32_t g = newGroups; g < oldGroups; ++g)
        for (const auto& m : metadata(f, g))
            for (std::uint64_t b = m.first; b < m.first + m.second; ++b) {
                if (b >= newBlocks) continue;
                const std::uint32_t h = static_cast<std::uint32_t>((b - f.fdb) / f.bpg);
                auto* bm = bitmapOf(h);
                if (!bm) {
                    err = "reading an ext block bitmap failed";
                    return false;
                }
                if (testBit(*bm, b - f.first(h))) {
                    clearBit(*bm, b - f.first(h));
                    ++freedIn[h];
                    dirty[h] = true;
                }
            }
    // The last remaining group may become shorter: its tail was free.
    const std::uint32_t last = newGroups - 1;
    const std::uint64_t had = f.groupBlocks(last, oldBlocks), now = f.groupBlocks(last, newBlocks);
    std::uint64_t tailFree = 0;
    if (now < had) {
        auto* bm = bitmapOf(last);
        if (!bm) {
            err = "reading an ext block bitmap failed";
            return false;
        }
        for (std::uint64_t i = now; i < had; ++i) {
            if (!testBit(*bm, i)) ++tailFree;
            setBit(*bm, i);
        }
        dirty[last] = true;
    }
    for (std::uint32_t g = 0; g < newGroups; ++g) {
        if (!dirty[g]) continue;
        if (f.groupCsum() && (f.flags(g) & kBlockUninit)) put16(f.d(g) + 0x12, f.flags(g) & ~kBlockUninit);
        if (!d.write(f.blockBitmap(g) * f.bs, bms[g].data(), bms[g].size())) {
            err = "writing an ext block bitmap failed";
            return false;
        }
        std::uint32_t free = f.freeBlocks(g) + freedIn[g];
        if (g == last) free -= static_cast<std::uint32_t>(tailFree);
        f.set16(g, 0x0C, 0x2C, free);
        bitmapCsums(f, g, &bms[g], nullptr);
        sealDesc(f, g);
    }
    std::uint64_t freedTotal = 0;
    for (auto n : freedIn) freedTotal += n;
    if (!updateResizeInode(d, f, newGroups, oldGroups, true, err)) return false;
    // Descriptors of the removed groups are cleared; the descriptor block count stays.
    for (std::uint32_t g = newGroups; g < oldGroups; ++g) std::memset(f.d(g), 0, f.descSize);
    if (!writeDescriptors(d, f, newGroups, err)) return false;
    setSbBlocks(f, 0x04, 0x150, newBlocks);
    scaleReserved(f, oldBlocks, newBlocks);
    setSbBlocks(f, 0x0C, 0x158, sbFree(f) + freedTotal - freeRemoved - tailFree);
    put32(&f.sb[0x00], newGroups * f.ipg);
    put32(&f.sb[0x10], le32(&f.sb[0x10]) - static_cast<std::uint32_t>(inodesRemoved));
    f.blocks = newBlocks;
    f.groups = newGroups;
    return writeSuperblocks(d, f, newGroups, err) && d.flush();
}

// ---- create -------------------------------------------------------------------

std::uint64_t journalBlocks(std::uint64_t blocks) {
    if (blocks < 2048) return 0;
    if (blocks < 32768) return 1024;
    if (blocks < 256 * 1024) return 4096;
    if (blocks < 512 * 1024) return 8192;
    if (blocks < 4096 * 1024) return 16384;
    return 32768;
}

// Block map (ext2/ext3) for a contiguous file: data at [data, data+n), the
// indirect blocks are taken from `meta` onwards. Returns metadata blocks used.
std::uint64_t blockMap(std::uint8_t* iblock, std::uint64_t data, std::uint64_t n, std::uint64_t meta,
                       std::uint32_t bs, std::vector<std::pair<std::uint64_t, std::vector<std::uint8_t>>>& writes) {
    const std::uint64_t per = bs / 4;
    std::uint64_t used = 0, i = 0;
    for (; i < n && i < 12; ++i) put32(iblock + 4 * i, static_cast<std::uint32_t>(data + i));
    if (i < n) {  // single indirect
        std::vector<std::uint8_t> ind(bs, 0);
        const std::uint64_t at = meta + used++;
        for (std::uint64_t k = 0; k < per && i < n; ++k, ++i) put32(&ind[static_cast<std::size_t>(4 * k)], static_cast<std::uint32_t>(data + i));
        put32(iblock + 48, static_cast<std::uint32_t>(at));
        writes.push_back({at, ind});
    }
    if (i < n) {  // double indirect
        std::vector<std::uint8_t> dind(bs, 0);
        const std::uint64_t dat = meta + used++;
        for (std::uint64_t k = 0; k < per && i < n; ++k) {
            std::vector<std::uint8_t> ind(bs, 0);
            const std::uint64_t at = meta + used++;
            for (std::uint64_t j = 0; j < per && i < n; ++j, ++i) put32(&ind[static_cast<std::size_t>(4 * j)], static_cast<std::uint32_t>(data + i));
            put32(&dind[static_cast<std::size_t>(4 * k)], static_cast<std::uint32_t>(at));
            writes.push_back({at, ind});
        }
        put32(iblock + 52, static_cast<std::uint32_t>(dat));
        writes.push_back({dat, dind});
    }
    return used;
}

void extentMap(std::uint8_t* iblock, std::uint64_t data, std::uint64_t n) {
    const std::uint32_t entries = static_cast<std::uint32_t>((n + 32767) / 32768);
    put16(iblock + 0, 0xF30A);
    put16(iblock + 2, entries);
    put16(iblock + 4, 4);
    put16(iblock + 6, 0);
    for (std::uint32_t e = 0; e < entries; ++e) {
        std::uint8_t* x = iblock + 12 + 12 * e;
        const std::uint64_t len = std::min<std::uint64_t>(32768, n - 32768ull * e);
        put32(x, 32768u * e);
        put16(x + 4, static_cast<std::uint32_t>(len));
        put16(x + 6, static_cast<std::uint32_t>((data + 32768ull * e) >> 32));
        put32(x + 8, static_cast<std::uint32_t>(data + 32768ull * e));
    }
}

void dirent(std::vector<std::uint8_t>& blk, std::size_t at, std::uint32_t ino, std::uint16_t rec, const char* name) {
    put32(&blk[at], ino);
    put16(&blk[at + 4], rec);
    blk[at + 6] = static_cast<std::uint8_t>(std::strlen(name));
    blk[at + 7] = 2;  // directory
    std::memcpy(&blk[at + 8], name, std::strlen(name));
}

bool makeFs(BlockDevice& d, FsType type, const std::string& label, std::string& err) {
    Fs f;
    f.bs = 4096;
    f.bpg = 8 * f.bs;
    f.inodeSize = 256;
    f.fdb = 0;
    f.descSize = 32;
    f.ro = kRoSparse | kRoLargeFile;
    f.incompat = kIncFiletype;
    f.compat = 0;
    if (type != FsType::Ext2) f.compat |= kCompatJournal;
    if (type == FsType::Ext4) {
        f.incompat |= kIncExtents;
        f.ro |= kRoHugeFile | kRoDirNlink | kRoExtraIsize;
    }
    std::uint64_t blocks = d.size() / f.bs;
    if (blocks < 512 || blocks > 0xFFFFFFFFull) {
        err = "the partition size is outside what WParted creates for ext (2 MiB to 16 TiB)";
        return false;
    }
    // Inodes: one per 16 KiB, a whole number of inode-table blocks per group.
    const std::uint32_t perBlock = f.bs / f.inodeSize;
    auto layoutFor = [&](std::uint64_t total) {
        f.groups = static_cast<std::uint32_t>((total + f.bpg - 1) / f.bpg);
        const std::uint64_t inodes = std::max<std::uint64_t>(total * f.bs / 16384, 16ull * f.groups);
        f.ipg = static_cast<std::uint32_t>(std::min<std::uint64_t>(8ull * f.bs, (inodes / f.groups + perBlock - 1) / perBlock * perBlock));
        f.ipg = std::max<std::uint32_t>(f.ipg, 16);
        f.itBlocks = f.ipg / perBlock;
        f.gdtBlocks = (f.groups + f.descPerBlock() - 1) / f.descPerBlock();
    };
    layoutFor(blocks);
    blocks = usableBlocks(f, blocks);
    layoutFor(blocks);
    f.blocks = blocks;
    const std::uint64_t jblocks = (f.compat & kCompatJournal) ? journalBlocks(blocks) : 0;

    // Group 0: [sb][gdt][bb][ib][itable][root dir][lost+found][journal map][journal]
    const std::uint64_t g0Meta = f.headBlocks(0) + 2 + f.itBlocks;
    const std::uint64_t rootBlk = g0Meta, lfBlk = rootBlk + 1, jStart = lfBlk + 1;
    if (type != FsType::Ext2 && !jblocks) {
        err = "the partition is too small for a journal (ext3 and ext4 need at least 8 MiB)";
        return false;
    }
    std::vector<std::pair<std::uint64_t, std::vector<std::uint8_t>>> jMapWrites;
    std::uint8_t jIblock[60] = {};
    std::uint64_t jMeta = 0;
    if (jblocks) {
        if (type == FsType::Ext4) {
            extentMap(jIblock, jStart, jblocks);
        } else {  // ext3: the indirect blocks come first, the journal data follows them
            const std::uint64_t per = f.bs / 4;
            const std::uint64_t rest = jblocks > 12 ? jblocks - 12 : 0;
            const std::uint64_t ind = rest == 0 ? 0 : rest <= per ? 1 : 2 + (rest - per + per - 1) / per;
            jMeta = blockMap(jIblock, jStart + ind, jblocks, jStart, f.bs, jMapWrites);
        }
    }
    const std::uint64_t jData = jStart + jMeta;
    const std::uint64_t g0Used = jData + jblocks;
    if (g0Used + 16 > f.groupBlocks(0, blocks)) {
        err = "the partition is too small for an ext file system with a journal";
        return false;
    }

    // Superblock.
    f.sb.assign(1024, 0);
    std::uint8_t* s = f.sb.data();
    const std::uint32_t now = static_cast<std::uint32_t>(std::time(nullptr));
    put32(s + 0x00, f.groups * f.ipg);
    put32(s + 0x04, static_cast<std::uint32_t>(blocks));
    put32(s + 0x08, static_cast<std::uint32_t>(blocks / 20));
    put32(s + 0x14, f.fdb);
    put32(s + 0x18, 2);  // 4096
    put32(s + 0x1C, 2);
    put32(s + 0x20, f.bpg);
    put32(s + 0x24, f.bpg);
    put32(s + 0x28, f.ipg);
    put32(s + 0x30, now);
    put16(s + 0x36, 0xFFFF);  // no mount-count check
    put16(s + 0x38, 0xEF53);
    put16(s + 0x3A, 1);  // clean
    put16(s + 0x3C, 1);  // errors: continue
    put32(s + 0x40, now);
    put32(s + 0x4C, 1);  // dynamic revision
    put32(s + 0x54, 11);
    put16(s + 0x58, f.inodeSize);
    put32(s + 0x5C, f.compat);
    put32(s + 0x60, f.incompat);
    put32(s + 0x64, f.ro);
    Guid uuid = randomGuid();
    uuid.bytes[6] = static_cast<std::uint8_t>((uuid.bytes[6] & 0x0F) | 0x40);
    uuid.bytes[8] = static_cast<std::uint8_t>((uuid.bytes[8] & 0x3F) | 0x80);
    std::memcpy(s + 0x68, uuid.bytes.data(), 16);
    std::memcpy(s + 0x78, label.data(), std::min<std::size_t>(label.size(), 16));
    const Guid hash = randomGuid();
    std::memcpy(s + 0xEC, hash.bytes.data(), 16);
    s[0xFC] = 1;  // half_md4
    put32(s + 0x108, now);
    if (type == FsType::Ext4) {
        put16(s + 0x15C, 32);
        put16(s + 0x15E, 32);
    }
    if (jblocks) {
        put32(s + 0xE0, 8);
        s[0xFD] = 1;  // s_jnl_blocks holds a copy of the journal inode's block map
        std::memcpy(s + 0x10C, jIblock, 60);
        put32(s + 0x10C + 64, static_cast<std::uint32_t>(jblocks * f.bs));
    }

    // Groups.
    f.gdt.assign(static_cast<std::size_t>(f.gdtBlocks) * f.bs, 0);
    std::uint64_t freeTotal = 0;
    std::vector<std::uint8_t> zeroTable(static_cast<std::size_t>(f.itBlocks) * f.bs, 0);
    for (std::uint32_t g = 0; g < f.groups; ++g) {
        const std::uint64_t start = f.first(g), size = f.groupBlocks(g, blocks);
        const std::uint64_t bb = start + f.headBlocks(g), ib = bb + 1, it = ib + 1;
        const std::uint64_t used = g == 0 ? g0Used : f.headBlocks(g) + 2 + f.itBlocks;
        std::vector<std::uint8_t> bbm(f.bs, 0), ibm(f.bs, 0);
        for (std::uint64_t i = 0; i < used; ++i) setBit(bbm, i);
        for (std::uint64_t i = size; i < 8ull * f.bs; ++i) setBit(bbm, i);
        for (std::uint64_t i = f.ipg; i < 8ull * f.bs; ++i) setBit(ibm, i);
        if (g == 0)
            for (std::uint64_t i = 0; i < 11; ++i) setBit(ibm, i);  // reserved inodes 1-10 and lost+found (11)
        if (!d.write(bb * f.bs, bbm.data(), bbm.size()) || !d.write(ib * f.bs, ibm.data(), ibm.size()) ||
            !d.write(it * f.bs, zeroTable.data(), zeroTable.size())) {
            err = "writing the ext block groups failed";
            return false;
        }
        f.set(g, 0x00, 0x20, bb);
        f.set(g, 0x04, 0x24, ib);
        f.set(g, 0x08, 0x28, it);
        f.set16(g, 0x0C, 0x2C, static_cast<std::uint32_t>(size - used));
        f.set16(g, 0x0E, 0x2E, g == 0 ? f.ipg - 11 : f.ipg);
        f.set16(g, 0x10, 0x30, g == 0 ? 2 : 0);
        freeTotal += size - used;
    }
    put32(s + 0x0C, static_cast<std::uint32_t>(freeTotal));
    put32(s + 0x10, f.groups * f.ipg - 11);

    // Inodes: root (2), journal (8), lost+found (11).
    auto inode = [&](std::uint16_t mode, std::uint16_t links, std::uint64_t size, std::uint64_t blocks512,
                     const std::uint8_t* iblock, bool extents) {
        std::vector<std::uint8_t> in(f.inodeSize, 0);
        put16(&in[0x00], mode);
        put32(&in[0x04], static_cast<std::uint32_t>(size));
        put32(&in[0x08], now);
        put32(&in[0x0C], now);
        put32(&in[0x10], now);
        put16(&in[0x1A], links);
        put32(&in[0x1C], static_cast<std::uint32_t>(blocks512));
        if (extents) put32(&in[0x20], 0x80000);
        std::memcpy(&in[0x28], iblock, 60);
        put32(&in[0x6C], static_cast<std::uint32_t>(size >> 32));
        put16(&in[0x80], 32);
        return in;
    };
    const bool ext4 = type == FsType::Ext4;
    auto dirMap = [&](std::uint64_t blk, std::uint8_t* iblock) {
        if (ext4) extentMap(iblock, blk, 1);
        else put32(iblock, static_cast<std::uint32_t>(blk));
    };
    std::uint8_t rootMap[60] = {}, lfMap[60] = {};
    dirMap(rootBlk, rootMap);
    dirMap(lfBlk, lfMap);
    const std::uint64_t it0 = f.inodeTable(0) * f.bs;
    const auto root = inode(0x41ED, 3, f.bs, f.bs / 512, rootMap, ext4);
    const auto lf = inode(0x41C0, 2, f.bs, f.bs / 512, lfMap, ext4);
    if (!d.write(it0 + 1ull * f.inodeSize, root.data(), root.size()) ||
        !d.write(it0 + 10ull * f.inodeSize, lf.data(), lf.size())) {
        err = "writing the ext root directory failed";
        return false;
    }
    // inode 1 (bad blocks) only needs times; e2fsck accepts it zeroed.
    std::vector<std::uint8_t> rb(f.bs, 0), lb(f.bs, 0);
    dirent(rb, 0, 2, 12, ".");
    dirent(rb, 12, 2, 12, "..");
    dirent(rb, 24, 11, static_cast<std::uint16_t>(f.bs - 24), "lost+found");
    dirent(lb, 0, 11, 12, ".");
    dirent(lb, 12, 2, static_cast<std::uint16_t>(f.bs - 12), "..");
    if (!d.write(rootBlk * f.bs, rb.data(), rb.size()) || !d.write(lfBlk * f.bs, lb.data(), lb.size())) {
        err = "writing the ext directories failed";
        return false;
    }
    if (jblocks) {
        const auto j = inode(0x8180, 1, jblocks * f.bs, (jblocks + jMeta) * (f.bs / 512), jIblock, ext4);
        if (!d.write(it0 + 7ull * f.inodeSize, j.data(), j.size())) {
            err = "writing the journal inode failed";
            return false;
        }
        for (const auto& w : jMapWrites)
            if (!d.write(w.first * f.bs, w.second.data(), w.second.size())) {
                err = "writing the journal block map failed";
                return false;
            }
        // JBD2 superblock (big endian), journal otherwise zero = empty.
        std::vector<std::uint8_t> js(f.bs, 0);
        auto be = [&](std::size_t at, std::uint32_t v) {
            for (int i = 0; i < 4; ++i) js[at + i] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
        };
        be(0x00, 0xC03B3998u);
        be(0x04, 4);  // superblock v2
        be(0x0C, f.bs);
        be(0x10, static_cast<std::uint32_t>(jblocks));
        be(0x14, 1);  // first log block
        be(0x18, 1);  // sequence
        std::memcpy(&js[0x30], uuid.bytes.data(), 16);
        be(0x40, 1);  // one user
        std::vector<std::uint8_t> zero(f.bs, 0);
        for (std::uint64_t b = 1; b < jblocks; ++b)
            if (!d.write((jData + b) * f.bs, zero.data(), zero.size())) {
                err = "clearing the journal failed";
                return false;
            }
        if (!d.write(jData * f.bs, js.data(), js.size())) {
            err = "writing the journal superblock failed";
            return false;
        }
    }
    // Descriptors and superblocks last.
    if (!writeDescriptors(d, f, f.groups, err) || !writeSuperblocks(d, f, f.groups, err)) return false;
    if (f.bs > 1024) {  // clear the boot area in front of the superblock
        std::vector<std::uint8_t> boot(1024, 0);
        if (!d.write(0, boot.data(), boot.size())) {
            err = "clearing the boot area failed";
            return false;
        }
    }
    return d.flush() || (err = "flush failed", false);
}

}  // namespace

// ---- entry points ---------------------------------------------------------------

bool create(BlockDevice& d, FsType type, const std::string& label, std::string& err) {
    return makeFs(d, type, label, err);
}

bool resize(BlockDevice& d, std::uint64_t newBytes, std::string& err) {
    Fs f;
    if (!readFs(d, f, err)) return false;
    if (const std::string p = resizeProblem(f); !p.empty()) {
        err = p;
        return false;
    }
    const std::uint64_t target = std::min<std::uint64_t>(newBytes, d.size()) / f.bs;
    if (target > f.blocks) return grow(d, f, target, err);
    if (target < f.blocks) return shrink(d, f, target, err);
    return true;
}

std::string shrinkProblem(BlockReader& r, std::uint64_t newBytes) {
    Fs f;
    std::string err;
    if (!readFs(r, f, err)) return err;
    const std::uint64_t target = newBytes / f.bs;
    if (target >= f.blocks) return {};
    return shrinkBlocker(r, f, usableBlocks(f, target));
}

std::string growProblem(BlockReader& r, std::uint64_t newBytes) {
    Fs f;
    std::string err;
    if (!readFs(r, f, err)) return err;
    if (const std::string p = resizeProblem(f); !p.empty()) return p;
    const std::uint64_t newBlocks = usableBlocks(f, newBytes / f.bs);
    const std::uint64_t groups = newBlocks > f.fdb ? (newBlocks - f.fdb + f.bpg - 1) / f.bpg : 0;
    if (groups > std::uint64_t{f.gdtBlocks} * f.descPerBlock())
        return "growing beyond " + formatSize((f.fdb + std::uint64_t{f.gdtBlocks} * f.descPerBlock() * f.bpg) * f.bs) +
               " needs additional group descriptor blocks, which WParted cannot add yet";
    return {};
}

std::uint64_t usedBytes(BlockReader& r) {
    Fs f;
    std::string err;
    if (!readSb(r, f, err)) return 0;
    const std::uint64_t free = sbFree(f);
    return free <= f.blocks ? (f.blocks - free) * f.bs : 0;
}

bool check(BlockReader& r, CheckReport& rep, std::string& err) {
    Fs f;
    if (!readFs(r, f, err)) return false;
    auto error = [&](const std::string& e) {
        rep.ok = false;
        if (rep.errors.size() < 20) rep.errors.push_back(e);
    };
    rep.totalBytes = f.blocks * f.bs;
    if (f.metaCsum() && le32(&f.sb[0x3FC]) != crc32c(0xFFFFFFFFu, f.sb.data(), 0x3FC)) error("superblock checksum is wrong");
    if (f.incompat & kIncRecover) error("the journal needs recovery");
    if (!(le16(&f.sb[0x3A]) & 1)) rep.warnings.push_back("the file system was not cleanly unmounted");
    if (f.blocks * f.bs > r.size()) error("the file system is larger than its partition");
    if (le32(&f.sb[0x00]) != f.groups * f.ipg) error("inode count does not match the block groups");
    std::uint64_t freeBlocks = 0, freeInodes = 0;
    std::vector<std::uint8_t> bm, im;
    for (std::uint32_t g = 0; g < f.groups; ++g) {
        const std::string at = "group " + std::to_string(g) + ": ";
        if (f.groupCsum() && le16(f.d(g) + 0x1E) != descChecksum(f, g, f.d(g))) error(at + "descriptor checksum is wrong");
        bool placed = true;
        for (const auto& m : metadata(f, g))
            if (m.first < f.fdb || m.first + m.second > f.blocks) placed = false;
        if (!placed) {
            error(at + "bitmaps or inode table outside the file system");
            continue;
        }
        if (!readBlockBitmap(r, f, g, bm) || !readInodeBitmap(r, f, g, im)) {
            error(at + "bitmaps cannot be read");
            continue;
        }
        const std::uint64_t n = f.groupBlocks(g, f.blocks);
        std::uint64_t freeB = 0;
        for (std::uint64_t i = 0; i < n; ++i) freeB += !testBit(bm, i);
        bool padding = true;
        for (std::uint64_t i = n; i < f.bpg && padding; ++i) padding = testBit(bm, i);
        if (!padding) error(at + "block bitmap padding is not set");
        std::uint64_t freeI = 0;
        for (std::uint64_t i = 0; i < f.ipg; ++i) freeI += !testBit(im, i);
        if (freeB != f.freeBlocks(g)) error(at + "free block count is " + std::to_string(f.freeBlocks(g)) + ", bitmap says " + std::to_string(freeB));
        if (freeI != f.freeInodes(g)) error(at + "free inode count is " + std::to_string(f.freeInodes(g)) + ", bitmap says " + std::to_string(freeI));
        for (std::uint32_t h = 0; h < f.groups; ++h)
            for (const auto& m : metadata(f, h))
                for (std::uint64_t b = std::max(m.first, f.first(g)); b < std::min(m.first + m.second, f.first(g) + n); ++b)
                    if (!testBit(bm, b - f.first(g))) {
                        error(at + "metadata block " + std::to_string(b) + " is marked free");
                        break;
                    }
        if (f.metaCsum() && !(f.flags(g) & kBlockUninit)) {
            const std::uint32_t c = crc32c(f.seed, bm.data(), f.bpg / 8);
            const std::uint32_t want = le16(f.d(g) + 0x18) | (f.descSize >= 0x3C ? std::uint32_t{le16(f.d(g) + 0x38)} << 16 : 0);
            if ((f.descSize >= 0x3C ? c : (c & 0xFFFF)) != want) error(at + "block bitmap checksum is wrong");
        }
        freeBlocks += freeB;
        freeInodes += freeI;
    }
    if (freeBlocks != sbFree(f))
        rep.warnings.push_back("the superblock free block count is out of date (Linux recalculates it)");
    if (freeInodes != le32(&f.sb[0x10]))
        rep.warnings.push_back("the superblock free inode count is out of date (Linux recalculates it)");
    rep.usedBytes = (f.blocks - std::min(f.blocks, freeBlocks)) * f.bs;
    return true;
}

}  // namespace wparted::fstools::ext
