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
            if (delta < 0) std::memset(&rec[inUse + delta], 0, static_cast<std::size_t>(-delta));
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

bool canCreate(FsType t) noexcept { return t == FsType::Fat16 || t == FsType::Fat32; }
bool canLabel(FsType t) noexcept {
    return t == FsType::Fat12 || t == FsType::Fat16 || t == FsType::Fat32 || t == FsType::ExFat || t == FsType::Ntfs ||
           t == FsType::Ext2 || t == FsType::Ext3 || t == FsType::Ext4 || t == FsType::LinuxSwap;
}
bool canSetUuid(FsType t) noexcept { return canLabel(t); }
bool canCheck(FsType t) noexcept { return t == FsType::Fat16 || t == FsType::Fat32; }
bool canResize(FsType t) noexcept { return t == FsType::Fat16 || t == FsType::Fat32 || t == FsType::Ntfs; }

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
    return fat::create(part, t, label, hidden, error);
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
        case FsType::LinuxSwap: {
            // ext and swap store UUIDs in RFC 4122 byte order (randomGuid() is in
            // GPT's mixed-endian order): version 4 in byte 6, variant in byte 8.
            Guid rfc = g;
            rfc.bytes[6] = static_cast<std::uint8_t>((rfc.bytes[6] & 0x0F) | 0x40);
            rfc.bytes[8] = static_cast<std::uint8_t>((rfc.bytes[8] & 0x3F) | 0x80);
            const std::uint8_t* b = rfc.bytes.data();
            std::snprintf(buf, sizeof buf, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
                          b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
            uuid = buf;
            if (t == FsType::LinuxSwap) return swapWrite(part, 1024 + 12, b, 16, error);
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
    return t == FsType::Ntfs ? ntfs::resize(part, newBytes, error) : fat::resize(part, newBytes, error);
}

std::string shrinkProblem(BlockReader& part, FsType t, std::uint64_t newBytes) {
    if (t == FsType::Ntfs) return ntfs::shrinkProblem(part, newBytes);
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
    return fat::check(part, report, error);
}

std::uint64_t usedBytes(BlockReader& part, FsType t) {
    if (t == FsType::Fat16 || t == FsType::Fat32) return fat::usedBytes(part);
    if (t == FsType::Ntfs) return ntfs::usedBytes(part);
    return 0;
}

}  // namespace wparted::fstools
