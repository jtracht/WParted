// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// NTFS on image files: used space and resizing without moving data. Written
// from the NTFS on-disk documentation of the Linux-NTFS project; verified in
// CI with ntfs-3g (ntfsresize --check, ntfsinfo).
//
// Grow: boot sector size, $Bitmap (extended, new clusters allocated in the
// added space if needed), $BadClus:$Bad (sparse over the volume) and the
// backup boot sector at the new end. Shrink: the same, but only if every
// cluster behind the new end is free; data is never relocated.
#include <algorithm>
#include <cstring>

#include "fs_internal.hpp"
#include "wparted/core/types.hpp"

namespace wparted::fstools::ntfs {
namespace {

void put16(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void put64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

struct Vol {
    std::uint32_t bps = 0;
    std::uint64_t cs = 0, recSize = 0, mftLcn = 0, totalSectors = 0, clusters = 0;
    std::uint32_t spc = 0;
};

bool parse(BlockReader& r, Vol& v, std::vector<std::uint8_t>& boot, std::string& err) {
    boot.assign(std::max<std::uint32_t>(512, r.sectorSize()), 0);
    if (!r.read(0, boot.data(), boot.size()) || std::memcmp(&boot[3], "NTFS    ", 8) != 0) {
        err = "no NTFS boot sector";
        return false;
    }
    v.bps = le16(&boot[11]);
    const std::uint8_t spc = boot[13];
    if (v.bps < 256 || v.bps > 4096 || (v.bps & (v.bps - 1)) || spc == 0 || spc > 0x80 || v.bps != r.sectorSize()) {
        err = "unsupported NTFS geometry for this device";
        return false;
    }
    v.spc = spc;
    v.cs = std::uint64_t{spc} * v.bps;
    const auto raw = static_cast<std::int8_t>(boot[0x40]);
    v.recSize = raw > 0 ? raw * v.cs : (raw > -31 ? 1ull << -raw : 0);
    v.mftLcn = le64(&boot[0x30]);
    v.totalSectors = le64(&boot[0x28]);
    v.clusters = v.totalSectors / v.spc;
    if (v.recSize < 512 || v.recSize > 65536 || v.mftLcn >= v.clusters || (v.totalSectors + 1) * v.bps > r.size()) {
        err = "invalid NTFS boot sector";
        return false;
    }
    return true;
}

bool fixups(std::vector<std::uint8_t>& rec, bool pack) {
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
            if (le16(&rec[pos]) != usn) return false;
            rec[pos] = rec[usaOff + 2 * i];
            rec[pos + 1] = rec[usaOff + 2 * i + 1];
        }
    }
    return true;
}

bool readRecord(BlockReader& r, const Vol& v, std::uint64_t n, std::vector<std::uint8_t>& rec, std::string& err) {
    rec.assign(static_cast<std::size_t>(v.recSize), 0);  // records 0-15 lie in the first MFT run
    if (!r.read(v.mftLcn * v.cs + n * v.recSize, rec.data(), rec.size()) || std::memcmp(rec.data(), "FILE", 4) != 0 ||
        !fixups(rec, false)) {
        err = "MFT record " + std::to_string(n) + " cannot be read";
        return false;
    }
    return true;
}

bool writeRecord(BlockDevice& d, const Vol& v, std::uint64_t n, std::vector<std::uint8_t> rec, std::string& err) {
    if (!fixups(rec, true) || !d.write(v.mftLcn * v.cs + n * v.recSize, rec.data(), rec.size())) {
        err = "writing MFT record " + std::to_string(n) + " failed";
        return false;
    }
    return true;
}

// Offset of the $DATA attribute with the given UTF-16 name ("" = unnamed), or 0.
std::size_t findData(const std::vector<std::uint8_t>& rec, const char* name) {
    const std::uint32_t inUse = std::min<std::uint32_t>(le32(&rec[0x18]), static_cast<std::uint32_t>(rec.size()));
    const std::size_t nameLen = std::strlen(name);
    for (std::size_t a = le16(&rec[0x14]); a + 16 <= inUse;) {
        const std::uint32_t type = le32(&rec[a]), len = le32(&rec[a + 4]);
        if (type == 0xFFFFFFFFu || len < 16 || a + len > inUse) break;
        if (type == 0x80 && rec[a + 9] == nameLen) {
            const std::size_t no = le16(&rec[a + 10]);
            bool same = no + 2 * nameLen <= len;
            for (std::size_t i = 0; same && i < nameLen; ++i)
                same = le16(&rec[a + no + 2 * i]) == static_cast<std::uint8_t>(name[i]);
            if (same) return a;
        }
        a += len;
    }
    return 0;
}

struct Run {
    std::uint64_t len = 0;
    std::int64_t lcn = -1;  // -1 = sparse
};

bool decodeRuns(const std::uint8_t* p, std::size_t avail, std::vector<Run>& runs) {
    std::int64_t lcn = 0;
    for (std::size_t i = 0; i < avail && p[i];) {
        const unsigned lb = p[i] & 0x0F, ob = p[i] >> 4;
        if (lb == 0 || lb > 8 || ob > 8 || i + 1 + lb + ob > avail) return false;
        std::uint64_t len = 0;
        for (unsigned k = 0; k < lb; ++k) len |= std::uint64_t{p[i + 1 + k]} << (8 * k);
        Run r;
        r.len = len;
        if (ob) {
            std::int64_t delta = 0;
            for (unsigned k = 0; k < ob; ++k) delta |= static_cast<std::int64_t>(std::uint64_t{p[i + 1 + lb + k]} << (8 * k));
            if (ob < 8 && (p[i + lb + ob] & 0x80)) delta |= -(std::int64_t{1} << (8 * ob));  // sign-extend
            lcn += delta;
            r.lcn = lcn;
        }
        runs.push_back(r);
        i += 1 + lb + ob;
    }
    return true;
}

std::vector<std::uint8_t> encodeRuns(const std::vector<Run>& runs) {
    std::vector<std::uint8_t> out;
    std::int64_t prev = 0;
    for (const Run& r : runs) {
        unsigned lb = 1;
        while (lb < 8 && (r.len >> (8 * lb))) ++lb;
        unsigned ob = 0;
        std::int64_t delta = 0;
        if (r.lcn >= 0) {
            delta = r.lcn - prev;
            ob = 1;
            while (ob < 8) {  // smallest signed width
                const std::int64_t lo = -(std::int64_t{1} << (8 * ob - 1)), hi = (std::int64_t{1} << (8 * ob - 1)) - 1;
                if (delta >= lo && delta <= hi) break;
                ++ob;
            }
            prev = r.lcn;
        }
        out.push_back(static_cast<std::uint8_t>(lb | (ob << 4)));
        for (unsigned k = 0; k < lb; ++k) out.push_back(static_cast<std::uint8_t>(r.len >> (8 * k)));
        for (unsigned k = 0; k < ob; ++k) out.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(delta) >> (8 * k)));
    }
    out.push_back(0);
    return out;
}

// Replaces the mapping pairs and sizes of the non-resident attribute at `a`.
bool rewriteAttr(std::vector<std::uint8_t>& rec, std::size_t a, const std::vector<Run>& runs, std::uint64_t allocated,
                 std::uint64_t dataSize, std::uint64_t initSize, std::string& err) {
    const std::uint32_t len = le32(&rec[a + 4]), inUse = le32(&rec[0x18]), recAlloc = le32(&rec[0x1C]);
    const std::size_t mp = le16(&rec[a + 0x20]);
    const std::vector<std::uint8_t> enc = encodeRuns(runs);
    const std::size_t newLen = (mp + enc.size() + 7) / 8 * 8;
    const std::int64_t delta = static_cast<std::int64_t>(newLen) - static_cast<std::int64_t>(len);
    if (static_cast<std::int64_t>(inUse) + delta > static_cast<std::int64_t>(recAlloc)) {
        err = "the new runlist does not fit into the MFT record";
        return false;
    }
    std::memmove(&rec[a + newLen], &rec[a + len], inUse - (a + len));
    if (delta < 0) std::memset(&rec[inUse + delta], 0, static_cast<std::size_t>(-delta));
    std::memset(&rec[a + mp], 0, newLen - mp);
    std::memcpy(&rec[a + mp], enc.data(), enc.size());
    std::uint64_t vcns = 0;
    for (const Run& r : runs) vcns += r.len;
    put32(&rec[a + 4], static_cast<std::uint32_t>(newLen));
    put64(&rec[a + 0x18], vcns ? vcns - 1 : 0);  // highest VCN
    put64(&rec[a + 0x28], allocated);
    put64(&rec[a + 0x30], dataSize);
    put64(&rec[a + 0x38], initSize);
    put32(&rec[0x18], static_cast<std::uint32_t>(inUse + delta));
    return true;
}

// Reads/writes `bytes` of an attribute's data through its runs.
bool runsIo(BlockDevice* d, BlockReader& r, const Vol& v, const std::vector<Run>& runs, std::vector<std::uint8_t>& data,
            std::size_t bytes, bool write, std::string& err) {
    std::size_t done = 0;
    for (const Run& run : runs) {
        if (done >= bytes) break;
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(run.len * v.cs, bytes - done));
        if (run.lcn < 0) {
            if (!write) std::memset(&data[done], 0, n);
        } else if (write ? !d->write(static_cast<std::uint64_t>(run.lcn) * v.cs, &data[done], n)
                         : !r.read(static_cast<std::uint64_t>(run.lcn) * v.cs, &data[done], n)) {
            err = "the $Bitmap data cannot be accessed";
            return false;
        }
        done += n;
    }
    if (done < bytes) {
        err = "$Bitmap runs are shorter than its size";
        return false;
    }
    return true;
}

bool getBit(const std::vector<std::uint8_t>& b, std::uint64_t i) { return (b[static_cast<std::size_t>(i / 8)] >> (i % 8)) & 1; }
void setBit(std::vector<std::uint8_t>& b, std::uint64_t i, bool on) {
    auto& byte = b[static_cast<std::size_t>(i / 8)];
    byte = static_cast<std::uint8_t>(on ? byte | (1u << (i % 8)) : byte & ~(1u << (i % 8)));
}

struct Bitmap {
    std::vector<std::uint8_t> rec;
    std::size_t attr = 0;
    std::vector<Run> runs;
    std::uint64_t allocated = 0, dataSize = 0;
    std::vector<std::uint8_t> bits;
};

bool loadBitmap(BlockReader& r, const Vol& v, Bitmap& bm, std::string& err) {
    if (!readRecord(r, v, 6, bm.rec, err)) return false;
    bm.attr = findData(bm.rec, "");
    if (!bm.attr || bm.rec[bm.attr + 8] != 1 || le16(&bm.rec[bm.attr + 0x0C]) != 0) {
        err = "$Bitmap has an unexpected layout";
        return false;
    }
    const std::size_t mp = le16(&bm.rec[bm.attr + 0x20]);
    if (!decodeRuns(&bm.rec[bm.attr + mp], le32(&bm.rec[bm.attr + 4]) - mp, bm.runs)) {
        err = "$Bitmap runlist is invalid";
        return false;
    }
    bm.allocated = le64(&bm.rec[bm.attr + 0x28]);
    bm.dataSize = le64(&bm.rec[bm.attr + 0x30]);
    if (bm.dataSize * 8 < v.clusters || bm.dataSize > 64ull * 1024 * 1024) {
        err = "$Bitmap is smaller than the volume";
        return false;
    }
    bm.bits.assign(static_cast<std::size_t>(bm.dataSize), 0);
    return runsIo(nullptr, r, v, bm.runs, bm.bits, bm.bits.size(), false, err);
}

// $VOLUME_INFORMATION (record 3): refuse volumes marked dirty (chkdsk pending).
bool isClean(BlockReader& r, const Vol& v, std::string& err) {
    std::vector<std::uint8_t> rec;
    if (!readRecord(r, v, 3, rec, err)) return false;
    const std::uint32_t inUse = std::min<std::uint32_t>(le32(&rec[0x18]), static_cast<std::uint32_t>(rec.size()));
    for (std::size_t a = le16(&rec[0x14]); a + 24 <= inUse;) {
        const std::uint32_t type = le32(&rec[a]), len = le32(&rec[a + 4]);
        if (type == 0xFFFFFFFFu || len < 24 || a + len > inUse) break;
        if (type == 0x70 && rec[a + 8] == 0) {
            const std::size_t val = a + le16(&rec[a + 0x14]);
            if (val + 12 <= a + len && (le16(&rec[val + 0x0A]) & 0x0001)) {
                err = "the NTFS volume is marked dirty; run chkdsk in Windows first";
                return false;
            }
            return true;
        }
        a += len;
    }
    return true;
}

}  // namespace

std::string shrinkProblem(BlockReader& r, std::uint64_t newBytes) {
    Vol v;
    std::vector<std::uint8_t> boot;
    std::string err;
    Bitmap bm;
    if (!parse(r, v, boot, err) || !loadBitmap(r, v, bm, err)) return err;
    const std::uint64_t newClusters = (newBytes / v.bps - 1) / v.spc;
    for (std::uint64_t c = newClusters; c < v.clusters; ++c)
        if (getBit(bm.bits, c))
            return "NTFS data lies behind the new end (WParted does not move NTFS data yet; shrink the volume in "
                   "Windows first or choose a larger size)";
    return {};
}

std::uint64_t usedBytes(BlockReader& r) {
    Vol v;
    std::vector<std::uint8_t> boot;
    std::string err;
    Bitmap bm;
    if (!parse(r, v, boot, err) || !loadBitmap(r, v, bm, err)) return 0;
    std::uint64_t used = 0;
    for (std::uint64_t c = 0; c < v.clusters; ++c) used += getBit(bm.bits, c);
    return used * v.cs;
}

bool resize(BlockDevice& d, std::uint64_t newBytes, std::string& err) {
    Vol v;
    std::vector<std::uint8_t> boot;
    if (!parse(d, v, boot, err)) return false;
    if (newBytes > d.size()) {
        err = "the new size is larger than the partition";
        return false;
    }
    const std::uint64_t newTotal = newBytes / v.bps - 1;  // last sector: backup boot sector
    const std::uint64_t newClusters = newTotal / v.spc;
    if (newClusters == v.clusters) {
        if (newTotal == v.totalSectors) return true;
    }
    if (newClusters < 64) {
        err = "the new size is too small for NTFS";
        return false;
    }
    if (!isClean(d, v, err)) return false;
    Bitmap bm;
    if (!loadBitmap(d, v, bm, err)) return false;
    // Convention for the bits behind the last cluster (keep what the creator used).
    bool padding = false;
    for (std::uint64_t c = v.clusters; c < bm.dataSize * 8; ++c) padding = padding || getBit(bm.bits, c);
    if (newClusters < v.clusters) {
        for (std::uint64_t c = newClusters; c < v.clusters; ++c)
            if (getBit(bm.bits, c)) {
                err = "clusters behind the new end are in use (data would have to be moved; shrink it in Windows "
                      "first or choose a larger size)";
                return false;
            }
    }
    const std::uint64_t newDataSize = ((newClusters + 7) / 8 + 7) / 8 * 8;
    std::vector<std::uint8_t> bits(static_cast<std::size_t>(newDataSize), 0);
    for (std::uint64_t c = 0; c < std::min(v.clusters, newClusters); ++c) setBit(bits, c, getBit(bm.bits, c));
    for (std::uint64_t c = newClusters; c < newDataSize * 8; ++c) setBit(bits, c, padding);
    std::vector<Run> runs = bm.runs;
    std::uint64_t allocated = bm.allocated;
    if (newDataSize > allocated) {  // allocate the missing $Bitmap clusters at the start of the new space
        const std::uint64_t need = (newDataSize - allocated + v.cs - 1) / v.cs;
        if (v.clusters + need > newClusters) {
            err = "not enough new space for the larger $Bitmap";
            return false;
        }
        for (std::uint64_t c = v.clusters; c < v.clusters + need; ++c) setBit(bits, c, true);
        Run r;
        r.len = need;
        r.lcn = static_cast<std::int64_t>(v.clusters);
        runs.push_back(r);
        allocated += need * v.cs;
    }
    // $BadClus:$Bad must be one sparse run over the whole volume (no bad clusters recorded).
    std::vector<std::uint8_t> bad;
    if (!readRecord(d, v, 8, bad, err)) return false;
    const std::size_t ba = findData(bad, "$Bad");
    if (!ba || bad[ba + 8] != 1) {
        err = "$BadClus:$Bad not found";
        return false;
    }
    std::vector<Run> badRuns;
    const std::size_t bmp = le16(&bad[ba + 0x20]);
    if (!decodeRuns(&bad[ba + bmp], le32(&bad[ba + 4]) - bmp, badRuns)) {
        err = "$BadClus runlist is invalid";
        return false;
    }
    for (const Run& r : badRuns)
        if (r.lcn >= 0) {
            err = "the volume has recorded bad clusters; run chkdsk in Windows first";
            return false;
        }
    const std::vector<Run> newBad{Run{newClusters, -1}};
    const std::uint64_t badInit = le64(&bad[ba + 0x38]);
    if (!rewriteAttr(bad, ba, newBad, newClusters * v.cs, newClusters * v.cs, badInit ? newClusters * v.cs : 0, err))
        return false;
    // Write: bitmap data, the two MFT records, then the boot sectors.
    if (!runsIo(&d, d, v, runs, bits, bits.size(), true, err)) return false;
    std::vector<std::uint8_t> bmRec = bm.rec;
    if (!rewriteAttr(bmRec, bm.attr, runs, allocated, newDataSize, newDataSize, err)) return false;
    if (!writeRecord(d, v, 6, bmRec, err) || !writeRecord(d, v, 8, bad, err)) return false;
    put64(&boot[0x28], newTotal);
    if (!d.write(0, boot.data(), boot.size()) || !d.write(newTotal * v.bps, boot.data(), boot.size())) {
        err = "writing the NTFS boot sectors failed";
        return false;
    }
    return d.flush() || (err = "flush failed", false);
}

}  // namespace wparted::fstools::ntfs
