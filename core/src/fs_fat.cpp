// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// FAT16/FAT32 on image files: create, label, serial, read-only check, grow
// and shrink. Written from the Microsoft "FAT: General Overview of On-Disk
// Format" specification (fatgen103); verified in CI with dosfstools
// (fsck.fat) and mtools. FAT12 is detected elsewhere but not modified here.
#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <map>
#include <set>

#include "fs_internal.hpp"
#include "wparted/core/types.hpp"

namespace wparted::fstools::fat {
namespace {

constexpr std::uint32_t kFat16Min = 4085, kFat16Max = 65524, kFat32Min = 65525, kFat32Max = 0x0FFFFFF5;
constexpr std::uint64_t kMaxFatBytes = 256ull * 1024 * 1024;  // in-memory FAT limit

struct Geometry {
    std::uint32_t bps = 0, spc = 0, rsvd = 0, nfats = 0, rootEnt = 0, fatSz = 0, rootClus = 0, fsInfo = 0, bkBoot = 0;
    std::uint64_t totSec = 0;
    bool fat32 = false;

    std::uint32_t rootSecs() const { return (rootEnt * 32 + bps - 1) / bps; }
    std::uint64_t fatStart(std::uint32_t i) const { return rsvd + std::uint64_t{i} * fatSz; }
    std::uint64_t rootStart() const { return fatStart(nfats); }  // FAT16 fixed root directory
    std::uint64_t dataStart() const { return rootStart() + rootSecs(); }
    std::uint32_t clusters() const {
        return totSec > dataStart() ? static_cast<std::uint32_t>((totSec - dataStart()) / spc) : 0;
    }
    std::uint64_t clusterBytes() const { return std::uint64_t{spc} * bps; }
    std::uint64_t clusterOffset(std::uint32_t c) const { return (dataStart() + std::uint64_t{c - 2} * spc) * bps; }
    std::uint32_t entryBytes() const { return fat32 ? 4 : 2; }
    std::uint32_t capacity() const { return static_cast<std::uint32_t>(std::uint64_t{fatSz} * bps / entryBytes()); }
    std::uint32_t labelOffset() const { return fat32 ? 71 : 43; }
    std::uint32_t serialOffset() const { return fat32 ? 67 : 39; }
};

void put16(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

bool readBoot(BlockReader& r, std::vector<std::uint8_t>& bs, std::string& err) {
    bs.assign(std::max<std::uint32_t>(512, r.sectorSize()), 0);
    if (!r.read(0, bs.data(), bs.size())) {
        err = "boot sector cannot be read";
        return false;
    }
    if (bs[510] != 0x55 || bs[511] != 0xAA) {
        err = "no FAT boot sector signature";
        return false;
    }
    return true;
}

// Parses and checks the BPB. FAT12 is rejected unless `allowFat12`.
bool parse(BlockReader& r, Geometry& g, std::vector<std::uint8_t>& bs, std::string& err, bool allowFat12 = false) {
    if (!readBoot(r, bs, err)) return false;
    const std::uint8_t* b = bs.data();
    g.bps = le16(b + 11);
    g.spc = b[13];
    g.rsvd = le16(b + 14);
    g.nfats = b[16];
    g.rootEnt = le16(b + 17);
    const std::uint32_t tot16 = le16(b + 19), fat16 = le16(b + 22);
    g.totSec = tot16 ? tot16 : le32(b + 32);
    g.fatSz = fat16 ? fat16 : le32(b + 36);
    g.fat32 = fat16 == 0;
    if (g.fat32) {
        g.rootClus = le32(b + 44);
        g.fsInfo = le16(b + 48);
        g.bkBoot = le16(b + 50);
    }
    if ((g.bps != 512 && g.bps != 1024 && g.bps != 2048 && g.bps != 4096) || g.spc == 0 || (g.spc & (g.spc - 1)) ||
        g.rsvd == 0 || g.nfats == 0 || g.nfats > 2 || g.fatSz == 0 || g.totSec == 0) {
        err = "invalid FAT boot sector";
        return false;
    }
    if (g.bps != r.sectorSize()) {
        err = "FAT sector size differs from the device sector size";
        return false;
    }
    if (g.totSec * g.bps > r.size()) {
        err = "the file system is larger than its partition";
        return false;
    }
    const std::uint32_t n = g.clusters();
    if (n < kFat16Min && !allowFat12) {
        err = "FAT12 file systems are not supported for this operation";
        return false;
    }
    if (n >= kFat16Min && g.fat32 != (n >= kFat32Min)) {
        err = "FAT type does not match the cluster count";
        return false;
    }
    if (g.fat32 && (g.rootClus < 2 || g.rootClus >= n + 2)) {
        err = "invalid FAT32 root directory cluster";
        return false;
    }
    if (std::uint64_t{g.fatSz} * g.bps > kMaxFatBytes) {
        err = "the FAT is too large for this version";
        return false;
    }
    if (g.capacity() < n + 2) {
        err = "the FAT is smaller than the cluster count needs";
        return false;
    }
    return true;
}

// One FAT copy in memory; set() keeps the reserved high nibble of FAT32 entries.
struct Fat {
    std::vector<std::uint8_t> raw;
    bool fat32 = false;
    std::uint32_t get(std::uint32_t i) const {
        return fat32 ? le32(&raw[4 * std::size_t{i}]) & 0x0FFFFFFFu : le16(&raw[2 * std::size_t{i}]);
    }
    void set(std::uint32_t i, std::uint32_t v) {
        if (fat32) put32(&raw[4 * std::size_t{i}], (le32(&raw[4 * std::size_t{i}]) & 0xF0000000u) | (v & 0x0FFFFFFFu));
        else put16(&raw[2 * std::size_t{i}], v);
    }
    bool eoc(std::uint32_t v) const { return fat32 ? v >= 0x0FFFFFF8u : v >= 0xFFF8u; }
    bool bad(std::uint32_t v) const { return fat32 ? v == 0x0FFFFFF7u : v == 0xFFF7u; }
    std::uint32_t eocValue() const { return fat32 ? 0x0FFFFFFFu : 0xFFFFu; }
};

bool loadFat(BlockReader& r, const Geometry& g, std::uint32_t copy, Fat& f, std::string& err) {
    f.fat32 = g.fat32;
    f.raw.assign(static_cast<std::size_t>(std::uint64_t{g.fatSz} * g.bps), 0);
    if (!r.read(g.fatStart(copy) * g.bps, f.raw.data(), f.raw.size())) {
        err = "the FAT cannot be read";
        return false;
    }
    return true;
}

bool storeFat(BlockDevice& d, const Geometry& g, const Fat& f, std::string& err) {
    for (std::uint32_t i = 0; i < g.nfats; ++i)
        if (!d.write(g.fatStart(i) * g.bps, f.raw.data(), f.raw.size())) {
            err = "writing the FAT failed";
            return false;
        }
    return true;
}

bool isPtr(const Geometry& g, std::uint32_t v) { return v >= 2 && v < g.clusters() + 2; }

// A directory entry that references a cluster, with its location: inside a
// directory cluster (`dirCluster` != 0) or in the FAT16 root region.
struct Ref {
    std::uint32_t dirCluster = 0;
    std::uint32_t offset = 0;  // byte offset inside the cluster / root region
    std::uint32_t first = 0;
    std::uint32_t size = 0;
    bool isDir = false;
    bool dot = false;          // "." or ".."
};

std::uint32_t firstCluster(const Geometry& g, const std::uint8_t* e) {
    return (g.fat32 ? std::uint32_t{le16(e + 20)} << 16 : 0u) | le16(e + 26);
}
void setFirstCluster(const Geometry& g, std::uint8_t* e, std::uint32_t c) {
    if (g.fat32) put16(e + 20, c >> 16);
    put16(e + 26, c & 0xFFFF);
}

// Walks every directory (depth-first). `onDir` receives each directory's
// cluster chain; problems (loops, unreadable clusters) go to `problems`.
bool walk(BlockReader& r, const Geometry& g, const Fat& f, std::vector<Ref>& refs,
          std::vector<std::vector<std::uint32_t>>& dirChains, std::vector<std::string>& problems, std::string& err) {
    std::set<std::uint32_t> visited;
    const std::uint32_t cb = static_cast<std::uint32_t>(g.clusterBytes());
    std::function<bool(std::uint32_t, int)> dir = [&](std::uint32_t start, int depth) -> bool {
        // start == 0: FAT16 fixed root
        std::vector<std::uint32_t> chain;
        if (start) {
            for (std::uint32_t c = start; isPtr(g, c) && chain.size() <= g.clusters(); c = f.get(c)) {
                if (!visited.insert(c).second) {
                    problems.push_back("directory cluster " + std::to_string(c) + " is used twice (loop or cross-link)");
                    break;
                }
                chain.push_back(c);
                if (f.eoc(f.get(c)) || f.get(c) == 0) break;
            }
            dirChains.push_back(chain);
        }
        std::vector<std::uint8_t> buf;
        auto entries = [&](std::uint32_t dirCluster, std::uint64_t at, std::uint32_t len) -> int {
            buf.assign(len, 0);
            if (!r.read(at, buf.data(), len)) {
                err = "directory data cannot be read";
                return -1;
            }
            for (std::uint32_t o = 0; o + 32 <= len; o += 32) {
                const std::uint8_t* e = &buf[o];
                if (e[0] == 0x00) return 1;  // end of directory
                if (e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x08)) continue;
                Ref ref;
                ref.dirCluster = dirCluster;
                ref.offset = o;
                ref.first = firstCluster(g, e);
                ref.size = le32(e + 28);
                ref.isDir = (e[11] & 0x10) != 0;
                ref.dot = e[0] == '.';
                refs.push_back(ref);
                if (ref.isDir && !ref.dot && ref.first && depth < 64) {
                    if (!isPtr(g, ref.first)) problems.push_back("directory entry points outside the data area");
                    else if (!visited.count(ref.first) && !dir(ref.first, depth + 1)) return -1;
                }
            }
            return 0;
        };
        if (!start) return entries(0, g.rootStart() * g.bps, g.rootSecs() * g.bps) >= 0;
        for (std::uint32_t c : chain) {
            const int rc = entries(c, g.clusterOffset(c), cb);
            if (rc < 0) return false;
            if (rc > 0) break;
        }
        return true;
    };
    return dir(g.fat32 ? g.rootClus : 0, 0);
}

std::uint32_t countFree(const Geometry& g, const Fat& f) {
    std::uint32_t n = 0;
    for (std::uint32_t c = 2; c < g.clusters() + 2; ++c)
        if (f.get(c) == 0) ++n;
    return n;
}

bool writeFsInfo(BlockDevice& d, const Geometry& g, std::uint32_t freeClusters, std::string& err) {
    if (!g.fat32 || g.fsInfo == 0 || g.fsInfo == 0xFFFF) return true;
    for (std::uint32_t s : {g.fsInfo, g.bkBoot && g.bkBoot != 0xFFFF ? g.bkBoot + g.fsInfo : 0u}) {
        if (!s) continue;
        std::vector<std::uint8_t> sec(g.bps, 0);
        if (!d.read(std::uint64_t{s} * g.bps, sec.data(), sec.size())) {
            err = "FSInfo sector cannot be read";
            return false;
        }
        if (le32(&sec[0]) != 0x41615252u || le32(&sec[484]) != 0x61417272u) continue;  // not an FSInfo sector
        put32(&sec[488], freeClusters);
        put32(&sec[492], 0xFFFFFFFFu);  // "next free" unknown: let the driver search
        if (!d.write(std::uint64_t{s} * g.bps, sec.data(), sec.size())) {
            err = "writing the FSInfo sector failed";
            return false;
        }
    }
    return true;
}

// Writes the boot sector and, for FAT32, its backup copy.
bool writeBoot(BlockDevice& d, const Geometry& g, const std::vector<std::uint8_t>& bs, std::string& err) {
    if (!d.write(0, bs.data(), bs.size())) {
        err = "writing the boot sector failed";
        return false;
    }
    if (g.fat32 && g.bkBoot && g.bkBoot != 0xFFFF && g.bkBoot < g.rsvd &&
        !d.write(std::uint64_t{g.bkBoot} * g.bps, bs.data(), bs.size())) {
        err = "writing the backup boot sector failed";
        return false;
    }
    return true;
}

void setTotal(std::vector<std::uint8_t>& bs, const Geometry& g) {
    if (!g.fat32 && g.totSec < 65536) {
        put16(&bs[19], static_cast<std::uint32_t>(g.totSec));
        put32(&bs[32], 0);
    } else {
        put16(&bs[19], 0);
        put32(&bs[32], static_cast<std::uint32_t>(g.totSec));
    }
    if (g.fat32) put32(&bs[36], g.fatSz);
    else put16(&bs[22], g.fatSz);
}

// Overlap-safe move of `bytes` from `from` to `to` inside the device.
bool moveBytes(BlockDevice& d, std::uint64_t from, std::uint64_t to, std::uint64_t bytes, std::string& err) {
    if (from == to || !bytes) return true;
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(std::min<std::uint64_t>(bytes, 4 * 1024 * 1024)));
    const bool backwards = to > from;
    for (std::uint64_t done = 0; done < bytes;) {
        const std::uint64_t n = std::min<std::uint64_t>(buf.size(), bytes - done);
        const std::uint64_t off = backwards ? bytes - done - n : done;
        if (!d.read(from + off, buf.data(), static_cast<std::size_t>(n)) ||
            !d.write(to + off, buf.data(), static_cast<std::size_t>(n))) {
            err = "moving file system data failed";
            return false;
        }
        done += n;
    }
    return true;
}

std::string fatName(const std::string& label) {
    std::string n = label.empty() ? std::string("NO NAME") : label;
    for (char& c : n) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    n.resize(11, ' ');
    return n;
}

}  // namespace

// ---------------------------------------------------------------------------

bool create(BlockDevice& d, FsType t, const std::string& label, std::uint64_t hidden, std::string& err) {
    const std::uint32_t bps = d.sectorSize();
    if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) {
        err = "unsupported sector size";
        return false;
    }
    Geometry g;
    g.bps = bps;
    g.fat32 = t == FsType::Fat32;
    g.totSec = d.size() / bps;
    if (!g.fat32 && g.totSec > 0xFFFFFFFFull) g.totSec = 0xFFFFFFFFull;
    if (g.fat32 && g.totSec > 0xFFFFFFFFull) {
        err = "the partition is too large for FAT32";
        return false;
    }
    g.nfats = 2;
    g.rsvd = g.fat32 ? 32 : 1;
    g.rootEnt = g.fat32 ? 0 : 512;
    const std::uint64_t bytes = g.totSec * bps;
    // Cluster size: Microsoft's default tables (fatgen103); adjusted below if the
    // cluster count falls outside the range of the FAT type.
    std::uint64_t want;
    if (g.fat32)
        want = bytes <= 260ull << 20 ? 512 : bytes <= 8ull << 30 ? 4096 : bytes <= 16ull << 30 ? 8192 : bytes <= 32ull << 30 ? 16384 : 32768;
    else
        want = bytes <= 16ull << 20 ? 1024 : bytes <= 128ull << 20 ? 2048 : bytes <= 256ull << 20 ? 4096 : bytes <= 512ull << 20 ? 8192 : bytes <= 1ull << 30 ? 16384 : 32768;
    bool done = false;
    int steps = 0;
    for (std::uint64_t cb = std::max<std::uint64_t>(want, bps); cb >= bps && cb <= 65536 && !done && steps++ < 16;) {
        g.spc = static_cast<std::uint32_t>(cb / bps);
        if (g.spc > 128) break;
        g.fatSz = 1;
        for (int i = 0; i < 64; ++i) {  // FAT size and cluster count depend on each other
            const std::uint64_t meta = g.rsvd + std::uint64_t{g.nfats} * g.fatSz + g.rootSecs();
            if (meta >= g.totSec) break;
            const std::uint64_t n = (g.totSec - meta) / g.spc;
            const std::uint64_t need = ((n + 2) * g.entryBytes() + bps - 1) / bps;
            if (need <= g.fatSz) break;
            g.fatSz = static_cast<std::uint32_t>(need);
        }
        const std::uint32_t n = g.clusters();
        if (g.fat32) {
            if (n >= kFat32Min && n <= kFat32Max) done = true;
            else if (n < kFat32Min && cb > bps) cb /= 2;  // smaller clusters give more of them
            else break;
        } else {
            if (n >= kFat16Min && n <= kFat16Max) done = true;
            else if (n > kFat16Max) cb *= 2;
            else if (cb > bps) cb /= 2;  // small partitions: smaller clusters
            else break;
        }
    }
    if (!done) {
        err = g.fat32 ? "the partition is too small for FAT32 (about 33 MiB with 512-byte sectors) or too large"
                      : "the partition size does not fit FAT16 (about 2 MiB to 4 GiB)";
        return false;
    }
    if (g.fat32) {
        g.rootClus = 2;
        g.fsInfo = 1;
        g.bkBoot = 6;
    }
    // Zero reserved area, FATs, FAT16 root directory and the FAT32 root cluster.
    const std::uint64_t zeroSectors = g.dataStart() + (g.fat32 ? g.spc : 0);
    std::vector<std::uint8_t> zero(static_cast<std::size_t>(std::min<std::uint64_t>(zeroSectors * bps, 1u << 20)), 0);
    for (std::uint64_t off = 0; off < zeroSectors * bps; off += zero.size()) {
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(zero.size(), zeroSectors * bps - off));
        if (!d.write(off, zero.data(), n)) {
            err = "clearing the file system area failed";
            return false;
        }
    }
    std::vector<std::uint8_t> bs(bps, 0);
    bs[0] = 0xEB;
    bs[1] = g.fat32 ? 0x58 : 0x3C;
    bs[2] = 0x90;
    std::memcpy(&bs[3], "MSWIN4.1", 8);
    put16(&bs[11], bps);
    bs[13] = static_cast<std::uint8_t>(g.spc);
    put16(&bs[14], g.rsvd);
    bs[16] = static_cast<std::uint8_t>(g.nfats);
    put16(&bs[17], g.rootEnt);
    bs[21] = 0xF8;
    put16(&bs[24], 63);
    put16(&bs[26], 255);
    put32(&bs[28], static_cast<std::uint32_t>(std::min<std::uint64_t>(hidden, 0xFFFFFFFFull)));
    setTotal(bs, g);
    std::uint32_t serial = 0;
    const Guid rnd = randomGuid();
    std::memcpy(&serial, rnd.bytes.data(), 4);
    const std::string name = fatName(label);
    if (g.fat32) {
        put16(&bs[40], 0);
        put16(&bs[42], 0);
        put32(&bs[44], g.rootClus);
        put16(&bs[48], g.fsInfo);
        put16(&bs[50], g.bkBoot);
        bs[64] = 0x80;
        bs[66] = 0x29;
        put32(&bs[67], serial);
        std::memcpy(&bs[71], name.data(), 11);
        std::memcpy(&bs[82], "FAT32   ", 8);
    } else {
        bs[36] = 0x80;
        bs[38] = 0x29;
        put32(&bs[39], serial);
        std::memcpy(&bs[43], name.data(), 11);
        std::memcpy(&bs[54], "FAT16   ", 8);
    }
    bs[510] = 0x55;
    bs[511] = 0xAA;
    if (!writeBoot(d, g, bs, err)) return false;
    if (g.fat32) {  // FSInfo (+ backup)
        std::vector<std::uint8_t> fi(bps, 0);
        put32(&fi[0], 0x41615252u);
        put32(&fi[484], 0x61417272u);
        put32(&fi[488], g.clusters() - 1);
        put32(&fi[492], 3);
        put32(&fi[508], 0xAA550000u);
        if (!d.write(std::uint64_t{g.fsInfo} * bps, fi.data(), bps) ||
            !d.write(std::uint64_t{g.bkBoot + g.fsInfo} * bps, fi.data(), bps)) {
            err = "writing the FSInfo sector failed";
            return false;
        }
    }
    Fat f;
    f.fat32 = g.fat32;
    f.raw.assign(static_cast<std::size_t>(std::uint64_t{g.fatSz} * bps), 0);
    f.set(0, g.fat32 ? 0x0FFFFFF8u : 0xFFF8u);
    f.set(1, f.eocValue());
    if (g.fat32) f.set(2, f.eocValue());  // root directory cluster
    if (!storeFat(d, g, f, err)) return false;
    if (!label.empty()) {
        std::uint8_t e[32] = {};
        std::memcpy(e, name.data(), 11);
        e[11] = 0x08;
        const std::uint64_t at = g.fat32 ? g.clusterOffset(2) : g.rootStart() * bps;
        if (!d.write(at, e, 32)) {
            err = "writing the volume label entry failed";
            return false;
        }
    }
    return d.flush() || (err = "flush failed", false);
}

bool setLabel(BlockDevice& d, const std::string& label, std::string& err) {
    Geometry g;
    std::vector<std::uint8_t> bs;
    if (!parse(d, g, bs, err, true)) return false;
    const std::string name = fatName(label);
    std::memcpy(&bs[g.labelOffset()], name.data(), 11);
    if (!writeBoot(d, g, bs, err)) return false;
    // Volume label entry in the root directory (what Windows shows).
    std::vector<std::pair<std::uint64_t, std::uint32_t>> regions;  // (offset, length)
    if (g.fat32) {
        Fat f;
        if (!loadFat(d, g, 0, f, err)) return false;
        std::uint32_t steps = 0;
        for (std::uint32_t c = g.rootClus; isPtr(g, c) && steps++ <= g.clusters(); c = f.get(c)) {
            regions.push_back({g.clusterOffset(c), static_cast<std::uint32_t>(g.clusterBytes())});
            if (f.eoc(f.get(c))) break;
        }
    } else {
        regions.push_back({g.rootStart() * g.bps, g.rootSecs() * g.bps});
    }
    std::uint64_t freeAt = 0;
    bool haveFree = false;
    for (const auto& reg : regions) {
        std::vector<std::uint8_t> buf(reg.second);
        if (!d.read(reg.first, buf.data(), buf.size())) {
            err = "root directory cannot be read";
            return false;
        }
        for (std::uint32_t o = 0; o + 32 <= buf.size(); o += 32) {
            std::uint8_t* e = &buf[o];
            if (e[0] == 0x00 || e[0] == 0xE5) {
                if (!haveFree) {
                    haveFree = true;
                    freeAt = reg.first + o;
                }
                if (e[0] == 0x00) break;
                continue;
            }
            if (e[11] == 0x0F || !(e[11] & 0x08) || (e[11] & 0x10)) continue;
            if (label.empty()) e[0] = 0xE5;  // remove the label entry
            else std::memcpy(e, name.data(), 11);
            if (!d.write(reg.first + o, e, 32)) {
                err = "writing the volume label entry failed";
                return false;
            }
            return d.flush() || (err = "flush failed", false);
        }
    }
    if (!label.empty()) {
        if (!haveFree) {
            err = "the root directory has no free entry for the label";
            return false;
        }
        std::uint8_t e[32] = {};
        std::memcpy(e, name.data(), 11);
        e[11] = 0x08;
        if (!d.write(freeAt, e, 32)) {
            err = "writing the volume label entry failed";
            return false;
        }
    }
    return d.flush() || (err = "flush failed", false);
}

bool setSerial(BlockDevice& d, std::uint32_t serial, std::string& err) {
    Geometry g;
    std::vector<std::uint8_t> bs;
    if (!parse(d, g, bs, err, true)) return false;
    put32(&bs[g.serialOffset()], serial);
    return writeBoot(d, g, bs, err) && (d.flush() || (err = "flush failed", false));
}

bool check(BlockReader& r, CheckReport& rep, std::string& err) {
    Geometry g;
    std::vector<std::uint8_t> bs;
    if (!parse(r, g, bs, err)) return false;
    Fat f, f2;
    if (!loadFat(r, g, 0, f, err)) return false;
    const std::uint32_t n = g.clusters();
    if (g.nfats > 1 && loadFat(r, g, 1, f2, err)) {
        for (std::uint32_t c = 2; c < n + 2; ++c)
            if (f.get(c) != f2.get(c)) {
                rep.errors.push_back("the two FAT copies differ (first at cluster " + std::to_string(c) + ")");
                break;
            }
    }
    std::vector<Ref> refs;
    std::vector<std::vector<std::uint32_t>> dirChains;
    if (!walk(r, g, f, refs, dirChains, rep.errors, err)) return false;
    std::vector<std::uint8_t> owned(n + 2, 0);
    const std::uint64_t cb = g.clusterBytes();
    auto follow = [&](std::uint32_t first, std::uint64_t expectBytes, bool isDir) {
        std::uint64_t count = 0;
        for (std::uint32_t c = first;; ++count) {
            if (!isPtr(g, c)) {
                rep.errors.push_back("a cluster chain points outside the data area (" + std::to_string(c) + ")");
                return;
            }
            if (owned[c]) {
                rep.errors.push_back("cluster " + std::to_string(c) + " belongs to more than one file (cross-link)");
                return;
            }
            owned[c] = 1;
            const std::uint32_t v = f.get(c);
            if (v == 0) {
                rep.errors.push_back("a cluster chain runs into a free cluster (" + std::to_string(c) + ")");
                return;
            }
            if (f.bad(v)) {
                rep.errors.push_back("a cluster chain contains a bad cluster");
                return;
            }
            if (f.eoc(v)) {
                ++count;
                break;
            }
            c = v;
        }
        if (!isDir) {
            const std::uint64_t want = (expectBytes + cb - 1) / cb;
            if (count < want) rep.errors.push_back("a file is shorter than its recorded size");
            else if (count > want) rep.warnings.push_back("a file has more clusters than its size needs");
        }
    };
    if (g.fat32) follow(g.rootClus, 0, true);
    for (const auto& ref : refs)
        if (!ref.dot && ref.first) follow(ref.first, ref.size, ref.isDir);
    std::uint32_t lost = 0, used = 0;
    for (std::uint32_t c = 2; c < n + 2; ++c) {
        const std::uint32_t v = f.get(c);
        if (v != 0 && !f.bad(v)) {
            ++used;
            if (!owned[c]) ++lost;
        }
    }
    if (lost) rep.warnings.push_back(std::to_string(lost) + " lost cluster(s) (allocated but not used by any file)");
    if (g.fat32 && g.fsInfo && g.fsInfo != 0xFFFF) {
        std::vector<std::uint8_t> fi(g.bps);
        if (r.read(std::uint64_t{g.fsInfo} * g.bps, fi.data(), fi.size()) && le32(&fi[0]) == 0x41615252u) {
            const std::uint32_t recorded = le32(&fi[488]), actual = countFree(g, f);
            if (recorded != 0xFFFFFFFFu && recorded != actual)
                rep.warnings.push_back("FSInfo free cluster count is out of date (" + std::to_string(recorded) + " vs " +
                                       std::to_string(actual) + ")");
        }
    }
    rep.usedBytes = std::uint64_t{used} * cb;
    rep.totalBytes = std::uint64_t{n} * cb;
    rep.ok = rep.errors.empty();
    return true;
}

std::uint64_t usedBytes(BlockReader& r) {
    Geometry g;
    std::vector<std::uint8_t> bs;
    std::string err;
    Fat f;
    if (!parse(r, g, bs, err) || !loadFat(r, g, 0, f, err)) return 0;
    const std::uint32_t freeCount = countFree(g, f);
    return std::uint64_t{g.clusters() - freeCount} * g.clusterBytes();
}

bool resize(BlockDevice& d, std::uint64_t newBytes, std::string& err) {
    Geometry g;
    std::vector<std::uint8_t> bs;
    if (!parse(d, g, bs, err)) return false;
    const std::uint64_t newTot = newBytes / g.bps;
    if (newTot == g.totSec) return true;
    if (newBytes > d.size()) {
        err = "the new size is larger than the partition";
        return false;
    }
    {
        CheckReport rep;  // never resize a damaged file system
        if (!check(d, rep, err)) return false;
        if (!rep.ok) {
            err = "the file system has errors and is not resized: " + rep.errors.front();
            return false;
        }
    }
    Fat f;
    if (!loadFat(d, g, 0, f, err)) return false;
    const std::uint32_t oldN = g.clusters();
    if (newTot < g.totSec) {
        // --- shrink: relocate clusters behind the new end, keep the FAT size -------
        Geometry ng = g;
        ng.totSec = newTot;
        const std::uint32_t newN = ng.clusters();
        if (g.fat32 ? newN < kFat32Min : newN < kFat16Min) {
            err = "the new size is too small for this FAT type";
            return false;
        }
        std::map<std::uint32_t, std::uint32_t> remap;
        std::uint32_t nextFree = 2;
        for (std::uint32_t c = newN + 2; c < oldN + 2; ++c) {
            const std::uint32_t v = f.get(c);
            if (v == 0 || f.bad(v)) continue;
            while (nextFree < newN + 2 && f.get(nextFree) != 0) ++nextFree;
            if (nextFree >= newN + 2) {
                err = "not enough free space for the data in front of the new end";
                return false;
            }
            remap[c] = nextFree++;
        }
        if (!remap.empty()) {
            std::vector<Ref> refs;
            std::vector<std::vector<std::uint32_t>> dirChains;
            std::vector<std::string> problems;
            if (!walk(d, g, f, refs, dirChains, problems, err)) return false;
            auto map = [&](std::uint32_t c) {
                auto it = remap.find(c);
                return it == remap.end() ? c : it->second;
            };
            // 1. copy cluster contents
            std::vector<std::uint8_t> buf(static_cast<std::size_t>(g.clusterBytes()));
            for (const auto& m : remap)
                if (!d.read(g.clusterOffset(m.first), buf.data(), buf.size()) ||
                    !d.write(g.clusterOffset(m.second), buf.data(), buf.size())) {
                    err = "moving cluster data failed";
                    return false;
                }
            // 2. new FAT: pointers through the map, moved entries at their new place
            Fat nf = f;
            for (std::uint32_t c = 2; c < oldN + 2; ++c) {
                std::uint32_t v = f.get(c);
                if (isPtr(g, v)) v = map(v);
                nf.set(map(c), v);
                if (remap.count(c)) nf.set(c, 0);
            }
            f = nf;
            // 3. directory entries (their own location may have moved too)
            for (const auto& ref : refs) {
                if (!ref.first || !remap.count(ref.first)) continue;
                const std::uint64_t at = ref.dirCluster ? g.clusterOffset(map(ref.dirCluster)) + ref.offset
                                                        : g.rootStart() * g.bps + ref.offset;
                std::uint8_t e[32];
                if (!d.read(at, e, 32)) {
                    err = "directory entry cannot be read";
                    return false;
                }
                setFirstCluster(g, e, map(ref.first));
                if (!d.write(at, e, 32)) {
                    err = "updating a directory entry failed";
                    return false;
                }
            }
            if (g.fat32 && remap.count(g.rootClus)) {
                g.rootClus = map(g.rootClus);
                put32(&bs[44], g.rootClus);
            }
        }
        for (std::uint32_t c = newN + 2; c < oldN + 2; ++c) f.set(c, 0);
        g.totSec = newTot;
        setTotal(bs, g);
        if (!storeFat(d, g, f, err) || !writeBoot(d, g, bs, err) || !writeFsInfo(d, g, countFree(g, f), err)) return false;
        return d.flush() || (err = "flush failed", false);
    }
    // --- grow: enlarge the FAT if needed (moves root directory and data) ---------
    Geometry ng = g;
    ng.totSec = newTot;
    for (int i = 0; i < 64; ++i) {
        const std::uint64_t need = (std::uint64_t{ng.clusters()} + 2) * ng.entryBytes();
        const std::uint32_t needSz = static_cast<std::uint32_t>((need + g.bps - 1) / g.bps);
        if (needSz <= ng.fatSz) break;
        ng.fatSz = needSz;
    }
    const std::uint32_t newN = ng.clusters();
    if (g.fat32 ? newN > kFat32Max : newN > kFat16Max) {
        err = g.fat32 ? "the new size has too many clusters for FAT32"
                      : "the new size has too many clusters for FAT16 with this cluster size";
        return false;
    }
    if (std::uint64_t{ng.fatSz} * g.bps > kMaxFatBytes) {
        err = "the FAT would be too large for this version";
        return false;
    }
    if (ng.fatSz != g.fatSz) {
        const std::uint64_t from = g.rootStart() * g.bps, to = ng.rootStart() * g.bps;
        const std::uint64_t bytes = (g.dataStart() - g.rootStart() + std::uint64_t{oldN} * g.spc) * g.bps;
        if (!moveBytes(d, from, to, bytes, err)) return false;
    }
    f.raw.resize(static_cast<std::size_t>(std::uint64_t{ng.fatSz} * g.bps), 0);
    for (std::uint32_t c = oldN + 2; c < ng.capacity(); ++c) f.set(c, 0);  // new clusters are free
    g = ng;
    setTotal(bs, g);
    if (!storeFat(d, g, f, err) || !writeBoot(d, g, bs, err) || !writeFsInfo(d, g, countFree(g, f), err)) return false;
    return d.flush() || (err = "flush failed", false);
}

}  // namespace wparted::fstools::fat
