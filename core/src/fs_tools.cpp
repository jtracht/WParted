// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// File-system operations on image files (see fs_tools.hpp): dispatcher and
// the label/UUID writers for exFAT, NTFS, ext2/3/4 and Linux swap. FAT lives
// in fs_fat.cpp. Sources: Microsoft exFAT specification, the NTFS on-disk
// documentation of the Linux-NTFS project, the ext4 disk layout in the Linux
// kernel documentation, the Linux swap header (include/linux/swap.h).
#include "wparted/core/fs_tools.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "fs_internal.hpp"
#include "wparted/core/types.hpp"

namespace wparted::fstools {
namespace {

void put16(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

// UTF-16 code units of `s`, or SIZE_MAX if it is not valid UTF-8 / too long.
std::size_t utf16Units(const std::string& s, std::size_t max, std::vector<std::uint8_t>* out = nullptr) {
    std::vector<std::uint8_t> buf(2 * (max + 1), 0);
    if (!utf8ToUtf16le(s, buf.data(), max)) return SIZE_MAX;
    std::size_t n = 0;
    while (n <= max && (buf[2 * n] || buf[2 * n + 1])) ++n;
    if (out) *out = buf;
    return n;
}

bool hasControl(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7F) return true;
    return false;
}

void put64le(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

std::uint32_t randomU32() {
    const Guid g = randomGuid();
    std::uint32_t v = 0;
    std::memcpy(&v, g.bytes.data(), 4);
    return v;
}

// --- exFAT --------------------------------------------------------------------

struct ExFat {
    std::uint32_t bps = 0, cb = 0, fatOffset = 0, heapOffset = 0, clusterCount = 0, root = 0;
};

bool exfatParse(BlockReader& r, ExFat& x, std::vector<std::uint8_t>& boot, std::string& err) {
    boot.assign(512, 0);
    if (!r.read(0, boot.data(), 512) || std::memcmp(&boot[3], "EXFAT   ", 8) != 0) {
        err = "no exFAT boot sector";
        return false;
    }
    const unsigned bpsShift = boot[108], spcShift = boot[109];
    if (bpsShift < 9 || bpsShift > 12 || spcShift > 25 - bpsShift) {
        err = "invalid exFAT boot sector";
        return false;
    }
    x.bps = 1u << bpsShift;
    x.cb = x.bps << spcShift;
    x.fatOffset = le32(&boot[80]);
    x.heapOffset = le32(&boot[88]);
    x.clusterCount = le32(&boot[92]);
    x.root = le32(&boot[96]);
    if (x.bps != r.sectorSize() || x.root < 2 || x.root >= x.clusterCount + 2) {
        err = "exFAT sector size or root directory is invalid for this device";
        return false;
    }
    return true;
}

// Rewrites the boot region (main and backup) with `sector0` and a fresh checksum.
bool exfatWriteBootRegion(BlockDevice& d, const ExFat& x, const std::vector<std::uint8_t>& sector0, std::string& err) {
    std::vector<std::uint8_t> region(static_cast<std::size_t>(x.bps) * 11);
    if (!d.read(0, region.data(), region.size())) {
        err = "exFAT boot region cannot be read";
        return false;
    }
    std::memcpy(region.data(), sector0.data(), 512);
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < region.size(); ++i) {
        if (i == 106 || i == 107 || i == 112) continue;  // VolumeFlags, PercentInUse
        sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + region[i];
    }
    std::vector<std::uint8_t> csum(x.bps);
    for (std::size_t i = 0; i + 4 <= csum.size(); i += 4) put32(&csum[i], sum);
    for (std::uint64_t base : {std::uint64_t{0}, std::uint64_t{12}}) {  // main and backup boot region
        if (!d.write(base * x.bps, region.data(), region.size()) || !d.write((base + 11) * x.bps, csum.data(), csum.size())) {
            err = "writing the exFAT boot region failed";
            return false;
        }
    }
    return d.flush() || (err = "flush failed", false);
}

// Creates an empty exFAT file system (Microsoft exFAT specification 1.00):
// boot region + backup, one FAT, allocation bitmap, up-case table and root
// directory in clusters 2, 3, ...; default cluster sizes from Microsoft.
bool exfatCreate(BlockDevice& d, const std::string& label, std::uint64_t hidden, std::string& err) {
    const std::uint32_t bps = d.sectorSize();
    unsigned bpsShift = 0;
    while ((1u << bpsShift) < bps) ++bpsShift;
    if ((1u << bpsShift) != bps || bpsShift < 9 || bpsShift > 12) {
        err = "unsupported sector size for exFAT";
        return false;
    }
    const std::uint64_t bytes = d.size(), sectors = bytes / bps;
    const std::uint64_t cb = bytes <= 256ull << 20 ? 4096 : bytes <= 32ull << 30 ? 32768 : 131072;
    const std::uint32_t spc = static_cast<std::uint32_t>(std::max<std::uint64_t>(1, cb / bps));
    unsigned spcShift = 0;
    while ((1u << spcShift) < spc) ++spcShift;
    if (sectors < 4096 || sectors > 0xFFFFFFFFull * spc) {
        err = "the partition size does not fit exFAT (at least 2 MiB)";
        return false;
    }
    auto alignUp = [](std::uint64_t v, std::uint64_t a) { return (v + a - 1) / a * a; };
    // From 32 MiB on, FAT and cluster heap start on 1 MiB boundaries (like SD
    // card formats); the gap leaves the FAT room to grow with the volume.
    const std::uint64_t boundary = sectors >= (32ull << 20) / bps ? (1ull << 20) / bps : spc;
    const std::uint32_t fatOffset = static_cast<std::uint32_t>(alignUp(24, std::max<std::uint64_t>(spc, boundary)));
    std::uint32_t fatLength = 1, clusterCount = 0, heapOffset = 0;
    for (int i = 0; i < 32; ++i) {  // FAT length and cluster count depend on each other
        heapOffset = static_cast<std::uint32_t>(alignUp(std::uint64_t{fatOffset} + fatLength, std::max<std::uint64_t>(spc, boundary)));
        clusterCount = static_cast<std::uint32_t>((sectors - heapOffset) / spc);
        const std::uint32_t need = static_cast<std::uint32_t>(alignUp((std::uint64_t{clusterCount} + 2) * 4, bps) / bps);
        if (need <= fatLength) break;
        fatLength = need;
    }
    std::vector<std::uint8_t> u16;
    const std::size_t units = utf16Units(label, 11, &u16);
    if (units == SIZE_MAX || units > 11) {
        err = "label too long for exFAT";
        return false;
    }
    // Up-case table, compressed: identity runs are 0xFFFF + count.
    std::vector<std::uint16_t> up = {0xFFFF, 'a'};
    for (std::uint16_t c = 'a'; c <= 'z'; ++c) up.push_back(static_cast<std::uint16_t>(c - 'a' + 'A'));
    up.push_back(0xFFFF);
    up.push_back(static_cast<std::uint16_t>(0x10000 - ('z' + 1)));
    std::vector<std::uint8_t> upBytes(up.size() * 2);
    for (std::size_t i = 0; i < up.size(); ++i) put16(&upBytes[2 * i], up[i]);
    std::uint32_t upSum = 0;
    for (std::uint8_t b : upBytes) upSum = ((upSum & 1) ? 0x80000000u : 0) + (upSum >> 1) + b;
    const std::uint64_t clusterBytes = std::uint64_t{spc} * bps;
    const std::uint64_t bitmapBytes = (clusterCount + 7) / 8;
    const std::uint32_t bitmapClusters = static_cast<std::uint32_t>((bitmapBytes + clusterBytes - 1) / clusterBytes);
    const std::uint32_t upClusters = static_cast<std::uint32_t>((upBytes.size() + clusterBytes - 1) / clusterBytes);
    const std::uint32_t bitmapFirst = 2, upFirst = bitmapFirst + bitmapClusters, rootFirst = upFirst + upClusters;
    const std::uint32_t usedClusters = bitmapClusters + upClusters + 1;
    if (usedClusters + 2 > clusterCount) {
        err = "the partition is too small for exFAT";
        return false;
    }
    auto heap = [&](std::uint32_t c) { return std::uint64_t{heapOffset} * bps + std::uint64_t{c - 2} * clusterBytes; };
    // Zero boot regions, FAT and the metadata clusters.
    {
        std::vector<std::uint8_t> zero(1 << 20, 0);
        auto clear = [&](std::uint64_t off, std::uint64_t len) {
            for (std::uint64_t o = 0; o < len; o += zero.size())
                if (!d.write(off + o, zero.data(), static_cast<std::size_t>(std::min<std::uint64_t>(zero.size(), len - o)))) return false;
            return true;
        };
        if (!clear(0, std::uint64_t{fatOffset + fatLength} * bps) || !clear(heap(2), std::uint64_t{usedClusters} * clusterBytes)) {
            err = "clearing the exFAT area failed";
            return false;
        }
    }
    // FAT: media entry, end-of-chain markers for the metadata chains.
    std::vector<std::uint8_t> fat(static_cast<std::size_t>((std::uint64_t{rootFirst} + 1) * 4), 0);
    put32(&fat[0], 0xFFFFFFF8u);
    put32(&fat[4], 0xFFFFFFFFu);
    auto chain = [&](std::uint32_t first, std::uint32_t n) {
        for (std::uint32_t i = 0; i < n; ++i) put32(&fat[4 * (first + i)], i + 1 < n ? first + i + 1 : 0xFFFFFFFFu);
    };
    chain(bitmapFirst, bitmapClusters);
    chain(upFirst, upClusters);
    chain(rootFirst, 1);
    // Allocation bitmap: the metadata clusters are in use.
    std::vector<std::uint8_t> bitmap(static_cast<std::size_t>(bitmapBytes), 0);
    for (std::uint32_t i = 0; i < usedClusters; ++i) bitmap[i / 8] = static_cast<std::uint8_t>(bitmap[i / 8] | (1u << (i % 8)));
    // Root directory: label, bitmap and up-case entries.
    std::vector<std::uint8_t> root(32 * 3, 0);
    root[0] = units ? 0x83 : 0x03;
    root[1] = static_cast<std::uint8_t>(units);
    std::memcpy(&root[2], u16.data(), 2 * units);
    root[32] = 0x81;
    put32(&root[32 + 20], bitmapFirst);
    put64le(&root[32 + 24], bitmapBytes);
    root[64] = 0x82;
    put32(&root[64 + 4], upSum);
    put32(&root[64 + 20], upFirst);
    put64le(&root[64 + 24], upBytes.size());
    if (!d.write(std::uint64_t{fatOffset} * bps, fat.data(), fat.size()) || !d.write(heap(bitmapFirst), bitmap.data(), bitmap.size()) ||
        !d.write(heap(upFirst), upBytes.data(), upBytes.size()) || !d.write(heap(rootFirst), root.data(), root.size())) {
        err = "writing exFAT metadata failed";
        return false;
    }
    // Boot region: main boot sector + 8 extended boot sectors with signatures.
    std::vector<std::uint8_t> boot(bps, 0);
    boot[0] = 0xEB;
    boot[1] = 0x76;
    boot[2] = 0x90;
    std::memcpy(&boot[3], "EXFAT   ", 8);
    put64le(&boot[64], hidden);
    put64le(&boot[72], sectors);
    put32(&boot[80], fatOffset);
    put32(&boot[84], fatLength);
    put32(&boot[88], heapOffset);
    put32(&boot[92], clusterCount);
    put32(&boot[96], rootFirst);
    put32(&boot[100], randomU32());
    put16(&boot[104], 0x0100);
    boot[108] = static_cast<std::uint8_t>(bpsShift);
    boot[109] = static_cast<std::uint8_t>(spcShift);
    boot[110] = 1;
    boot[111] = 0x80;
    boot[112] = static_cast<std::uint8_t>(std::min<std::uint64_t>(100, std::uint64_t{usedClusters} * 100 / clusterCount));
    boot[510] = 0x55;
    boot[511] = 0xAA;
    std::vector<std::uint8_t> ext(bps, 0);
    put32(&ext[bps - 4], 0xAA550000u);
    for (std::uint32_t s = 1; s <= 8; ++s)
        if (!d.write(std::uint64_t{s} * bps, ext.data(), ext.size()) || !d.write(std::uint64_t{s + 12} * bps, ext.data(), ext.size())) {
            err = "writing the exFAT boot region failed";
            return false;
        }
    ExFat x;
    x.bps = bps;
    return exfatWriteBootRegion(d, x, boot, err);
}

bool exfatSetSerial(BlockDevice& d, std::uint32_t serial, std::string& err) {
    ExFat x;
    std::vector<std::uint8_t> boot;
    if (!exfatParse(d, x, boot, err)) return false;
    put32(&boot[100], serial);
    return exfatWriteBootRegion(d, x, boot, err);
}

bool exfatSetLabel(BlockDevice& d, const std::string& label, std::string& err) {
    ExFat x;
    std::vector<std::uint8_t> boot;
    if (!exfatParse(d, x, boot, err)) return false;
    std::vector<std::uint8_t> u16;
    const std::size_t units = utf16Units(label, 11, &u16);
    if (units == SIZE_MAX || units > 11) {
        err = "label too long for exFAT";
        return false;
    }
    std::uint8_t entry[32] = {};
    entry[0] = units ? 0x83 : 0x03;  // in use / no label
    entry[1] = static_cast<std::uint8_t>(units);
    std::memcpy(&entry[2], u16.data(), 2 * units);
    std::uint64_t freeAt = 0;
    bool haveFree = false;
    std::vector<std::uint8_t> buf(x.cb);
    std::uint32_t c = x.root;
    for (std::uint32_t steps = 0; c >= 2 && c < x.clusterCount + 2 && steps <= x.clusterCount; ++steps) {
        const std::uint64_t at = std::uint64_t{x.heapOffset} * x.bps + std::uint64_t{c - 2} * x.cb;
        if (!d.read(at, buf.data(), buf.size())) {
            err = "exFAT root directory cannot be read";
            return false;
        }
        for (std::uint32_t o = 0; o + 32 <= x.cb; o += 32) {
            const std::uint8_t type = buf[o];
            if (type == 0x83 || type == 0x03) {
                if (!d.write(at + o, entry, 32)) {
                    err = "writing the exFAT label entry failed";
                    return false;
                }
                return d.flush() || (err = "flush failed", false);
            }
            if ((type == 0x00 || (type & 0x80) == 0) && !haveFree) {
                haveFree = true;
                freeAt = at + o;
            }
            if (type == 0x00) break;
        }
        std::uint8_t next[4];
        if (!d.read(std::uint64_t{x.fatOffset} * x.bps + 4ull * c, next, 4)) break;
        c = le32(next);
    }
    if (!units) return true;  // no label entry and none wanted
    if (!haveFree) {
        err = "the exFAT root directory has no free entry for the label";
        return false;
    }
    if (!d.write(freeAt, entry, 32)) {
        err = "writing the exFAT label entry failed";
        return false;
    }
    return d.flush() || (err = "flush failed", false);
}

// Grow or shrink exFAT in place (exFAT specification 1.00). Grow: the FAT may
// use the space up to the cluster heap; the allocation bitmap gets clusters
// from the added area if it needs more. Shrink: only if every cluster behind
// the new end is free. TexFAT (two FATs) and dirty volumes are refused.
struct ExFatBitmap {
    std::uint64_t entryAt = 0;            // byte offset of the bitmap directory entry
    std::uint64_t length = 0;             // bitmap size in bytes
    std::vector<std::uint32_t> clusters;  // bitmap clusters in chain order
};

bool exfatChain(BlockReader& r, const ExFat& x, std::uint32_t first, std::vector<std::uint32_t>& out) {
    out.clear();
    for (std::uint32_t c = first; c >= 2 && c < x.clusterCount + 2;) {
        if (out.size() > x.clusterCount) return false;  // loop
        out.push_back(c);
        std::uint8_t next[4];
        if (!r.read(std::uint64_t{x.fatOffset} * x.bps + 4ull * c, next, 4)) return false;
        c = le32(next);
    }
    return !out.empty();
}

bool exfatFindBitmap(BlockReader& r, const ExFat& x, ExFatBitmap& bm, std::string& err) {
    std::vector<std::uint32_t> root;
    if (!exfatChain(r, x, x.root, root)) {
        err = "the exFAT root directory chain is invalid";
        return false;
    }
    std::vector<std::uint8_t> buf(x.cb);
    for (std::uint32_t c : root) {
        const std::uint64_t at = std::uint64_t{x.heapOffset} * x.bps + std::uint64_t{c - 2} * x.cb;
        if (!r.read(at, buf.data(), buf.size())) break;
        for (std::uint32_t o = 0; o + 32 <= x.cb; o += 32) {
            if (buf[o] == 0x00) break;
            if (buf[o] == 0x81 && (buf[o + 1] & 1) == 0) {
                bm.entryAt = at + o;
                bm.length = le64(&buf[o + 24]);
                std::vector<std::uint32_t> chain;
                if (!exfatChain(r, x, le32(&buf[o + 20]), chain)) {
                    err = "the exFAT allocation bitmap chain is invalid";
                    return false;
                }
                bm.clusters = chain;
                return true;
            }
        }
    }
    err = "the exFAT allocation bitmap was not found";
    return false;
}

bool exfatBitmapIo(BlockDevice* w, BlockReader& r, const ExFat& x, const ExFatBitmap& bm, std::vector<std::uint8_t>& bits,
                   bool write) {
    std::size_t done = 0;
    for (std::uint32_t c : bm.clusters) {
        if (done >= bits.size()) break;
        const std::size_t n = std::min<std::size_t>(x.cb, bits.size() - done);
        const std::uint64_t at = std::uint64_t{x.heapOffset} * x.bps + std::uint64_t{c - 2} * x.cb;
        if (write ? !w->write(at, &bits[done], n) : !r.read(at, &bits[done], n)) return false;
        done += n;
    }
    return done >= bits.size();
}

// Moves every file, directory, the bitmap and the up-case table out of the
// clusters that a shrink removes. A moved item gets a FAT chain. Children are
// handled after their parent moved, so each directory entry is updated where
// it lies now. Data is copied before anything refers to it.
class ExFatMover {
public:
    ExFatMover(BlockDevice& d, const ExFat& x, std::vector<std::uint8_t>& bits, std::uint32_t limit)
        : d_(d), x_(x), bits_(bits), limit_(limit), buf_(x.cb) {}

    bool run(std::uint32_t& root, std::string& err) {
        std::vector<std::uint32_t> chain;
        if (!exfatChain(d_, x_, root, chain)) {  // the root directory always uses the FAT
            err = "the exFAT root directory chain is invalid";
            return false;
        }
        if (!moveChain(chain, err)) return false;
        root = chain.front();
        return walk(chain, err);
    }

private:
    std::uint64_t at(std::uint32_t c) const { return std::uint64_t{x_.heapOffset} * x_.bps + std::uint64_t{c - 2} * x_.cb; }
    bool used(std::uint32_t c) const { return (bits_[(c - 2) / 8] >> ((c - 2) % 8)) & 1; }
    void mark(std::uint32_t c, bool on) {
        auto& b = bits_[(c - 2) / 8];
        b = static_cast<std::uint8_t>(on ? b | (1u << ((c - 2) % 8)) : b & ~(1u << ((c - 2) % 8)));
    }
    bool putFat(std::uint32_t c, std::uint32_t v) {
        std::uint8_t b[4];
        put32(b, v);
        return d_.write(std::uint64_t{x_.fatOffset} * x_.bps + 4ull * c, b, 4);
    }
    bool chainOf(std::uint32_t first, std::uint64_t bytes, bool contiguous, std::vector<std::uint32_t>& out) const {
        if (!contiguous) return exfatChain(d_, x_, first, out);
        out.clear();
        const std::uint64_t n = (bytes + x_.cb - 1) / x_.cb;
        if (first < 2 || first + n > x_.clusterCount + 2ull) return false;
        for (std::uint64_t i = 0; i < n; ++i) out.push_back(static_cast<std::uint32_t>(first + i));
        return true;
    }
    // Moves the clusters behind the limit; afterwards the FAT holds the whole chain.
    bool moveChain(std::vector<std::uint32_t>& chain, std::string& err) {
        bool moved = false;
        for (auto& c : chain) {
            if (c < limit_ + 2) continue;
            while (next_ < limit_ && used(next_ + 2)) ++next_;
            if (next_ >= limit_) {
                err = "there is not enough free space in front of the new end";
                return false;
            }
            const std::uint32_t to = next_ + 2;
            if (!d_.read(at(c), buf_.data(), x_.cb) || !d_.write(at(to), buf_.data(), x_.cb) || !d_.flush()) {
                err = "copying exFAT data failed";
                return false;
            }
            mark(to, true);
            mark(c, false);
            if (!putFat(c, 0)) {
                err = "updating the exFAT FAT failed";
                return false;
            }
            c = to;
            moved = true;
        }
        if (!moved) return true;
        for (std::size_t i = 0; i < chain.size(); ++i)
            if (!putFat(chain[i], i + 1 < chain.size() ? chain[i + 1] : 0xFFFFFFFFu)) {
                err = "updating the exFAT FAT failed";
                return false;
            }
        return true;
    }
    bool walk(const std::vector<std::uint32_t>& dirClusters, std::string& err) {
        if (++depth_ > 256) {
            err = "the exFAT directories are nested too deeply";
            return false;
        }
        std::vector<std::uint8_t> dir(dirClusters.size() * x_.cb);
        for (std::size_t i = 0; i < dirClusters.size(); ++i)
            if (!d_.read(at(dirClusters[i]), &dir[i * x_.cb], x_.cb)) {
                err = "an exFAT directory cannot be read";
                return false;
            }
        auto save = [&](std::size_t o, std::size_t n) {  // entry bytes may cross a cluster boundary
            for (std::size_t i = o / x_.cb; i <= (o + n - 1) / x_.cb; ++i)
                if (!d_.write(at(dirClusters[i]), &dir[i * x_.cb], x_.cb)) return false;
            return true;
        };
        std::vector<std::vector<std::uint32_t>> subdirs;
        for (std::size_t o = 0; o + 32 <= dir.size(); o += 32) {
            const std::uint8_t type = dir[o];
            if (type == 0x00) break;
            if (type == 0x81 || type == 0x82) {  // allocation bitmap, up-case table
                const std::uint64_t bytes = le64(&dir[o + 24]);
                const std::uint32_t first = le32(&dir[o + 20]);
                std::vector<std::uint32_t> chain;
                // They may be contiguous without a usable FAT chain.
                if (!exfatChain(d_, x_, first, chain) || chain.size() * std::uint64_t{x_.cb} < bytes) {
                    if (!chainOf(first, bytes, true, chain)) {
                        err = "an exFAT system file has an invalid chain";
                        return false;
                    }
                }
                std::vector<std::uint32_t> before = chain;
                if (!moveChain(chain, err)) return false;
                if (chain != before) {
                    put32(&dir[o + 20], chain.front());
                    if (!save(o, 32)) {
                        err = "updating an exFAT directory failed";
                        return false;
                    }
                }
            } else if (type == 0x85) {
                const std::size_t count = dir[o + 1];
                const std::size_t setBytes = (count + 1) * 32;
                if (count < 2 || o + setBytes > dir.size() || dir[o + 32] != 0xC0) {
                    err = "an exFAT directory entry set is invalid";
                    return false;
                }
                const std::size_t s = o + 32;
                const bool isDir = (le16(&dir[o + 4]) & 0x10) != 0;
                const bool noChain = (dir[s + 1] & 0x02) != 0;
                const std::uint32_t first = le32(&dir[s + 20]);
                const std::uint64_t bytes = le64(&dir[s + 24]);
                if (first) {
                    std::vector<std::uint32_t> chain;
                    if (!chainOf(first, bytes, noChain, chain)) {
                        err = "an exFAT file has an invalid chain";
                        return false;
                    }
                    const std::vector<std::uint32_t> before = chain;
                    if (!moveChain(chain, err)) return false;
                    if (chain != before) {
                        dir[s + 1] = static_cast<std::uint8_t>(dir[s + 1] & ~0x02);
                        put32(&dir[s + 20], chain.front());
                        std::uint16_t sum = 0;  // SetChecksum over the whole set, skipping its own two bytes
                        for (std::size_t i = 0; i < setBytes; ++i) {
                            if (i == 2 || i == 3) continue;
                            sum = static_cast<std::uint16_t>(((sum << 15) | (sum >> 1)) + dir[o + i]);
                        }
                        dir[o + 2] = static_cast<std::uint8_t>(sum);
                        dir[o + 3] = static_cast<std::uint8_t>(sum >> 8);
                        if (!save(o, setBytes)) {
                            err = "updating an exFAT directory failed";
                            return false;
                        }
                    }
                    if (isDir) subdirs.push_back(chain);
                }
                o += count * 32;
            }
        }
        for (const auto& sub : subdirs)
            if (!walk(sub, err)) return false;
        --depth_;
        return true;
    }

    BlockDevice& d_;
    const ExFat& x_;
    std::vector<std::uint8_t>& bits_;
    std::uint32_t limit_;
    std::vector<std::uint8_t> buf_;
    std::uint32_t next_ = 0;
    int depth_ = 0;
};

std::string exfatResizeProblem(BlockReader& r, std::uint64_t newBytes) {
    ExFat x;
    std::vector<std::uint8_t> boot;
    std::string err;
    if (!exfatParse(r, x, boot, err)) return err;
    if (boot[110] != 1) return "exFAT with two FATs (TexFAT) is not supported";
    if (le16(&boot[106]) & 0x0002) return "the exFAT volume is marked dirty; check it in Windows first";
    const std::uint32_t spc = x.cb / x.bps;
    const std::uint64_t sectors = newBytes / x.bps;
    if (sectors <= std::uint64_t{x.heapOffset} + spc * 16ull) return "the new size is too small for this exFAT volume";
    const std::uint64_t want = std::min<std::uint64_t>((sectors - x.heapOffset) / spc, 0xFFFFFFF5ull);
    const std::uint64_t fatRoom = std::uint64_t{x.heapOffset - x.fatOffset} * x.bps / 4;
    if (want + 2 > fatRoom)
        return "growing to this size needs a larger FAT than the space in front of the cluster heap allows (at most " +
               formatSize((fatRoom - 2) * x.cb + std::uint64_t{x.heapOffset} * x.bps) + ")";
    if (want < x.clusterCount) {
        ExFatBitmap bm;
        if (!exfatFindBitmap(r, x, bm, err)) return err;
        std::vector<std::uint8_t> bits(static_cast<std::size_t>(bm.length));
        if (!exfatBitmapIo(nullptr, r, x, bm, bits, false)) return "the exFAT allocation bitmap cannot be read";
        std::uint64_t used = 0;
        for (std::uint64_t c = 0; c < x.clusterCount; ++c) used += (bits[static_cast<std::size_t>(c / 8)] >> (c % 8)) & 1;
        if (used + 4 > want) return "the data does not fit into the new size";
    }
    return {};
}

bool exfatResize(BlockDevice& d, std::uint64_t newBytes, std::string& err) {
    if (const std::string p = exfatResizeProblem(d, newBytes); !p.empty()) {
        err = p;
        return false;
    }
    ExFat x;
    std::vector<std::uint8_t> boot;
    if (!exfatParse(d, x, boot, err)) return false;
    const std::uint32_t spc = x.cb / x.bps;
    const std::uint64_t sectors = newBytes / x.bps;
    const std::uint32_t count = static_cast<std::uint32_t>(std::min<std::uint64_t>((sectors - x.heapOffset) / spc, 0xFFFFFFF5ull));
    if (count == x.clusterCount) return true;
    if (count < x.clusterCount) {  // move what lies behind the new end first
        ExFatBitmap bm0;
        if (!exfatFindBitmap(d, x, bm0, err)) return false;
        std::vector<std::uint8_t> bits0(static_cast<std::size_t>(bm0.length));
        if (!exfatBitmapIo(nullptr, d, x, bm0, bits0, false)) {
            err = "the exFAT allocation bitmap cannot be read";
            return false;
        }
        bool behind = false;
        for (std::uint64_t c = count; c < x.clusterCount; ++c)
            behind = behind || ((bits0[static_cast<std::size_t>(c / 8)] >> (c % 8)) & 1);
        if (behind) {
            std::uint32_t root = x.root;
            ExFatMover mover(d, x, bits0, count);
            if (!mover.run(root, err)) return false;
            ExFatBitmap moved;
            if (root != x.root) {
                put32(&boot[96], root);
                x.root = root;
                if (!exfatWriteBootRegion(d, x, boot, err)) return false;
            }
            if (!exfatFindBitmap(d, x, moved, err)) return false;
            if (!exfatBitmapIo(&d, d, x, moved, bits0, true) || !d.flush()) {
                err = "writing the exFAT allocation bitmap failed";
                return false;
            }
        }
    }
    ExFatBitmap bm;
    if (!exfatFindBitmap(d, x, bm, err)) return false;
    std::vector<std::uint8_t> bits(static_cast<std::size_t>(bm.length));
    if (!exfatBitmapIo(nullptr, d, x, bm, bits, false)) {
        err = "the exFAT allocation bitmap cannot be read";
        return false;
    }
    const std::uint64_t newLength = (std::uint64_t{count} + 7) / 8;
    const std::uint64_t oldFatLength = le32(&boot[84]);
    const std::uint64_t needFat = ((std::uint64_t{count} + 2) * 4 + x.bps - 1) / x.bps;
    auto putFat = [&](std::uint32_t c, std::uint32_t v) {
        std::uint8_t b[4];
        put32(b, v);
        return d.write(std::uint64_t{x.fatOffset} * x.bps + 4ull * c, b, 4);
    };
    if (count > x.clusterCount) {
        // Larger FAT: the added sectors start out free (zero).
        if (needFat > oldFatLength) {
            std::vector<std::uint8_t> zero(static_cast<std::size_t>((needFat - oldFatLength) * x.bps), 0);
            if (!d.write((x.fatOffset + oldFatLength) * x.bps, zero.data(), zero.size())) {
                err = "extending the exFAT FAT failed";
                return false;
            }
            put32(&boot[84], static_cast<std::uint32_t>(needFat));
        }
        bits.resize(static_cast<std::size_t>(newLength), 0);
        // More bitmap clusters, taken from the added area and chained in the FAT.
        std::uint32_t nextFree = x.clusterCount + 2;
        while (std::uint64_t{bm.clusters.size()} * x.cb < newLength) {
            const std::uint32_t c = nextFree++;
            if (!putFat(bm.clusters.back(), c) || !putFat(c, 0xFFFFFFFFu)) {
                err = "extending the exFAT bitmap chain failed";
                return false;
            }
            bm.clusters.push_back(c);
            bits[static_cast<std::size_t>((c - 2) / 8)] |= static_cast<std::uint8_t>(1u << ((c - 2) % 8));
        }
    } else {
        bits.resize(static_cast<std::size_t>(newLength));
        if (count % 8) bits.back() &= static_cast<std::uint8_t>((1u << (count % 8)) - 1);
    }
    std::vector<std::uint8_t> full(static_cast<std::size_t>(bm.clusters.size() * x.cb), 0);
    std::memcpy(full.data(), bits.data(), bits.size());
    ExFatBitmap whole = bm;
    if (!exfatBitmapIo(&d, d, x, whole, full, true)) {
        err = "writing the exFAT allocation bitmap failed";
        return false;
    }
    std::uint8_t len[8];
    put64le(len, newLength);
    if (!d.write(bm.entryAt + 24, len, 8)) {
        err = "updating the exFAT bitmap entry failed";
        return false;
    }
    std::uint64_t used = 0;
    for (std::uint32_t c = 0; c < count; ++c) used += (bits[c / 8] >> (c % 8)) & 1;
    put64le(&boot[72], sectors);
    put32(&boot[92], count);
    boot[112] = static_cast<std::uint8_t>(used * 100 / count);
    return exfatWriteBootRegion(d, x, boot, err);
}

// --- NTFS ---------------------------------------------------------------------

struct Ntfs {
    std::uint32_t bps = 0;
    std::uint64_t cluster = 0, recSize = 0, mftLcn = 0, mirrLcn = 0, totalSectors = 0;
};

bool ntfsParse(BlockReader& r, Ntfs& n, std::vector<std::uint8_t>& boot, std::string& err) {
    boot.assign(std::max<std::uint32_t>(512, r.sectorSize()), 0);
    if (!r.read(0, boot.data(), boot.size()) || std::memcmp(&boot[3], "NTFS    ", 8) != 0) {
        err = "no NTFS boot sector";
        return false;
    }
    n.bps = le16(&boot[11]);
    const std::uint8_t spc = boot[13];
    if (n.bps < 256 || n.bps > 4096 || (n.bps & (n.bps - 1)) || spc == 0 || (spc > 0x80 && 256 - spc > 20) ||
        n.bps != r.sectorSize()) {
        err = "invalid NTFS boot sector for this device";
        return false;
    }
    n.cluster = spc <= 0x80 ? std::uint64_t{spc} * n.bps : std::uint64_t{n.bps} << (256 - spc);
    const auto raw = static_cast<std::int8_t>(boot[0x40]);
    n.recSize = raw > 0 ? raw * n.cluster : (raw > -31 ? 1ull << -raw : 0);
    n.mftLcn = le64(&boot[0x30]);
    n.mirrLcn = le64(&boot[0x38]);
    n.totalSectors = le64(&boot[0x28]);
    if (n.recSize < 512 || n.recSize > 65536 || n.mftLcn > r.size() / n.cluster || n.mirrLcn > r.size() / n.cluster) {
        err = "invalid NTFS MFT location";
        return false;
    }
    return true;
}

bool ntfsSetSerial(BlockDevice& d, const Guid& rnd, std::string& err) {
    Ntfs n;
    std::vector<std::uint8_t> boot;
    if (!ntfsParse(d, n, boot, err)) return false;
    std::memcpy(&boot[0x48], rnd.bytes.data(), 8);
    if (!d.write(0, boot.data(), boot.size())) {
        err = "writing the NTFS boot sector failed";
        return false;
    }
    // Backup boot sector: the sector after the volume (end of the partition).
    const std::uint64_t backup = n.totalSectors * n.bps;
    std::vector<std::uint8_t> b2(boot.size());
    if (backup + b2.size() <= d.size() && d.read(backup, b2.data(), b2.size()) && std::memcmp(&b2[3], "NTFS    ", 8) == 0) {
        std::memcpy(&b2[0x48], rnd.bytes.data(), 8);
        if (!d.write(backup, b2.data(), b2.size())) {
            err = "writing the NTFS backup boot sector failed";
            return false;
        }
    }
    return d.flush() || (err = "flush failed", false);
}

// Applies (unpack) or creates (pack) the update sequence protection of an MFT record.
bool ntfsFixups(std::vector<std::uint8_t>& rec, bool pack) {
    const std::size_t usaOff = le16(&rec[4]), usaCount = le16(&rec[6]);
    if (usaCount == 0 || usaOff + 2 * usaCount > rec.size()) return false;
    std::uint16_t usn = le16(&rec[usaOff]);
    if (pack) {
        usn = static_cast<std::uint16_t>(usn + 1);
        if (usn == 0 || usn == 0xFFFF) usn = 1;
        put16(&rec[usaOff], usn);
    }
    for (std::size_t i = 1; i < usaCount; ++i) {
        const std::size_t pos = i * 512 - 2;
        if (pos + 2 > rec.size()) break;
        if (pack) {
            rec[usaOff + 2 * i] = rec[pos];
            rec[usaOff + 2 * i + 1] = rec[pos + 1];
            put16(&rec[pos], usn);
        } else {
            if (le16(&rec[pos]) != usn) return false;  // torn write
            rec[pos] = rec[usaOff + 2 * i];
            rec[pos + 1] = rec[usaOff + 2 * i + 1];
        }
    }
    return true;
}

bool ntfsSetLabel(BlockDevice& d, const std::string& label, std::string& err) {
    Ntfs n;
    std::vector<std::uint8_t> boot;
    if (!ntfsParse(d, n, boot, err)) return false;
    std::vector<std::uint8_t> u16;
    const std::size_t units = utf16Units(label, 128, &u16);
    if (units == SIZE_MAX || units > 128) {
        err = "label too long for NTFS";
        return false;
    }
    const std::uint64_t at = n.mftLcn * n.cluster + 3 * n.recSize, mirr = n.mirrLcn * n.cluster + 3 * n.recSize;
    std::vector<std::uint8_t> rec(static_cast<std::size_t>(n.recSize));
    if (!d.read(at, rec.data(), rec.size()) || std::memcmp(rec.data(), "FILE", 4) != 0 || !ntfsFixups(rec, false)) {
        err = "MFT record 3 ($Volume) cannot be read";
        return false;
    }
    {  // $MFTMirr must hold an intact copy; both get the new record
        std::vector<std::uint8_t> m(rec.size());
        if (!d.read(mirr, m.data(), m.size()) || std::memcmp(m.data(), "FILE", 4) != 0 || !ntfsFixups(m, false)) {
            err = "$MFTMirr record 3 is damaged; run chkdsk first";
            return false;
        }
    }
    const std::uint32_t inUse = le32(&rec[0x18]), allocated = le32(&rec[0x1C]);
    if (inUse > rec.size() || allocated > rec.size()) {
        err = "corrupt $Volume record";
        return false;
    }
    for (std::size_t a = le16(&rec[0x14]); a + 24 <= inUse;) {
        const std::uint32_t type = le32(&rec[a]), len = le32(&rec[a + 4]);
        if (type == 0xFFFFFFFFu || len < 24 || a + len > inUse) break;
        if (type == 0x60) {
            if (rec[a + 8] != 0) {
                err = "$VOLUME_NAME is not resident";
                return false;
            }
            const std::size_t voff = le16(&rec[a + 0x14]);
            const std::size_t newLen = (voff + 2 * units + 7) / 8 * 8;
            const std::int64_t delta = static_cast<std::int64_t>(newLen) - static_cast<std::int64_t>(len);
            if (static_cast<std::int64_t>(inUse) + delta > static_cast<std::int64_t>(allocated)) {
                err = "the label does not fit into the $Volume record";
                return false;
            }
            std::memmove(&rec[a + newLen], &rec[a + len], inUse - (a + len));  // shift following attributes
            if (delta < 0) std::memset(&rec[static_cast<std::size_t>(inUse + delta)], 0, static_cast<std::size_t>(-delta));
            std::memset(&rec[a + voff], 0, newLen - voff);
            std::memcpy(&rec[a + voff], u16.data(), 2 * units);
            put32(&rec[a + 4], static_cast<std::uint32_t>(newLen));
            put32(&rec[a + 0x10], static_cast<std::uint32_t>(2 * units));
            put32(&rec[0x18], static_cast<std::uint32_t>(inUse + delta));
            if (!ntfsFixups(rec, true) || !d.write(at, rec.data(), rec.size()) || !d.write(mirr, rec.data(), rec.size())) {
                err = "writing the $Volume record failed";
                return false;
            }
            return d.flush() || (err = "flush failed", false);
        }
        a += len;
    }
    err = "$VOLUME_NAME attribute not found";
    return false;
}

// --- ext2/3/4 -------------------------------------------------------------------

constexpr std::uint32_t kExtCompatSparse2 = 0x200, kExtRoSparse = 0x1, kExtRoGdtCsum = 0x10, kExtRoMetaCsum = 0x400,
                        kExtIncompatRecover = 0x4, kExtIncompat64 = 0x80, kExtIncompatCsumSeed = 0x2000;

struct Ext {
    std::uint64_t blockSize = 0, blocks = 0, groups = 0;
    std::uint32_t bpg = 0, firstData = 0, compat = 0, incompat = 0, ro = 0;
};

bool extParse(BlockReader& r, Ext& e, std::vector<std::uint8_t>& sb, std::string& err) {
    sb.assign(1024, 0);
    if (!r.read(1024, sb.data(), 1024) || le16(&sb[0x38]) != 0xEF53) {
        err = "no ext2/3/4 superblock";
        return false;
    }
    const std::uint32_t logBs = le32(&sb[0x18]);
    if (logBs > 6) {
        err = "invalid ext block size";
        return false;
    }
    e.blockSize = 1024ull << logBs;
    e.bpg = le32(&sb[0x20]);
    e.firstData = le32(&sb[0x14]);
    e.compat = le32(&sb[0x5C]);
    e.incompat = le32(&sb[0x60]);
    e.ro = le32(&sb[0x64]);
    e.blocks = le32(&sb[0x04]) | ((e.incompat & kExtIncompat64) ? std::uint64_t{le32(&sb[0x150])} << 32 : 0);
    if (e.bpg == 0 || e.blocks <= e.firstData) {
        err = "invalid ext superblock";
        return false;
    }
    e.groups = (e.blocks - e.firstData + e.bpg - 1) / e.bpg;
    if (e.incompat & kExtIncompatRecover) {
        err = "the journal needs recovery; mount or check the file system first";
        return false;
    }
    if ((e.ro & kExtRoMetaCsum) && sb[0x175] != 1) {
        err = "unknown ext checksum type";
        return false;
    }
    return true;
}

bool hasBackup(const Ext& e, const std::vector<std::uint8_t>& sb, std::uint64_t g) {
    if (e.compat & kExtCompatSparse2) return g == le32(&sb[0x24C]) || g == le32(&sb[0x250]);
    if (!(e.ro & kExtRoSparse)) return true;
    if (g <= 1) return true;
    for (std::uint64_t base : {3u, 5u, 7u})
        for (std::uint64_t p = base; p <= g; p *= base)
            if (p == g) return true;
    return false;
}

// Applies `change` to the primary and every backup superblock and fixes checksums.
template <class F>
bool extUpdate(BlockDevice& d, const Ext& e, const std::vector<std::uint8_t>& primary, F change, std::string& err) {
    for (std::uint64_t g = 0; g < e.groups; ++g) {
        if (g && !hasBackup(e, primary, g)) continue;
        const std::uint64_t off = g == 0 ? 1024 : (e.firstData + g * e.bpg) * e.blockSize;
        if (off + 1024 > d.size()) break;
        std::vector<std::uint8_t> sb(1024);
        if (!d.read(off, sb.data(), sb.size())) {
            err = "superblock cannot be read";
            return false;
        }
        if (le16(&sb[0x38]) != 0xEF53 || (g && le16(&sb[0x5A]) != g)) continue;  // not a backup here
        change(sb);
        if (e.ro & kExtRoMetaCsum) put32(&sb[0x3FC], crc32c(0xFFFFFFFFu, sb.data(), 0x3FC));
        if (!d.write(off, sb.data(), sb.size())) {
            err = "writing a superblock failed";
            return false;
        }
    }
    return d.flush() || (err = "flush failed", false);
}

bool extSetLabel(BlockDevice& d, const std::string& label, std::string& err) {
    Ext e;
    std::vector<std::uint8_t> sb;
    if (!extParse(d, e, sb, err)) return false;
    return extUpdate(d, e, sb, [&](std::vector<std::uint8_t>& s) {
        std::memset(&s[0x78], 0, 16);
        std::memcpy(&s[0x78], label.data(), std::min<std::size_t>(label.size(), 16));
    }, err);
}

bool extSetUuid(BlockDevice& d, const Guid& rnd, std::string& err) {
    Ext e;
    std::vector<std::uint8_t> sb;
    if (!extParse(d, e, sb, err)) return false;
    if ((e.ro & kExtRoGdtCsum) && !(e.ro & kExtRoMetaCsum)) {
        err = "group descriptor checksums depend on the UUID (uninit_bg); not supported yet";
        return false;
    }
    // With metadata_csum all checksums are seeded from the UUID. Keeping the old
    // seed in s_checksum_seed (feature csum_seed) leaves them valid.
    const bool needSeed = (e.ro & kExtRoMetaCsum) && !(e.incompat & kExtIncompatCsumSeed);
    const std::uint32_t seed = crc32c(0xFFFFFFFFu, &sb[0x68], 16);
    return extUpdate(d, e, sb, [&](std::vector<std::uint8_t>& s) {
        if (needSeed) {
            put32(&s[0x60], le32(&s[0x60]) | kExtIncompatCsumSeed);
            put32(&s[0x270], seed);
        }
        std::memcpy(&s[0x68], rnd.bytes.data(), 16);  // ext stores the UUID in RFC 4122 byte order
    }, err);
}

// --- XFS (big-endian superblocks, one per allocation group) ---------------------------

std::uint32_t be32v(const std::uint8_t* p) { return std::uint32_t{p[0]} << 24 | std::uint32_t{p[1]} << 16 | std::uint32_t{p[2]} << 8 | p[3]; }
void putBe32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
}
constexpr std::uint32_t kXfsMetaUuid = 1u << 2;  // sb_features_incompat: XFS_SB_FEAT_INCOMPAT_META_UUID

template <class F>
bool xfsUpdate(BlockDevice& d, F change, std::string& err) {
    std::uint8_t sb0[512];
    if (!d.read(0, sb0, sizeof sb0) || std::memcmp(sb0, "XFSB", 4) != 0) {
        err = "no XFS superblock";
        return false;
    }
    const std::uint32_t bs = be32v(sb0 + 4), agblocks = be32v(sb0 + 84), agcount = be32v(sb0 + 88);
    const std::uint32_t sectsize = std::uint32_t{sb0[102]} << 8 | sb0[103];
    const bool v5 = ((std::uint32_t{sb0[100]} << 8 | sb0[101]) & 0x000F) == 5;
    if (bs < 512 || bs > 65536 || agblocks == 0 || agcount == 0 || sectsize < 512 || sectsize > 4096) {
        err = "invalid XFS superblock";
        return false;
    }
    for (std::uint32_t ag = 0; ag < agcount; ++ag) {
        const std::uint64_t off = std::uint64_t{ag} * agblocks * bs;
        if (off + sectsize > d.size()) break;
        std::vector<std::uint8_t> sb(sectsize);
        if (!d.read(off, sb.data(), sb.size()) || std::memcmp(sb.data(), "XFSB", 4) != 0) continue;
        change(sb, v5);
        if (v5) {  // sb_crc (little endian) = ~crc32c over the sector with the field zeroed
            put32(&sb[224], 0);
            put32(&sb[224], ~crc32c(0xFFFFFFFFu, sb.data(), sb.size()));
        }
        if (!d.write(off, sb.data(), sb.size())) {
            err = "writing an XFS superblock failed";
            return false;
        }
    }
    return d.flush() || (err = "flush failed", false);
}

bool xfsSetLabel(BlockDevice& d, const std::string& label, std::string& err) {
    return xfsUpdate(d, [&](std::vector<std::uint8_t>& sb, bool) {
        std::memset(&sb[108], 0, 12);
        std::memcpy(&sb[108], label.data(), std::min<std::size_t>(label.size(), 12));
    }, err);
}

bool xfsSetUuid(BlockDevice& d, const Guid& rfc, std::string& err) {
    return xfsUpdate(d, [&](std::vector<std::uint8_t>& sb, bool v5) {
        // v5 metadata embeds the UUID; keep it valid through sb_meta_uuid.
        if (v5 && !(be32v(&sb[216]) & kXfsMetaUuid)) {
            std::memcpy(&sb[248], &sb[32], 16);
            putBe32(&sb[216], be32v(&sb[216]) | kXfsMetaUuid);
        }
        std::memcpy(&sb[32], rfc.bytes.data(), 16);
    }, err);
}

// Growing XFS (v5 only, what xfs_growfs does): the last allocation group is extended and new
// groups get their headers and empty B-tree roots. Offline, so the log has to be clean.
std::uint16_t be16v(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }
std::uint64_t be64v(const std::uint8_t* p) { return std::uint64_t{be32v(p)} << 32 | be32v(p + 4); }
void putBe16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}
void putBe64(std::uint8_t* p, std::uint64_t v) {
    putBe32(p, static_cast<std::uint32_t>(v >> 32));
    putBe32(p + 4, static_cast<std::uint32_t>(v));
}

constexpr std::uint32_t kXfsNullAg = 0xFFFFFFFFu;
constexpr std::uint32_t kXfsRoFinobt = 1, kXfsRoRmap = 2, kXfsRoReflink = 4, kXfsRoInobtCount = 8;
constexpr std::uint32_t kXfsIncKnown = 0x01 /*ftype*/ | 0x02 /*sparse inodes*/ | kXfsMetaUuid | 0x08 /*bigtime*/ |
                                       0x20 /*nrext64*/ | 0x40 /*exchange*/ | 0x80 /*parent*/;
constexpr std::uint64_t kXfsMinAgBlocks = 64;  // XFS_MIN_AG_BLOCKS, also what xfs_growfs drops

struct XfsGeo {
    std::vector<std::uint8_t> sb;
    std::uint32_t bs = 0, agblocks = 0, agcount = 0, sect = 0, ro = 0;
    std::uint64_t dblocks = 0;
    std::uint8_t agblklog = 0;
    const std::uint8_t* uuid = nullptr;  // the UUID embedded in metadata

    std::uint64_t agOff(std::uint32_t ag) const { return std::uint64_t{ag} * agblocks * bs; }
    bool finobt() const { return (ro & kXfsRoFinobt) != 0; }
    bool rmap() const { return (ro & kXfsRoRmap) != 0; }
    bool reflink() const { return (ro & kXfsRoReflink) != 0; }
    std::uint32_t bnoBlock() const { return 3 * sect / bs + 1; }
    std::uint32_t ibtBlock() const { return bnoBlock() + 2; }
    std::uint32_t fibtBlock() const { return ibtBlock() + 1; }
    std::uint32_t rmapBlock() const { return ibtBlock() + (finobt() ? 2 : 1); }
    std::uint32_t refcBlock() const { return rmapBlock() + (rmap() ? 1 : 0); }
    std::uint32_t firstFree() const { return refcBlock() + (reflink() ? 1 : 0); }
};

bool xfsCrcOk(const std::vector<std::uint8_t>& block, std::size_t at) {
    std::vector<std::uint8_t> c(block);
    put32(&c[at], 0);
    return le32(&block[at]) == ~crc32c(0xFFFFFFFFu, c.data(), c.size());
}
void xfsSeal(std::uint8_t* block, std::size_t size, std::size_t at) {
    put32(block + at, 0);
    put32(block + at, ~crc32c(0xFFFFFFFFu, block, size));
}

std::string xfsLoad(BlockReader& r, XfsGeo& g) {
    std::uint8_t head[512];
    if (r.size() < sizeof head || !r.read(0, head, sizeof head) || std::memcmp(head, "XFSB", 4) != 0) return "no XFS superblock";
    if ((be16v(head + 100) & 0x000F) != 5) return "only XFS version 5 can be resized";
    g.bs = be32v(head + 4);
    g.dblocks = be64v(head + 8);
    g.agblocks = be32v(head + 84);
    g.agcount = be32v(head + 88);
    g.sect = be16v(head + 102);
    g.agblklog = head[124];
    if (g.bs < 512 || g.bs > 65536 || (g.bs & (g.bs - 1)) || g.sect < 512 || g.sect > 32768 || (g.sect & (g.sect - 1)) ||
        g.sect > g.bs || g.agblocks < kXfsMinAgBlocks || g.agblklog > 31 || (std::uint64_t{g.agblocks} >> g.agblklog) > 1 ||
        g.agcount == 0 || g.dblocks <= std::uint64_t{g.agcount - 1} * g.agblocks ||
        g.dblocks > std::uint64_t{g.agcount} * g.agblocks) {
        return "invalid XFS superblock";
    }
    g.sb.resize(g.sect);
    if (!r.read(0, g.sb.data(), g.sb.size())) return "the XFS superblock cannot be read";
    if (!xfsCrcOk(g.sb, 224)) return "the XFS superblock checksum is wrong";
    if (g.sb[126] != 0) return "the XFS file system was never completely created";
    g.ro = be32v(&g.sb[212]);
    const std::uint32_t inc = be32v(&g.sb[216]);
    if (g.ro & ~(kXfsRoFinobt | kXfsRoRmap | kXfsRoReflink | kXfsRoInobtCount)) return "this XFS file system uses features that cannot be resized yet";
    if (inc & ~kXfsIncKnown) return "this XFS file system needs repair or uses features that cannot be resized yet";
    if (be32v(&g.sb[208]) != 0 || be32v(&g.sb[220]) != 0) return "this XFS file system uses features that cannot be resized yet";
    if (be64v(&g.sb[16]) != 0 || be64v(&g.sb[24]) != 0) return "XFS with a realtime section cannot be resized";
    g.uuid = (inc & kXfsMetaUuid) ? &g.sb[248] : &g.sb[32];
    return {};
}

// The log is clean when its newest record is exactly one unmount record. Everything else,
// even a log that cannot be understood, is refused: only the kernel can replay it.
std::string xfsLogProblem(BlockReader& r, const XfsGeo& g) {
    static const char* const kDirty = "the XFS log is not clean; mount and unmount the file system on Linux first";
    const std::uint64_t logStart = be64v(&g.sb[48]);
    if (logStart == 0) return "XFS with an external log cannot be resized";
    const std::uint64_t agBlock = logStart & ((std::uint64_t{1} << g.agblklog) - 1);
    const std::uint64_t base = ((logStart >> g.agblklog) * g.agblocks + agBlock) * g.bs;
    const std::uint64_t logBB = std::uint64_t{be32v(&g.sb[96])} * g.bs / 512;
    if (logBB < 8 || base + logBB * 512 > r.size()) return "invalid XFS log";

    const auto cycleOf = [](const std::uint8_t* bb) { return be32v(bb) == 0xFEEDBABEu ? be32v(bb + 4) : be32v(bb); };
    std::vector<std::uint8_t> chunk(1024 * 1024);
    std::uint64_t head = 0;
    std::uint32_t first = 0;
    bool found = false;
    for (std::uint64_t i = 0; i < logBB && !found;) {
        const std::uint64_t n = std::min<std::uint64_t>(logBB - i, chunk.size() / 512);
        if (!r.read(base + i * 512, chunk.data(), static_cast<std::size_t>(n * 512))) return "the XFS log cannot be read";
        if (i == 0) first = cycleOf(chunk.data());
        for (std::uint64_t k = 0; k < n; ++k) {
            if (cycleOf(&chunk[static_cast<std::size_t>(k * 512)]) != first) {
                head = i + k;
                found = true;
                break;
            }
        }
        i += n;
    }
    std::uint8_t bb[512];
    const std::uint64_t limit = std::min<std::uint64_t>(logBB, 2048);  // larger than any record
    for (std::uint64_t back = 1; back <= limit; ++back) {
        const std::uint64_t blk = (head + logBB - back) % logBB;
        if (!r.read(base + blk * 512, bb, sizeof bb)) return "the XFS log cannot be read";
        if (be32v(bb) != 0xFEEDBABEu) continue;
        if (be32v(bb + 40) != 1) return kDirty;  // h_num_logops
        const std::uint32_t hSize = be32v(bb + 320);
        const std::uint64_t hblks = (be32v(bb + 8) & 2) ? std::max<std::uint64_t>(1, (std::uint64_t{hSize} + 32767) / 32768) : 1;
        if (!r.read(base + (blk + hblks) % logBB * 512, bb, sizeof bb)) return "the XFS log cannot be read";
        return (bb[9] & 0x20) ? std::string() : kDirty;  // oh_flags: XLOG_UNMOUNT_TRANS
    }
    return kDirty;
}

// The free-space trees of the last group, when that group has to get longer.
struct XfsTail {
    std::vector<std::uint8_t> agf, agi, bno, cnt;
    std::uint32_t agfLen = 0;
};

std::string xfsLoadTail(BlockReader& r, const XfsGeo& g, XfsTail& t) {
    const std::uint32_t ag = g.agcount - 1;
    const std::uint64_t off = g.agOff(ag);
    t.agf.resize(g.sect);
    t.agi.resize(g.sect);
    t.bno.resize(g.bs);
    t.cnt.resize(g.bs);
    if (!r.read(off + g.sect, t.agf.data(), g.sect) || !r.read(off + 2ull * g.sect, t.agi.data(), g.sect)) return "the last XFS group cannot be read";
    if (std::memcmp(t.agf.data(), "XAGF", 4) != 0 || !xfsCrcOk(t.agf, 216) || be32v(&t.agf[8]) != ag) return "the last XFS group is damaged";
    if (std::memcmp(t.agi.data(), "XAGI", 4) != 0 || !xfsCrcOk(t.agi, 312) || be32v(&t.agi[8]) != ag) return "the last XFS group is damaged";
    t.agfLen = be32v(&t.agf[12]);
    if (t.agfLen != g.dblocks - std::uint64_t{ag} * g.agblocks || be32v(&t.agi[12]) != t.agfLen) return "the last XFS group is damaged";
    if (be32v(&t.agf[28]) != 1 || be32v(&t.agf[32]) != 1) return "the free-space trees of the last XFS group are too large to extend yet";
    const std::uint32_t bnoRoot = be32v(&t.agf[16]), cntRoot = be32v(&t.agf[20]);
    if (bnoRoot >= t.agfLen || cntRoot >= t.agfLen) return "the last XFS group is damaged";
    if (!r.read(off + std::uint64_t{bnoRoot} * g.bs, t.bno.data(), g.bs) || !r.read(off + std::uint64_t{cntRoot} * g.bs, t.cnt.data(), g.bs)) return "the last XFS group cannot be read";
    if (std::memcmp(t.bno.data(), "AB3B", 4) != 0 || !xfsCrcOk(t.bno, 52) || std::memcmp(t.cnt.data(), "AB3C", 4) != 0 || !xfsCrcOk(t.cnt, 52)) return "the last XFS group is damaged";
    const std::uint32_t nb = be16v(&t.bno[6]), nc = be16v(&t.cnt[6]), maxRecs = (g.bs - 56) / 8;
    if (nb != nc || nb > maxRecs) return "the last XFS group is damaged";
    if (nb == maxRecs) return "the free-space trees of the last XFS group are too large to extend yet";
    return {};
}

// New size in blocks; like xfs_growfs it leaves out a tail too small to be a group.
std::uint64_t xfsTargetBlocks(const XfsGeo& g, std::uint64_t newBytes) {
    std::uint64_t blocks = newBytes / g.bs;
    const std::uint64_t mod = blocks % g.agblocks;
    if (blocks > std::uint64_t{g.agcount} * g.agblocks && mod != 0 && mod < kXfsMinAgBlocks) blocks -= mod;
    return blocks;
}

std::string xfsGrowProblem(BlockReader& r, std::uint64_t newBytes) {
    XfsGeo g;
    if (std::string p = xfsLoad(r, g); !p.empty()) return p;
    const std::uint64_t target = xfsTargetBlocks(g, newBytes);
    if (target < g.dblocks) return "shrinking XFS is not supported";
    if ((target + g.agblocks - 1) / g.agblocks > 0x7FFFFFFFull) return "the XFS file system would get too many groups";
    if (std::string p = xfsLogProblem(r, g); !p.empty()) return p;
    if (target > g.dblocks && g.dblocks < std::uint64_t{g.agcount} * g.agblocks) {
        XfsTail t;
        if (std::string p = xfsLoadTail(r, g, t); !p.empty()) return p;
    }
    return {};
}

void xfsBtreeHeader(std::uint8_t* b, const XfsGeo& g, const char* magic, std::uint32_t ag, std::uint32_t block, std::uint16_t recs) {
    std::memcpy(b, magic, 4);
    putBe16(b + 6, recs);
    putBe32(b + 8, kXfsNullAg);
    putBe32(b + 12, kXfsNullAg);
    putBe64(b + 16, (std::uint64_t{ag} * g.agblocks + block) * (g.bs / 512));  // bb_blkno
    std::memcpy(b + 32, g.uuid, 16);
    putBe32(b + 48, ag);
}

// All headers of a new group: superblock copy, AGF, AGI, AGFL and the B-tree roots.
std::vector<std::uint8_t> xfsNewGroup(const XfsGeo& g, const std::vector<std::uint8_t>& sb, std::uint32_t ag, std::uint32_t len) {
    const std::uint32_t freeStart = g.firstFree(), freeLen = len - freeStart, sect = g.sect, bs = g.bs;
    std::vector<std::uint8_t> out(std::size_t{freeStart} * bs);
    std::uint8_t* const p = out.data();

    std::memcpy(p, sb.data(), sect);
    xfsSeal(p, sect, 224);

    std::uint8_t* const agf = p + sect;
    std::memcpy(agf, "XAGF", 4);
    putBe32(agf + 4, 1);
    putBe32(agf + 8, ag);
    putBe32(agf + 12, len);
    putBe32(agf + 16, g.bnoBlock());
    putBe32(agf + 20, g.bnoBlock() + 1);
    putBe32(agf + 28, 1);
    putBe32(agf + 32, 1);
    if (g.rmap()) {
        putBe32(agf + 24, g.rmapBlock());
        putBe32(agf + 36, 1);
        putBe32(agf + 80, 1);
    }
    const std::uint32_t agflSize = (sect - 36) / 4;
    putBe32(agf + 44, agflSize - 1);  // agf_fllast; first 0, count 0
    putBe32(agf + 52, freeLen);
    putBe32(agf + 56, freeLen);
    std::memcpy(agf + 64, g.uuid, 16);
    if (g.reflink()) {
        putBe32(agf + 84, 1);
        putBe32(agf + 88, g.refcBlock());
        putBe32(agf + 92, 1);
    }
    xfsSeal(agf, sect, 216);

    std::uint8_t* const agi = p + 2 * sect;
    std::memcpy(agi, "XAGI", 4);
    putBe32(agi + 4, 1);
    putBe32(agi + 8, ag);
    putBe32(agi + 12, len);
    putBe32(agi + 20, g.ibtBlock());
    putBe32(agi + 24, 1);
    putBe32(agi + 32, kXfsNullAg);
    putBe32(agi + 36, kXfsNullAg);
    std::memset(agi + 40, 0xFF, 256);
    std::memcpy(agi + 296, g.uuid, 16);
    if (g.finobt()) {
        putBe32(agi + 328, g.fibtBlock());
        putBe32(agi + 332, 1);
    }
    if (g.ro & kXfsRoInobtCount) {
        putBe32(agi + 336, 1);
        if (g.finobt()) putBe32(agi + 340, 1);
    }
    xfsSeal(agi, sect, 312);

    std::uint8_t* const agfl = p + 3 * sect;
    std::memcpy(agfl, "XAFL", 4);
    putBe32(agfl + 4, ag);
    std::memcpy(agfl + 8, g.uuid, 16);
    std::memset(agfl + 36, 0xFF, std::size_t{agflSize} * 4);
    xfsSeal(agfl, sect, 32);

    const auto block = [&](std::uint32_t b) { return p + std::size_t{b} * bs; };
    const auto freeRoot = [&](std::uint32_t b, const char* magic) {
        std::uint8_t* q = block(b);
        xfsBtreeHeader(q, g, magic, ag, b, 1);
        putBe32(q + 56, freeStart);
        putBe32(q + 60, freeLen);
        xfsSeal(q, bs, 52);
    };
    freeRoot(g.bnoBlock(), "AB3B");
    freeRoot(g.bnoBlock() + 1, "AB3C");
    const auto emptyRoot = [&](std::uint32_t b, const char* magic) {
        xfsBtreeHeader(block(b), g, magic, ag, b, 0);
        xfsSeal(block(b), bs, 52);
    };
    emptyRoot(g.ibtBlock(), "IAB3");
    if (g.finobt()) emptyRoot(g.fibtBlock(), "FIB3");
    if (g.rmap()) {
        // Owners: XFS_RMAP_OWN_FS -3, OWN_AG -5, OWN_INOBT -6, OWN_REFC -8.
        struct Rec { std::uint32_t start, count; std::int64_t owner; };
        std::vector<Rec> recs = {{0, g.bnoBlock(), -3}, {g.bnoBlock(), 2, -5}, {g.ibtBlock(), g.rmapBlock() - g.ibtBlock(), -6}, {g.rmapBlock(), 1, -5}};
        if (g.reflink()) recs.push_back({g.refcBlock(), 1, -8});
        std::uint8_t* q = block(g.rmapBlock());
        xfsBtreeHeader(q, g, "RMB3", ag, g.rmapBlock(), static_cast<std::uint16_t>(recs.size()));
        std::size_t at = 56;
        for (const Rec& rec : recs) {
            putBe32(q + at, rec.start);
            putBe32(q + at + 4, rec.count);
            putBe64(q + at + 8, static_cast<std::uint64_t>(rec.owner));
            at += 24;  // the offset field stays zero
        }
        xfsSeal(q, bs, 52);
    }
    if (g.reflink()) emptyRoot(g.refcBlock(), "R3FC");
    return out;
}

// Adds `extra` free blocks to the end of the last group, in both free-space trees.
void xfsExtendTail(const XfsGeo& g, XfsTail& t, std::uint32_t extra) {
    struct Ext { std::uint32_t start, count; };
    const std::uint32_t n = be16v(&t.bno[6]);
    std::vector<Ext> ext;
    for (std::uint32_t i = 0; i < n; ++i) ext.push_back({be32v(&t.bno[56 + i * 8]), be32v(&t.bno[60 + i * 8])});
    if (!ext.empty() && ext.back().start + ext.back().count == t.agfLen) ext.back().count += extra;
    else ext.push_back({t.agfLen, extra});

    const auto store = [&](std::vector<std::uint8_t>& leaf, const std::vector<Ext>& recs) {
        putBe16(&leaf[6], static_cast<std::uint16_t>(recs.size()));
        for (std::size_t i = 0; i < recs.size(); ++i) {
            putBe32(&leaf[56 + i * 8], recs[i].start);
            putBe32(&leaf[60 + i * 8], recs[i].count);
        }
        xfsSeal(leaf.data(), g.bs, 52);
    };
    store(t.bno, ext);
    std::sort(ext.begin(), ext.end(), [](const Ext& a, const Ext& b) { return a.count != b.count ? a.count < b.count : a.start < b.start; });
    store(t.cnt, ext);

    const std::uint32_t newLen = t.agfLen + extra;
    putBe32(&t.agf[12], newLen);
    putBe32(&t.agf[52], be32v(&t.agf[52]) + extra);
    putBe32(&t.agf[56], ext.back().count);
    xfsSeal(t.agf.data(), g.sect, 216);
    putBe32(&t.agi[12], newLen);
    xfsSeal(t.agi.data(), g.sect, 312);
}

bool xfsGrow(BlockDevice& d, std::uint64_t newBytes, std::string& err) {
    if (std::string p = xfsGrowProblem(d, newBytes); !p.empty()) {
        err = p;
        return false;
    }
    XfsGeo g;
    if (std::string p = xfsLoad(d, g); !p.empty()) {
        err = p;
        return false;
    }
    const std::uint64_t target = xfsTargetBlocks(g, newBytes);
    if (target == g.dblocks) return true;
    if (target * g.bs > d.size()) {
        err = "the XFS file system does not fit into the partition";
        return false;
    }
    const std::uint32_t oldCount = g.agcount;
    const std::uint32_t newCount = static_cast<std::uint32_t>((target + g.agblocks - 1) / g.agblocks);
    const std::uint64_t oldLast = g.dblocks - std::uint64_t{oldCount - 1} * g.agblocks;
    const std::uint64_t newLast = newCount == oldCount ? target - std::uint64_t{oldCount - 1} * g.agblocks : g.agblocks;
    const std::uint64_t added = target - g.dblocks;

    XfsTail tail;
    if (newLast > oldLast) {
        if (std::string p = xfsLoadTail(d, g, tail); !p.empty()) {
            err = p;
            return false;
        }
    }

    // Free space: everything added, minus the headers of the new groups.
    std::uint64_t headers = 0;
    for (std::uint32_t ag = oldCount; ag < newCount; ++ag) headers += g.firstFree();
    std::vector<std::uint8_t> sb(g.sb);
    putBe64(&sb[8], target);
    putBe32(&sb[88], newCount);
    putBe64(&sb[144], be64v(&sb[144]) + added - headers);

    // Order: new groups first, the primary superblock last. Until then the file system is
    // still the old one and only unused space has changed.
    for (std::uint32_t ag = oldCount; ag < newCount; ++ag) {
        const std::uint64_t len = ag + 1 == newCount ? target - std::uint64_t{ag} * g.agblocks : g.agblocks;
        const std::vector<std::uint8_t> group = xfsNewGroup(g, sb, ag, static_cast<std::uint32_t>(len));
        if (!d.write(g.agOff(ag), group.data(), group.size())) {
            err = "writing a new XFS group failed";
            return false;
        }
    }
    if (!d.flush()) {
        err = "flush failed";
        return false;
    }
    if (newLast > oldLast) {
        const std::uint32_t bnoRoot = be32v(&tail.agf[16]), cntRoot = be32v(&tail.agf[20]);
        xfsExtendTail(g, tail, static_cast<std::uint32_t>(newLast - oldLast));
        const std::uint64_t off = g.agOff(oldCount - 1);
        if (!d.write(off + std::uint64_t{bnoRoot} * g.bs, tail.bno.data(), g.bs) ||
            !d.write(off + std::uint64_t{cntRoot} * g.bs, tail.cnt.data(), g.bs) ||
            !d.write(off + g.sect, tail.agf.data(), g.sect) || !d.write(off + 2ull * g.sect, tail.agi.data(), g.sect) || !d.flush()) {
            err = "extending the last XFS group failed";
            return false;
        }
    }
    for (std::uint32_t ag = oldCount; ag-- > 1;) {
        std::vector<std::uint8_t> s(g.sect);
        if (!d.read(g.agOff(ag), s.data(), s.size()) || std::memcmp(s.data(), "XFSB", 4) != 0) {
            err = "an XFS superblock copy is damaged";
            return false;
        }
        putBe64(&s[8], target);
        putBe32(&s[88], newCount);
        xfsSeal(s.data(), s.size(), 224);
        if (!d.write(g.agOff(ag), s.data(), s.size())) {
            err = "writing an XFS superblock failed";
            return false;
        }
    }
    if (!d.flush()) {
        err = "flush failed";
        return false;
    }
    xfsSeal(sb.data(), sb.size(), 224);
    if (!d.write(0, sb.data(), sb.size()) || !d.flush()) {
        err = "writing the XFS superblock failed";
        return false;
    }
    return true;
}

// --- btrfs (superblock at 64 KiB, mirrors at 64 MiB and 256 GiB) ---------------------

constexpr std::uint64_t kBtrfsMetadataUuid = 1ull << 10;  // incompat METADATA_UUID

template <class F>
bool btrfsUpdate(BlockDevice& d, F change, std::string& err) {
    bool any = false;
    for (std::uint64_t off : {0x10000ull, 0x4000000ull, 0x4000000000ull}) {
        if (off + 4096 > d.size()) break;
        std::vector<std::uint8_t> sb(4096);
        if (!d.read(off, sb.data(), sb.size()) || std::memcmp(&sb[0x40], "_BHRfS_M", 8) != 0 || le64(&sb[0x30]) != off) continue;
        if (le16(&sb[0xC4]) != 0) {
            err = "only crc32c checksums are supported for btrfs";
            return false;
        }
        change(sb);
        std::memset(sb.data(), 0, 32);
        put32(sb.data(), ~crc32c(0xFFFFFFFFu, &sb[0x20], sb.size() - 0x20));
        if (!d.write(off, sb.data(), sb.size())) {
            err = "writing a btrfs superblock failed";
            return false;
        }
        any = true;
    }
    if (!any) {
        err = "no btrfs superblock";
        return false;
    }
    return d.flush() || (err = "flush failed", false);
}

// Growing btrfs: the new space simply is not allocated yet, so only the device
// size changes, in the superblocks and in the DEV_ITEM of the chunk tree. The
// bootstrap chunk array in the superblock says where the chunk tree lies.
struct BtrfsGrower {
    BlockDevice& d;
    std::vector<std::uint8_t> sb;
    std::uint32_t nodeSize = 0;
    std::uint64_t devId = 0, newBytes = 0;

    bool physical(std::uint64_t logical, std::vector<std::uint64_t>& out) const {
        const std::uint32_t size = le32(&sb[0xA0]);
        for (std::size_t o = 0x32B; o + 17 + 48 <= 0x32B + std::size_t{size};) {
            const std::uint64_t start = le64(&sb[o + 9]);
            const std::uint8_t* c = &sb[o + 17];
            const std::uint64_t length = le64(c);
            const std::uint32_t stripes = le16(c + 44);
            if (o + 17 + 48 + std::size_t{stripes} * 32 > 0x32B + std::size_t{size}) return false;
            if (logical >= start && logical - start < length) {
                for (std::uint32_t s = 0; s < stripes; ++s) out.push_back(le64(c + 48 + s * 32 + 8) + (logical - start));
                return !out.empty();
            }
            o += 17 + 48 + std::size_t{stripes} * 32;
        }
        return false;
    }
    bool walk(std::uint64_t logical, int depth, std::string& err) {
        std::vector<std::uint64_t> where;
        if (depth > 8 || !physical(logical, where)) {
            err = "the btrfs chunk tree lies outside the bootstrap chunks";
            return false;
        }
        std::vector<std::uint8_t> n(nodeSize);
        if (!d.read(where[0], n.data(), n.size()) || le64(&n[0x30]) != logical) {
            err = "a btrfs chunk tree block cannot be read";
            return false;
        }
        const std::uint32_t items = le32(&n[0x60]);
        const std::uint8_t level = n[0x64];
        // The DEV_ITEM key: (1, 216, device id).
        auto before = [&](const std::uint8_t* k) {  // key <= target
            const std::uint64_t obj = le64(k), off = le64(k + 9);
            const std::uint8_t type = k[8];
            if (obj != 1) return obj < 1;
            if (type != 216) return type < 216;
            return off <= devId;
        };
        if (level) {
            std::uint64_t child = 0;
            for (std::uint32_t i = 0; i < items && 0x65 + (i + 1) * 33ull <= n.size(); ++i) {
                const std::uint8_t* e = &n[0x65 + i * 33];
                if (!before(e)) break;
                child = le64(e + 17);
            }
            if (!child) {
                err = "the btrfs device item was not found";
                return false;
            }
            return walk(child, depth + 1, err);
        }
        for (std::uint32_t i = 0; i < items && 0x65 + (i + 1) * 25ull <= n.size(); ++i) {
            const std::uint8_t* e = &n[0x65 + i * 25];
            if (le64(e) != 1 || e[8] != 216 || le64(e + 9) != devId) continue;
            const std::size_t at = 0x65 + std::size_t{le32(e + 17)};
            if (at + 16 > n.size() || le64(&n[at]) != devId) break;
            put64le(&n[at + 8], newBytes);
            std::memset(n.data(), 0, 32);
            put32(n.data(), ~crc32c(0xFFFFFFFFu, &n[0x20], n.size() - 0x20));
            for (std::uint64_t w : where)
                if (!d.write(w, n.data(), n.size())) {
                    err = "writing the btrfs chunk tree failed";
                    return false;
                }
            return true;
        }
        err = "the btrfs device item was not found";
        return false;
    }
};

std::string btrfsGrowProblem(BlockReader& r, std::uint64_t newBytes) {
    std::vector<std::uint8_t> sb(4096);
    if (r.size() < 0x10000 + 4096 || !r.read(0x10000, sb.data(), sb.size()) || std::memcmp(&sb[0x40], "_BHRfS_M", 8) != 0) return "no btrfs superblock";
    if (le16(&sb[0xC4]) != 0) return "only crc32c checksums are supported for btrfs";
    if (le64(&sb[0x88]) != 1) return "btrfs on several devices is not supported";
    if (le64(&sb[0x60]) != 0) return "the btrfs log must be replayed first; mount the file system once";
    if (newBytes < le64(&sb[0x70])) return "shrinking btrfs is not supported";
    return {};
}

bool btrfsGrow(BlockDevice& d, std::uint64_t newBytes, std::string& err) {
    if (const std::string p = btrfsGrowProblem(d, newBytes); !p.empty()) {
        err = p;
        return false;
    }
    BtrfsGrower g{d, std::vector<std::uint8_t>(4096), 0, 0, 0};
    if (!d.read(0x10000, g.sb.data(), g.sb.size())) {
        err = "no btrfs superblock";
        return false;
    }
    const std::uint32_t sector = le32(&g.sb[0x90]);
    g.nodeSize = le32(&g.sb[0x94]);
    if (!sector || g.nodeSize < 4096 || g.nodeSize > 65536) {
        err = "invalid btrfs superblock";
        return false;
    }
    g.devId = le64(&g.sb[0xC9]);
    g.newBytes = newBytes / sector * sector;
    if (g.newBytes == le64(&g.sb[0x70])) return true;
    if (!g.walk(le64(&g.sb[0x58]), 0, err) || !d.flush()) {
        if (err.empty()) err = "flush failed";
        return false;
    }
    const std::uint64_t target = g.newBytes;
    return btrfsUpdate(d, [target](std::vector<std::uint8_t>& sb) {
        put64le(&sb[0x70], target);
        put64le(&sb[0xC9 + 8], target);
    }, err);
}

bool btrfsSetLabel(BlockDevice& d, const std::string& label, std::string& err) {
    return btrfsUpdate(d, [&](std::vector<std::uint8_t>& sb) {
        std::memset(&sb[0x12B], 0, 256);
        std::memcpy(&sb[0x12B], label.data(), std::min<std::size_t>(label.size(), 255));
    }, err);
}

bool btrfsSetUuid(BlockDevice& d, const Guid& rfc, std::string& err) {
    return btrfsUpdate(d, [&](std::vector<std::uint8_t>& sb) {
        // Tree blocks carry the old fsid; with METADATA_UUID they keep it
        // (what btrfstune -m does). dev_item.fsid stays the metadata UUID.
        std::uint64_t incompat = le64(&sb[0xBC]);
        if (!(incompat & kBtrfsMetadataUuid)) {
            std::memcpy(&sb[0x23B], &sb[0x20], 16);
            incompat |= kBtrfsMetadataUuid;
            for (int i = 0; i < 8; ++i) sb[0xBC + i] = static_cast<std::uint8_t>(incompat >> (8 * i));
        }
        std::memcpy(&sb[0x20], rfc.bytes.data(), 16);
    }, err);
}

// --- Linux swap ------------------------------------------------------------------

bool swapCheck(BlockReader& r, std::string& err) {
    for (std::uint64_t page : {4096ull, 8192ull, 16384ull, 65536ull}) {
        char sig[10];
        if (page <= r.size() && r.read(page - 10, sig, 10) && std::memcmp(sig, "SWAPSPACE2", 10) == 0) return true;
    }
    err = "no Linux swap signature (version 1)";
    return false;
}

bool swapWrite(BlockDevice& d, std::uint64_t offset, const void* data, std::size_t len, std::string& err) {
    if (!swapCheck(d, err)) return false;
    if (!d.write(offset, data, len)) {
        err = "writing the swap header failed";
        return false;
    }
    return d.flush() || (err = "flush failed", false);
}

}  // namespace

// ---------------------------------------------------------------------------

std::uint32_t crc32c(std::uint32_t crc, const std::uint8_t* data, std::size_t len) noexcept {
    static std::uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    for (std::size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

static bool isExt(FsType t) noexcept { return t == FsType::Ext2 || t == FsType::Ext3 || t == FsType::Ext4; }
bool canCreate(FsType t) noexcept {
    return t == FsType::Fat16 || t == FsType::Fat32 || t == FsType::ExFat || t == FsType::Ntfs || isExt(t);
}
bool canLabel(FsType t) noexcept {
    return t == FsType::Fat12 || t == FsType::Fat16 || t == FsType::Fat32 || t == FsType::ExFat || t == FsType::Ntfs ||
           t == FsType::Ext2 || t == FsType::Ext3 || t == FsType::Ext4 || t == FsType::LinuxSwap || t == FsType::Xfs ||
           t == FsType::Btrfs;
}
bool canSetUuid(FsType t) noexcept { return canLabel(t); }
bool canCheck(FsType t) noexcept { return t == FsType::Fat16 || t == FsType::Fat32 || t == FsType::Ntfs || isExt(t); }
bool canResize(FsType t) noexcept {
    return t == FsType::Fat16 || t == FsType::Fat32 || t == FsType::Ntfs || t == FsType::ExFat || t == FsType::Btrfs ||
           t == FsType::Xfs || isExt(t);
}

std::string labelProblem(FsType t, const std::string& label) {
    if (hasControl(label)) return "the label contains control characters";
    switch (t) {
        case FsType::Fat12:
        case FsType::Fat16:
        case FsType::Fat32:
            if (label.size() > 11) return "FAT labels have at most 11 characters";
            for (unsigned char c : label)
                if (c >= 0x80 || std::strchr("\"*+,./:;<=>?[\\]|", c)) return "FAT labels allow letters, digits, spaces and - _ ! # $ % & ' ( ) @ ^ ` { } ~";
            return {};
        case FsType::ExFat: return utf16Units(label, 11) > 11 ? "exFAT labels have at most 11 characters" : std::string();
        case FsType::Ntfs: return utf16Units(label, 128) > 128 ? "NTFS labels have at most 128 characters" : std::string();
        case FsType::Ext2:
        case FsType::Ext3:
        case FsType::Ext4:
        case FsType::LinuxSwap: return label.size() > 16 ? "this label has at most 16 bytes" : std::string();
        case FsType::Xfs: return label.size() > 12 ? "XFS labels have at most 12 bytes" : std::string();
        case FsType::Btrfs: return label.size() > 255 ? "btrfs labels have at most 255 bytes" : std::string();
        default: return "labels cannot be changed for this file system yet";
    }
}

bool create(BlockDevice& part, FsType t, const std::string& label, std::uint64_t hidden, std::string& error) {
    if (!canCreate(t)) {
        error = std::string("creating ") + fsTypeName(t) + " is not supported";
        return false;
    }
    if (const std::string p = labelProblem(t, label); !p.empty()) {
        error = p;
        return false;
    }
    if (isExt(t)) return ext::create(part, t, label, error);
    if (t == FsType::Ntfs) return ntfs::create(part, label, hidden, error);
    return t == FsType::ExFat ? exfatCreate(part, label, hidden, error) : fat::create(part, t, label, hidden, error);
}

bool setLabel(BlockDevice& part, FsType t, const std::string& label, std::string& error) {
    if (const std::string p = labelProblem(t, label); !p.empty()) {
        error = p;
        return false;
    }
    switch (t) {
        case FsType::Fat12:
        case FsType::Fat16:
        case FsType::Fat32: return fat::setLabel(part, label, error);
        case FsType::ExFat: return exfatSetLabel(part, label, error);
        case FsType::Ntfs: return ntfsSetLabel(part, label, error);
        case FsType::Ext2:
        case FsType::Ext3:
        case FsType::Ext4: return extSetLabel(part, label, error);
        case FsType::LinuxSwap: {
            std::uint8_t b[16] = {};
            std::memcpy(b, label.data(), std::min<std::size_t>(label.size(), 16));
            return swapWrite(part, 1024 + 28, b, 16, error);
        }
        case FsType::Xfs: return xfsSetLabel(part, label, error);
        case FsType::Btrfs: return btrfsSetLabel(part, label, error);
        default: error = "labels cannot be changed for this file system yet"; return false;
    }
}

bool setNewUuid(BlockDevice& part, FsType t, std::string& uuid, std::string& error) {
    const Guid g = randomGuid();
    char buf[40];
    switch (t) {
        case FsType::Fat12:
        case FsType::Fat16:
        case FsType::Fat32:
        case FsType::ExFat: {
            const std::uint32_t serial = randomU32();
            std::snprintf(buf, sizeof buf, "%04X-%04X", serial >> 16, serial & 0xFFFF);
            uuid = buf;
            return t == FsType::ExFat ? exfatSetSerial(part, serial, error) : fat::setSerial(part, serial, error);
        }
        case FsType::Ntfs:
            std::snprintf(buf, sizeof buf, "%016llX", static_cast<unsigned long long>(le64(g.bytes.data())));
            uuid = buf;
            return ntfsSetSerial(part, g, error);
        case FsType::Ext2:
        case FsType::Ext3:
        case FsType::Ext4:
        case FsType::LinuxSwap:
        case FsType::Xfs:
        case FsType::Btrfs: {
            // ext, swap, XFS and btrfs store UUIDs in RFC 4122 byte order (randomGuid() is in
            // GPT's mixed-endian order): version 4 in byte 6, variant in byte 8.
            Guid rfc = g;
            rfc.bytes[6] = static_cast<std::uint8_t>((rfc.bytes[6] & 0x0F) | 0x40);
            rfc.bytes[8] = static_cast<std::uint8_t>((rfc.bytes[8] & 0x3F) | 0x80);
            const std::uint8_t* b = rfc.bytes.data();
            std::snprintf(buf, sizeof buf, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
                          b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
            uuid = buf;
            if (t == FsType::LinuxSwap) return swapWrite(part, 1024 + 12, b, 16, error);
            if (t == FsType::Xfs) return xfsSetUuid(part, rfc, error);
            if (t == FsType::Btrfs) return btrfsSetUuid(part, rfc, error);
            return extSetUuid(part, rfc, error);
        }
        default: error = "a new UUID cannot be set for this file system yet"; return false;
    }
}

bool resize(BlockDevice& part, FsType t, std::uint64_t newBytes, std::string& error) {
    if (!canResize(t)) {
        error = std::string("resizing ") + fsTypeName(t) + " is not supported";
        return false;
    }
    if (isExt(t)) return ext::resize(part, newBytes, error);
    if (t == FsType::Btrfs) return btrfsGrow(part, newBytes, error);
    if (t == FsType::Xfs) return xfsGrow(part, newBytes, error);
    if (t == FsType::ExFat) return exfatResize(part, newBytes, error);
    return t == FsType::Ntfs ? ntfs::resize(part, newBytes, error) : fat::resize(part, newBytes, error);
}

std::string resizeProblem(BlockReader& part, FsType t, std::uint64_t newBytes) {
    if (!canResize(t)) return std::string("resizing ") + fsTypeName(t) + " is not supported";
    if (t == FsType::Btrfs) return btrfsGrowProblem(part, newBytes);
    if (t == FsType::Xfs) return xfsGrowProblem(part, newBytes);
    if (newBytes < part.size()) return shrinkProblem(part, t, newBytes);
    if (t == FsType::ExFat) return exfatResizeProblem(part, newBytes);
    return isExt(t) ? ext::growProblem(part, newBytes) : std::string();
}

std::string shrinkProblem(BlockReader& part, FsType t, std::uint64_t newBytes) {
    if (t == FsType::Ntfs) return ntfs::shrinkProblem(part, newBytes);
    if (isExt(t)) return ext::shrinkProblem(part, newBytes);
    if (t == FsType::ExFat) return exfatResizeProblem(part, newBytes);
    if (t == FsType::Fat16 || t == FsType::Fat32) {
        CheckReport rep;
        std::string err;
        if (!fat::check(part, rep, err)) return err;
        if (!rep.ok) return "the file system has errors: " + rep.errors.front();
        if (rep.usedBytes + (newBytes / 16) > newBytes) return "the data does not fit into the new size";
        return {};
    }
    return std::string("shrinking ") + fsTypeName(t) + " is not supported";
}

bool check(BlockReader& part, FsType t, CheckReport& report, std::string& error) {
    if (!canCheck(t)) {
        error = std::string("checking ") + fsTypeName(t) + " is not supported";
        return false;
    }
    if (t == FsType::Ntfs) return ntfs::check(part, report, error);
    return isExt(t) ? ext::check(part, report, error) : fat::check(part, report, error);
}

std::uint64_t usedBytes(BlockReader& part, FsType t) {
    if (t == FsType::Fat16 || t == FsType::Fat32) return fat::usedBytes(part);
    if (t == FsType::Ntfs) return ntfs::usedBytes(part);
    if (isExt(t)) return ext::usedBytes(part);
    return 0;
}

}  // namespace wparted::fstools
