// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// NTFS creation and read-only check on image files (WP-040), written from the
// NTFS on-disk documentation of the Linux-NTFS project. No code from ntfs-3g
// is used.
//
// create: NTFS 3.1 with 4 KiB clusters, 1 KiB MFT records (or the sector size
//   if larger) and 4 KiB index records. System files 0-11 ($MFT, $MFTMirr,
//   $LogFile (empty, all 0xFF - Windows initialises it), $Volume, $AttrDef,
//   root, $Bitmap, $Boot, $BadClus, $Secure, $UpCase, $Extend), records 12-15
//   reserved. Simplifications: version-1.2 standard information (no security
//   ids, $Secure is empty), $Extend has no $ObjId/$Quota/$Reparse (Windows
//   adds them), the up-case table covers the Unicode simple uppercase mappings
//   of the common scripts, the volume is not bootable.
// check: read-only - MFT records readable (magic, fixups), $MFTMirr equal to
//   the first MFT records, every cluster referenced by a non-resident
//   attribute of an in-use record marked in $Bitmap, no cluster used twice,
//   backup boot sector equal to the boot sector.
#include <algorithm>
#include <cctype>
#include <ctime>
#include <cstring>
#include <map>

#include "fs_internal.hpp"
#include "wparted/core/types.hpp"

namespace wparted::fstools::ntfs {
namespace {

void p16(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
void p32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void p64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::size_t align8(std::size_t n) { return (n + 7) & ~std::size_t{7}; }

// Update sequence array: protects the last two bytes of every 512-byte block.
void protect(std::vector<std::uint8_t>& rec, std::size_t usaOff) {
    const std::size_t count = rec.size() / 512 + 1;
    p16(&rec[4], static_cast<std::uint32_t>(usaOff));
    p16(&rec[6], static_cast<std::uint32_t>(count));
    p16(&rec[usaOff], 1);
    for (std::size_t i = 1; i < count; ++i) {
        const std::size_t pos = i * 512 - 2;
        rec[usaOff + 2 * i] = rec[pos];
        rec[usaOff + 2 * i + 1] = rec[pos + 1];
        p16(&rec[pos], 1);
    }
}

bool unprotect(std::vector<std::uint8_t>& rec) {
    const std::size_t usaOff = le16(&rec[4]), count = le16(&rec[6]);
    if (count == 0 || usaOff + 2 * count > rec.size()) return false;
    const std::uint16_t usn = le16(&rec[usaOff]);
    for (std::size_t i = 1; i < count && i * 512 <= rec.size(); ++i) {
        const std::size_t pos = i * 512 - 2;
        if (le16(&rec[pos]) != usn) return false;
        rec[pos] = rec[usaOff + 2 * i];
        rec[pos + 1] = rec[usaOff + 2 * i + 1];
    }
    return true;
}

std::vector<std::uint8_t> u16(const std::string& ascii) {
    std::vector<std::uint8_t> out(ascii.size() * 2, 0);
    for (std::size_t i = 0; i < ascii.size(); ++i) out[2 * i] = static_cast<std::uint8_t>(ascii[i]);
    return out;
}

struct Run {
    std::uint64_t len = 0;
    std::int64_t lcn = -1;  // -1 = sparse
};

std::vector<std::uint8_t> encode(const std::vector<Run>& runs) {
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
            while (ob < 8) {
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

bool decode(const std::uint8_t* p, std::size_t avail, std::vector<Run>& runs) {
    std::int64_t lcn = 0;
    for (std::size_t i = 0; i < avail && p[i];) {
        const unsigned lb = p[i] & 0x0F, ob = p[i] >> 4;
        if (lb == 0 || lb > 8 || ob > 8 || i + 1 + lb + ob > avail) return false;
        Run r;
        for (unsigned k = 0; k < lb; ++k) r.len |= std::uint64_t{p[i + 1 + k]} << (8 * k);
        if (ob) {
            std::int64_t delta = 0;
            for (unsigned k = 0; k < ob; ++k) delta |= static_cast<std::int64_t>(std::uint64_t{p[i + 1 + lb + k]} << (8 * k));
            if (ob < 8 && (p[i + lb + ob] & 0x80)) delta |= -(std::int64_t{1} << (8 * ob));
            lcn += delta;
            r.lcn = lcn;
        }
        runs.push_back(r);
        i += 1 + lb + ob;
    }
    return true;
}

// Builds one MFT record attribute by attribute.
class Record {
public:
    Record(std::size_t size, std::uint32_t number, std::uint16_t seq, std::uint16_t flags, std::uint16_t links)
        : b_(size, 0) {
        std::memcpy(b_.data(), "FILE", 4);
        usa_ = 0x30;
        at_ = align8(usa_ + 2 * (size / 512 + 1));
        p16(&b_[0x10], seq);
        p16(&b_[0x12], links);
        p16(&b_[0x14], static_cast<std::uint32_t>(at_));
        p16(&b_[0x16], flags);
        p32(&b_[0x1C], static_cast<std::uint32_t>(size));
        p32(&b_[0x2C], number);
    }
    void resident(std::uint32_t type, const std::vector<std::uint8_t>& name, const std::vector<std::uint8_t>& value,
                  std::uint8_t residentFlags = 0) {
        const std::size_t nameOff = 0x18, valueOff = align8(nameOff + name.size());
        const std::size_t len = align8(valueOff + value.size());
        std::uint8_t* a = &b_[at_];
        p32(a, type);
        p32(a + 4, static_cast<std::uint32_t>(len));
        a[9] = static_cast<std::uint8_t>(name.size() / 2);
        p16(a + 10, static_cast<std::uint32_t>(nameOff));
        p16(a + 14, id_++);
        p32(a + 0x10, static_cast<std::uint32_t>(value.size()));
        p16(a + 0x14, static_cast<std::uint32_t>(valueOff));
        a[0x16] = residentFlags;
        if (!name.empty()) std::memcpy(a + nameOff, name.data(), name.size());
        if (!value.empty()) std::memcpy(a + valueOff, value.data(), value.size());
        at_ += len;
    }
    void nonResident(std::uint32_t type, const std::vector<std::uint8_t>& name, const std::vector<Run>& runs,
                     std::uint64_t allocated, std::uint64_t size, std::uint64_t init) {
        const std::vector<std::uint8_t> mp = encode(runs);
        const std::size_t nameOff = 0x40, mpOff = align8(nameOff + name.size());
        const std::size_t len = align8(mpOff + mp.size());
        std::uint64_t vcns = 0;
        for (const Run& r : runs) vcns += r.len;
        std::uint8_t* a = &b_[at_];
        p32(a, type);
        p32(a + 4, static_cast<std::uint32_t>(len));
        a[8] = 1;
        a[9] = static_cast<std::uint8_t>(name.size() / 2);
        p16(a + 10, static_cast<std::uint32_t>(nameOff));
        p16(a + 14, id_++);
        p64(a + 0x10, 0);
        p64(a + 0x18, vcns ? vcns - 1 : 0);
        p16(a + 0x20, static_cast<std::uint32_t>(mpOff));
        p64(a + 0x28, allocated);
        p64(a + 0x30, size);
        p64(a + 0x38, init);
        if (!name.empty()) std::memcpy(a + nameOff, name.data(), name.size());
        std::memcpy(a + mpOff, mp.data(), mp.size());
        at_ += len;
    }
    // Finished record with fixups applied, ready to write.
    std::vector<std::uint8_t> sealed() {
        std::vector<std::uint8_t> out = raw();
        protect(out, usa_);
        return out;
    }
    std::vector<std::uint8_t> raw() const {
        std::vector<std::uint8_t> out = b_;
        p32(&out[at_], 0xFFFFFFFFu);
        p32(&out[0x18], static_cast<std::uint32_t>(at_ + 8));
        p16(&out[0x28], id_);
        return out;
    }
    std::size_t used() const { return at_ + 8; }

private:
    std::vector<std::uint8_t> b_;
    std::size_t usa_ = 0, at_ = 0;
    std::uint16_t id_ = 0;
};

std::uint64_t ntNow() {
    return (static_cast<std::uint64_t>(std::time(nullptr)) + 11644473600ull) * 10000000ull;
}

std::vector<std::uint8_t> standardInfo(std::uint32_t attrs, std::uint64_t t) {
    std::vector<std::uint8_t> v(48, 0);
    for (int i = 0; i < 4; ++i) p64(&v[8 * i], t);
    p32(&v[0x20], attrs);
    return v;
}

std::vector<std::uint8_t> fileName(std::uint64_t parent, const std::string& name, std::uint32_t attrs, std::uint64_t alloc,
                                   std::uint64_t size, std::uint64_t t) {
    const std::vector<std::uint8_t> n = u16(name);
    std::vector<std::uint8_t> v(0x42 + n.size(), 0);
    p64(&v[0], parent);
    for (int i = 0; i < 4; ++i) p64(&v[8 + 8 * i], t);
    p64(&v[0x28], alloc);
    p64(&v[0x30], size);
    p32(&v[0x38], attrs);
    v[0x40] = static_cast<std::uint8_t>(name.size());
    v[0x41] = 3;  // Win32 and DOS name
    std::memcpy(&v[0x42], n.data(), n.size());
    return v;
}

// $INDEX_ROOT value with an index header and the given entries (already encoded).
std::vector<std::uint8_t> indexRoot(std::uint32_t indexedType, std::uint32_t collation, std::uint32_t blockSize,
                                    std::int8_t clustersPerBlock, const std::vector<std::uint8_t>& entries, bool large) {
    std::vector<std::uint8_t> v(0x20 + entries.size(), 0);
    p32(&v[0], indexedType);
    p32(&v[4], collation);
    p32(&v[8], blockSize);
    v[12] = static_cast<std::uint8_t>(clustersPerBlock);
    p32(&v[0x10], 0x10);
    p32(&v[0x14], static_cast<std::uint32_t>(0x10 + entries.size()));
    p32(&v[0x18], static_cast<std::uint32_t>(0x10 + entries.size()));
    v[0x1C] = large ? 1 : 0;
    std::memcpy(&v[0x20], entries.data(), entries.size());
    return v;
}

std::vector<std::uint8_t> endEntry(bool child) {
    std::vector<std::uint8_t> e(child ? 24 : 16, 0);
    p16(&e[8], static_cast<std::uint32_t>(e.size()));
    p16(&e[12], child ? 3 : 2);  // (has subnode) | last
    return e;  // a child VCN of 0 is already zero
}

// Simple uppercase mappings (Unicode) for the scripts in common use.
std::vector<std::uint16_t> upcaseTable() {
    std::vector<std::uint16_t> t(65536);
    for (std::uint32_t c = 0; c < 65536; ++c) t[c] = static_cast<std::uint16_t>(c);
    auto shift = [&](std::uint32_t from, std::uint32_t to, int delta) {
        for (std::uint32_t c = from; c <= to; ++c) t[c] = static_cast<std::uint16_t>(static_cast<int>(c) + delta);
    };
    auto pairs = [&](std::uint32_t from, std::uint32_t to, bool upperEven) {  // alternating upper/lower
        for (std::uint32_t c = from; c <= to; ++c)
            if ((c % 2 == 1) == upperEven) t[c] = static_cast<std::uint16_t>(c - 1);
    };
    shift('a', 'z', -32);
    shift(0xE0, 0xF6, -32);
    shift(0xF8, 0xFE, -32);
    t[0xFF] = 0x178;
    t[0xB5] = 0x39C;
    pairs(0x100, 0x12F, true);
    pairs(0x132, 0x137, true);
    pairs(0x139, 0x148, false);
    pairs(0x14A, 0x177, true);
    pairs(0x179, 0x17E, false);
    pairs(0x1A0, 0x1A5, true);
    pairs(0x1DE, 0x1EF, true);
    pairs(0x1F8, 0x21F, true);
    pairs(0x222, 0x233, true);
    t[0x3AC] = 0x386;
    shift(0x3AD, 0x3AF, -37);
    shift(0x3B1, 0x3C1, -32);
    t[0x3C2] = 0x3A3;
    shift(0x3C3, 0x3CB, -32);
    t[0x3CC] = 0x38C;
    shift(0x3CD, 0x3CE, -63);
    pairs(0x3D8, 0x3EF, true);
    shift(0x430, 0x44F, -32);
    shift(0x450, 0x45F, -80);
    pairs(0x460, 0x481, true);
    pairs(0x48A, 0x4BF, true);
    pairs(0x4C1, 0x4CE, false);
    pairs(0x4D0, 0x52F, true);
    shift(0x561, 0x586, -48);
    pairs(0x1E00, 0x1E95, true);
    pairs(0x1EA0, 0x1EFF, true);
    shift(0x2170, 0x217F, -16);
    shift(0x24D0, 0x24E9, -26);
    shift(0xFF41, 0xFF5A, -32);
    return t;
}

struct SysFile {
    std::uint32_t record;
    const char* name;
    std::uint32_t attrs;
    std::uint64_t alloc, size;
};

}  // namespace

bool create(BlockDevice& d, const std::string& label, std::uint64_t hidden, std::string& err) {
    const std::uint32_t bps = d.sectorSize();
    if (bps < 512 || bps > 4096 || (bps & (bps - 1))) {
        err = "unsupported sector size for NTFS";
        return false;
    }
    const std::uint64_t cs = 4096, spc = cs / bps;
    const std::uint64_t recSize = std::max<std::uint64_t>(1024, bps), idxSize = 4096;
    const std::uint64_t sectors = d.size() / bps;
    if (sectors < 2 + spc) {
        err = "the partition is too small for NTFS";
        return false;
    }
    const std::uint64_t totalSectors = sectors - 1;  // the last sector holds the backup boot sector
    const std::uint64_t clusters = totalSectors / spc;
    if (clusters < 1024 || clusters > 0xFFFFFFFFull) {
        err = "the partition size is outside what WParted creates for NTFS (4 MiB to 16 TiB)";
        return false;
    }
    const std::uint64_t volBytes = clusters * cs;
    const std::uint64_t mib = 1024 * 1024;
    const std::uint64_t logBytes = volBytes < 200 * mib ? 2 * mib
                                   : volBytes < 400 * mib ? 4 * mib
                                                          : std::min<std::uint64_t>(64 * mib, std::max<std::uint64_t>(4 * mib, (volBytes / 100 + mib - 1) / mib * mib));
    const std::uint64_t mftRecords = 64;
    const std::uint64_t bitmapBytes = ((clusters + 7) / 8 + 7) / 8 * 8;
    auto clustersFor = [&](std::uint64_t bytes) { return (bytes + cs - 1) / cs; };
    // Cluster layout.
    std::uint64_t next = 0;
    auto take = [&](std::uint64_t n) {
        const std::uint64_t at = next;
        next += n;
        return at;
    };
    const std::uint64_t bootLcn = take(clustersFor(8192));
    const std::uint64_t mftLcn = take(clustersFor(mftRecords * recSize));
    const std::uint64_t mftBitmapLcn = take(1);  // ntfs-3g extends the MFT bitmap cluster by cluster
    const std::uint64_t mirrLcn = take(clustersFor(4 * recSize));
    const std::uint64_t logLcn = take(clustersFor(logBytes));
    const std::uint64_t attrDefLcn = take(1);
    const std::uint64_t rootIdxLcn = take(clustersFor(idxSize));
    const std::uint64_t bitmapLcn = take(clustersFor(bitmapBytes));
    const std::uint64_t upLcn = take(clustersFor(131072));
    if (next + 16 > clusters) {
        err = "the partition is too small for NTFS";
        return false;
    }
    const std::uint64_t t = ntNow();
    auto ref = [](std::uint64_t rec, std::uint64_t seq) { return rec | (seq << 48); };
    auto seqOf = [](std::uint32_t r) -> std::uint16_t { return static_cast<std::uint16_t>(r == 0 ? 1 : r); };
    const std::uint64_t root = ref(5, 5);
    const std::uint32_t sysAttrs = 0x06;  // hidden | system
    const std::uint32_t dirFlag = 0x10000000;

    // Contents of the data files.
    std::vector<std::uint8_t> upcase(131072);
    {
        const auto table = upcaseTable();
        for (std::size_t i = 0; i < table.size(); ++i) p16(&upcase[2 * i], table[i]);
    }
    std::vector<std::uint8_t> attrDef(2560, 0);
    {
        struct Def {
            const char* name;
            std::uint32_t type, collation, flags;
            std::uint64_t min, max;
        };
        const std::uint64_t any = ~std::uint64_t{0};
        const Def defs[] = {{"$STANDARD_INFORMATION", 0x10, 0, 0x40, 48, 72}, {"$ATTRIBUTE_LIST", 0x20, 0, 0x80, 0, any},
                            {"$FILE_NAME", 0x30, 1, 0x42, 68, 578},           {"$OBJECT_ID", 0x40, 0, 0x40, 0, 256},
                            {"$SECURITY_DESCRIPTOR", 0x50, 0, 0x80, 0, any},  {"$VOLUME_NAME", 0x60, 0, 0x40, 2, 256},
                            {"$VOLUME_INFORMATION", 0x70, 0, 0x40, 12, 12},   {"$DATA", 0x80, 0, 0, 0, any},
                            {"$INDEX_ROOT", 0x90, 0, 0x40, 0, any},           {"$INDEX_ALLOCATION", 0xA0, 0, 0x80, 0, any},
                            {"$BITMAP", 0xB0, 0, 0x80, 0, any},               {"$REPARSE_POINT", 0xC0, 0, 0x80, 0, 16384},
                            {"$EA_INFORMATION", 0xD0, 0, 0x40, 8, 8},         {"$EA", 0xE0, 0, 0, 0, 65536},
                            {"$LOGGED_UTILITY_STREAM", 0x100, 0, 0x80, 0, 65536}};
        std::size_t i = 0;
        for (const Def& def : defs) {
            std::uint8_t* e = &attrDef[160 * i++];
            const auto n = u16(def.name);
            std::memcpy(e, n.data(), n.size());
            p32(e + 0x80, def.type);
            p32(e + 0x88, def.collation);
            p32(e + 0x8C, def.flags);
            p64(e + 0x90, def.min);
            p64(e + 0x98, def.max);
        }
    }
    std::vector<std::uint8_t> bitmap(static_cast<std::size_t>(bitmapBytes), 0);
    for (std::uint64_t c = 0; c < next; ++c) bitmap[static_cast<std::size_t>(c / 8)] |= static_cast<std::uint8_t>(1u << (c % 8));

    const SysFile files[] = {
        {0, "$MFT", sysAttrs, mftRecords * recSize, mftRecords * recSize},
        {1, "$MFTMirr", sysAttrs, clustersFor(4 * recSize) * cs, 4 * recSize},
        {2, "$LogFile", sysAttrs, clustersFor(logBytes) * cs, logBytes},
        {3, "$Volume", sysAttrs, 0, 0},
        {4, "$AttrDef", sysAttrs, cs, attrDef.size()},
        {5, ".", sysAttrs | dirFlag, 0, 0},
        {6, "$Bitmap", sysAttrs, clustersFor(bitmapBytes) * cs, bitmapBytes},
        {7, "$Boot", sysAttrs, clustersFor(8192) * cs, 8192},
        {8, "$BadClus", sysAttrs, 0, 0},
        {9, "$Secure", sysAttrs | 0x20000000, 0, 0},
        {10, "$UpCase", sysAttrs, upcase.size(), upcase.size()},
        {11, "$Extend", sysAttrs | dirFlag, 0, 0},
    };
    auto fn = [&](const SysFile& f) { return fileName(root, f.name, f.attrs, f.alloc, f.size, t); };

    // Root directory index block: entries sorted by up-cased name.
    std::vector<const SysFile*> sorted;
    for (const auto& f : files) sorted.push_back(&f);
    std::sort(sorted.begin(), sorted.end(), [](const SysFile* a, const SysFile* b) {
        std::string x = a->name, y = b->name;
        for (char& c : x) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        for (char& c : y) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return x < y;
    });
    std::vector<std::uint8_t> indx(static_cast<std::size_t>(idxSize), 0);
    {
        std::memcpy(indx.data(), "INDX", 4);
        const std::size_t usa = 0x28, entriesAt = align8(usa + 2 * (idxSize / 512 + 1));
        std::size_t at = entriesAt;
        for (const SysFile* f : sorted) {
            const auto key = fn(*f);
            const std::size_t len = align8(16 + key.size());
            p64(&indx[at], ref(f->record, seqOf(f->record)));
            p16(&indx[at + 8], static_cast<std::uint32_t>(len));
            p16(&indx[at + 10], static_cast<std::uint32_t>(key.size()));
            std::memcpy(&indx[at + 16], key.data(), key.size());
            at += len;
        }
        const auto end = endEntry(false);
        std::memcpy(&indx[at], end.data(), end.size());
        at += end.size();
        p32(&indx[0x18], static_cast<std::uint32_t>(entriesAt - 0x18));
        p32(&indx[0x1C], static_cast<std::uint32_t>(at - 0x18));
        p32(&indx[0x20], static_cast<std::uint32_t>(idxSize - 0x18));
        protect(indx, usa);
    }

    // MFT records.
    std::vector<std::vector<std::uint8_t>> recs(static_cast<std::size_t>(mftRecords));
    auto base = [&](const SysFile& f, std::uint16_t flags) {
        Record r(static_cast<std::size_t>(recSize), f.record, seqOf(f.record), flags, 1);
        r.resident(0x10, {}, standardInfo(f.attrs & 0xFFFF, t));
        r.resident(0x30, {}, fn(f), 1);
        return r;
    };
    {
        Record r = base(files[0], 1);
        r.nonResident(0x80, {}, {{clustersFor(mftRecords * recSize), static_cast<std::int64_t>(mftLcn)}},
                      clustersFor(mftRecords * recSize) * cs, mftRecords * recSize, mftRecords * recSize);
        r.nonResident(0xB0, {}, {{1, static_cast<std::int64_t>(mftBitmapLcn)}}, cs, 8, 8);
        recs[0] = r.raw();
    }
    {
        Record r = base(files[1], 1);
        r.nonResident(0x80, {}, {{clustersFor(4 * recSize), static_cast<std::int64_t>(mirrLcn)}}, files[1].alloc, files[1].size,
                      files[1].size);
        recs[1] = r.raw();
    }
    {
        Record r = base(files[2], 1);
        r.nonResident(0x80, {}, {{clustersFor(logBytes), static_cast<std::int64_t>(logLcn)}}, files[2].alloc, logBytes, logBytes);
        recs[2] = r.raw();
    }
    {
        Record r = base(files[3], 1);
        std::vector<std::uint8_t> name16;
        {
            std::vector<std::uint8_t> tmp(2 * 128 + 2, 0);
            if (!label.empty() && !utf8ToUtf16le(label, tmp.data(), 128)) {
                err = "invalid NTFS label";
                return false;
            }
            std::size_t n = 0;
            while (n < 128 && (tmp[2 * n] || tmp[2 * n + 1])) ++n;
            name16.assign(tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(2 * n));
        }
        r.resident(0x60, {}, name16);
        std::vector<std::uint8_t> info(12, 0);
        info[8] = 3;  // NTFS 3.1
        info[9] = 1;
        r.resident(0x70, {}, info);
        r.resident(0x80, {}, {});
        recs[3] = r.raw();
    }
    {
        Record r = base(files[4], 1);
        r.nonResident(0x80, {}, {{1, static_cast<std::int64_t>(attrDefLcn)}}, cs, attrDef.size(), attrDef.size());
        recs[4] = r.raw();
    }
    {
        Record r = base(files[5], 3);
        r.resident(0x90, u16("$I30"), indexRoot(0x30, 1, static_cast<std::uint32_t>(idxSize), 1, endEntry(true), true));
        r.nonResident(0xA0, u16("$I30"), {{clustersFor(idxSize), static_cast<std::int64_t>(rootIdxLcn)}}, idxSize, idxSize, idxSize);
        std::vector<std::uint8_t> bm(8, 0);
        bm[0] = 1;
        r.resident(0xB0, u16("$I30"), bm);
        recs[5] = r.raw();
    }
    {
        Record r = base(files[6], 1);
        r.nonResident(0x80, {}, {{clustersFor(bitmapBytes), static_cast<std::int64_t>(bitmapLcn)}}, files[6].alloc, bitmapBytes,
                      bitmapBytes);
        recs[6] = r.raw();
    }
    {
        Record r = base(files[7], 1);
        r.nonResident(0x80, {}, {{clustersFor(8192), static_cast<std::int64_t>(bootLcn)}}, files[7].alloc, 8192, 8192);
        recs[7] = r.raw();
    }
    {
        Record r = base(files[8], 1);
        r.resident(0x80, {}, {});
        r.nonResident(0x80, u16("$Bad"), {{clusters, -1}}, clusters * cs, clusters * cs, clusters * cs);
        recs[8] = r.raw();
    }
    {
        Record r = base(files[9], 1 | 8);
        r.nonResident(0x80, u16("$SDS"), {}, 0, 0, 0);
        r.resident(0x90, u16("$SDH"), indexRoot(0, 0x12, static_cast<std::uint32_t>(idxSize), 1, endEntry(false), false));
        r.resident(0x90, u16("$SII"), indexRoot(0, 0x10, static_cast<std::uint32_t>(idxSize), 1, endEntry(false), false));
        recs[9] = r.raw();
    }
    {
        Record r = base(files[10], 1);
        r.nonResident(0x80, {}, {{clustersFor(upcase.size()), static_cast<std::int64_t>(upLcn)}}, upcase.size(), upcase.size(),
                      upcase.size());
        recs[10] = r.raw();
    }
    {
        Record r = base(files[11], 3);
        r.resident(0x90, u16("$I30"), indexRoot(0x30, 1, static_cast<std::uint32_t>(idxSize), 1, endEntry(false), false));
        recs[11] = r.raw();
    }
    for (std::uint32_t n = 12; n < mftRecords; ++n) {
        Record r(static_cast<std::size_t>(recSize), n, static_cast<std::uint16_t>(n < 16 ? n : 1), n < 16 ? 1 : 0, 0);
        if (n < 16) r.resident(0x10, {}, standardInfo(sysAttrs, t));
        recs[n] = r.raw();
    }
    for (std::size_t n = 0; n < recs.size(); ++n) {
        if (recs[n].size() < 8 || le32(&recs[n][0x18]) > recSize) {
            err = "an NTFS system record does not fit";
            return false;
        }
        protect(recs[n], 0x30);
    }

    // Write everything; the boot sector last.
    auto writeAt = [&](std::uint64_t lcn, const std::vector<std::uint8_t>& data) { return d.write(lcn * cs, data.data(), data.size()); };
    std::vector<std::uint8_t> mft(static_cast<std::size_t>(mftRecords * recSize));
    for (std::size_t n = 0; n < recs.size(); ++n) std::memcpy(&mft[n * recSize], recs[n].data(), recs[n].size());
    std::vector<std::uint8_t> mirr(mft.begin(), mft.begin() + static_cast<std::ptrdiff_t>(4 * recSize));
    mirr.resize(static_cast<std::size_t>(clustersFor(4 * recSize) * cs), 0);
    std::vector<std::uint8_t> log(static_cast<std::size_t>(logBytes), 0xFF);
    attrDef.resize(static_cast<std::size_t>(cs), 0);
    bitmap.resize(static_cast<std::size_t>(clustersFor(bitmapBytes) * cs), 0);
    std::vector<std::uint8_t> bootArea(static_cast<std::size_t>(clustersFor(8192) * cs), 0);
    std::vector<std::uint8_t> mftBitmap(static_cast<std::size_t>(cs), 0);
    mftBitmap[0] = 0xFF;
    mftBitmap[1] = 0xFF;  // records 0-15
    if (!writeAt(mftBitmapLcn, mftBitmap) || !writeAt(mftLcn, mft) || !writeAt(mirrLcn, mirr) || !writeAt(logLcn, log) || !writeAt(attrDefLcn, attrDef) ||
        !writeAt(rootIdxLcn, indx) || !writeAt(bitmapLcn, bitmap) || !writeAt(upLcn, upcase) || !writeAt(bootLcn, bootArea)) {
        err = "writing the NTFS system files failed";
        return false;
    }
    std::vector<std::uint8_t> boot(bps, 0);
    const std::uint8_t jump[3] = {0xEB, 0x52, 0x90};
    std::memcpy(&boot[0], jump, 3);
    std::memcpy(&boot[3], "NTFS    ", 8);
    p16(&boot[0x0B], bps);
    boot[0x0D] = static_cast<std::uint8_t>(spc);
    boot[0x15] = 0xF8;
    p16(&boot[0x18], 63);
    p16(&boot[0x1A], 255);
    p32(&boot[0x1C], static_cast<std::uint32_t>(std::min<std::uint64_t>(hidden, 0xFFFFFFFFu)));
    boot[0x24] = 0x80;
    boot[0x26] = 0x80;
    p64(&boot[0x28], totalSectors);
    p64(&boot[0x30], mftLcn);
    p64(&boot[0x38], mirrLcn);
    boot[0x40] = recSize < cs ? static_cast<std::uint8_t>(-static_cast<int>(recSize == 1024 ? 10 : recSize == 2048 ? 11 : 12))
                              : static_cast<std::uint8_t>(recSize / cs);
    boot[0x44] = 1;  // one cluster per index record (4 KiB)
    const Guid serial = randomGuid();
    std::memcpy(&boot[0x48], serial.bytes.data(), 8);
    boot[510] = 0x55;
    boot[511] = 0xAA;
    if (!d.write(totalSectors * bps, boot.data(), boot.size()) || !d.write(0, boot.data(), boot.size())) {
        err = "writing the NTFS boot sectors failed";
        return false;
    }
    return d.flush() || (err = "flush failed", false);
}

bool check(BlockReader& r, CheckReport& rep, std::string& err) {
    std::vector<std::uint8_t> boot(std::max<std::uint32_t>(512, r.sectorSize()));
    if (!r.read(0, boot.data(), boot.size()) || std::memcmp(&boot[3], "NTFS    ", 8) != 0) {
        err = "no NTFS boot sector";
        return false;
    }
    const std::uint32_t bps = le16(&boot[11]), spc = boot[13];
    if (bps != r.sectorSize() || spc == 0) {
        err = "unsupported NTFS geometry";
        return false;
    }
    const std::uint64_t cs = std::uint64_t{bps} * spc, total = le64(&boot[0x28]), clusters = total / spc;
    const auto raw = static_cast<std::int8_t>(boot[0x40]);
    const std::uint64_t recSize = raw > 0 ? raw * cs : (raw > -31 ? 1ull << -raw : 0);
    const std::uint64_t mftLcn = le64(&boot[0x30]), mirrLcn = le64(&boot[0x38]);
    if (recSize < 512 || recSize > 65536 || mftLcn >= clusters || mirrLcn >= clusters || (total + 1) * bps > r.size()) {
        err = "invalid NTFS boot sector";
        return false;
    }
    auto error = [&](const std::string& e) {
        rep.ok = false;
        if (rep.errors.size() < 20) rep.errors.push_back(e);
    };
    rep.totalBytes = clusters * cs;
    std::vector<std::uint8_t> backup(boot.size());
    if (!r.read(total * bps, backup.data(), backup.size()) || backup != boot)
        rep.warnings.push_back("the backup boot sector differs from the boot sector");

    // $MFT: its own runs give the location of every record.
    std::vector<std::uint8_t> rec(static_cast<std::size_t>(recSize));
    if (!r.read(mftLcn * cs, rec.data(), rec.size()) || std::memcmp(rec.data(), "FILE", 4) != 0 || !unprotect(rec)) {
        err = "MFT record 0 cannot be read";
        return false;
    }
    auto attrs = [&](const std::vector<std::uint8_t>& m, auto&& visit) {
        const std::uint32_t inUse = std::min<std::uint32_t>(le32(&m[0x18]), static_cast<std::uint32_t>(m.size()));
        for (std::size_t a = le16(&m[0x14]); a + 4 <= inUse;) {
            const std::uint32_t type = le32(&m[a]);
            if (type == 0xFFFFFFFFu) return true;
            if (a + 16 > inUse) return false;
            const std::uint32_t len = le32(&m[a + 4]);
            if (len < 16 || a + len > inUse) return false;
            visit(a, type, len);
            a += len;
        }
        return false;  // no end marker
    };
    std::vector<Run> mftRuns;
    std::uint64_t mftBytes = 0;
    std::vector<std::uint8_t> mftBitmap;
    attrs(rec, [&](std::size_t a, std::uint32_t type, std::uint32_t len) {
        if (type == 0x80 && rec[a + 9] == 0 && rec[a + 8] == 1) {
            const std::size_t mp = le16(&rec[a + 0x20]);
            decode(&rec[a + mp], len - mp, mftRuns);
            mftBytes = le64(&rec[a + 0x30]);
        }
        if (type == 0xB0 && rec[a + 9] == 0) {
            if (rec[a + 8] == 0) {
                const std::size_t vo = le16(&rec[a + 0x14]), vl = le32(&rec[a + 0x10]);
                if (a + vo + vl <= a + len) mftBitmap.assign(rec.begin() + static_cast<std::ptrdiff_t>(a + vo), rec.begin() + static_cast<std::ptrdiff_t>(a + vo + vl));
            } else {
                std::vector<Run> runs;
                const std::size_t mp = le16(&rec[a + 0x20]);
                decode(&rec[a + mp], len - mp, runs);
                mftBitmap.assign(static_cast<std::size_t>(std::min<std::uint64_t>(le64(&rec[a + 0x30]), 64ull << 20)), 0);
                std::size_t done = 0;
                for (const Run& run : runs)
                    for (std::uint64_t c = 0; c < run.len && done < mftBitmap.size() && run.lcn >= 0; ++c) {
                        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(cs, mftBitmap.size() - done));
                        r.read((static_cast<std::uint64_t>(run.lcn) + c) * cs, &mftBitmap[done], n);
                        done += n;
                    }
            }
        }
    });
    if (mftRuns.empty() || mftBytes < 16 * recSize || mftBitmap.empty()) {
        err = "the $MFT record has no usable $DATA or $BITMAP";
        return false;
    }
    auto recordOffset = [&](std::uint64_t n, std::uint64_t& off) {
        std::uint64_t vcnByte = n * recSize;
        for (const Run& run : mftRuns) {
            if (vcnByte < run.len * cs) {
                if (run.lcn < 0) return false;
                off = static_cast<std::uint64_t>(run.lcn) * cs + vcnByte;
                return true;
            }
            vcnByte -= run.len * cs;
        }
        return false;
    };
    // $Bitmap (record 6) and the clusters every in-use record references.
    std::vector<std::uint8_t> used(static_cast<std::size_t>((clusters + 7) / 8), 0);
    std::vector<std::uint8_t> volBitmap;
    std::uint64_t referenced = 0;
    const std::uint64_t records = std::min<std::uint64_t>(mftBytes / recSize, mftBitmap.size() * 8);
    std::vector<std::uint8_t> first4;
    for (std::uint64_t n = 0; n < records; ++n) {
        const bool inUse = (mftBitmap[static_cast<std::size_t>(n / 8)] >> (n % 8)) & 1;
        if (!inUse) continue;
        std::uint64_t off = 0;
        std::vector<std::uint8_t> m(static_cast<std::size_t>(recSize));
        if (!recordOffset(n, off) || !r.read(off, m.data(), m.size())) {
            error("MFT record " + std::to_string(n) + " cannot be read");
            continue;
        }
        if (n < 4) first4.insert(first4.end(), m.begin(), m.end());
        if (std::memcmp(m.data(), "FILE", 4) != 0 || !unprotect(m)) {
            error("MFT record " + std::to_string(n) + " is damaged (signature or update sequence)");
            continue;
        }
        if (!(le16(&m[0x16]) & 1)) {
            error("MFT record " + std::to_string(n) + " is marked in use in the MFT bitmap but not in the record");
            continue;
        }
        const bool ok = attrs(m, [&](std::size_t a, std::uint32_t, std::uint32_t len) {
            if (m[a + 8] != 1) return;
            std::vector<Run> runs;
            const std::size_t mp = le16(&m[a + 0x20]);
            if (mp >= len || !decode(&m[a + mp], len - mp, runs)) {
                error("MFT record " + std::to_string(n) + ": invalid runlist");
                return;
            }
            for (const Run& run : runs) {
                if (run.lcn < 0) continue;
                for (std::uint64_t c = static_cast<std::uint64_t>(run.lcn); c < static_cast<std::uint64_t>(run.lcn) + run.len; ++c) {
                    if (c >= clusters) {
                        error("MFT record " + std::to_string(n) + " references a cluster beyond the volume");
                        break;
                    }
                    auto& byte = used[static_cast<std::size_t>(c / 8)];
                    if ((byte >> (c % 8)) & 1) error("cluster " + std::to_string(c) + " is used twice");
                    byte = static_cast<std::uint8_t>(byte | (1u << (c % 8)));
                    ++referenced;
                }
            }
            if (n == 6 && le32(&m[a]) == 0x80 && m[a + 9] == 0) {
                volBitmap.assign(static_cast<std::size_t>(std::min<std::uint64_t>(le64(&m[a + 0x30]), 64ull << 20)), 0);
                std::size_t done = 0;
                for (const Run& run : runs)
                    for (std::uint64_t c = 0; c < run.len && done < volBitmap.size() && run.lcn >= 0; ++c) {
                        const std::size_t k = static_cast<std::size_t>(std::min<std::uint64_t>(cs, volBitmap.size() - done));
                        r.read((static_cast<std::uint64_t>(run.lcn) + c) * cs, &volBitmap[done], k);
                        done += k;
                    }
            }
        });
        if (!ok) error("MFT record " + std::to_string(n) + ": attributes are damaged");
    }
    // $MFTMirr holds the first (up to) four records.
    {
        std::vector<std::uint8_t> mirr(first4.size());
        if (first4.empty() || !r.read(mirrLcn * cs, mirr.data(), mirr.size()) || mirr != first4)
            error("$MFTMirr differs from the first MFT records");
    }
    if (volBitmap.size() * 8 < clusters) {
        error("$Bitmap is smaller than the volume");
    } else {
        std::uint64_t marked = 0, lost = 0;
        for (std::uint64_t c = 0; c < clusters; ++c) {
            const bool bm = (volBitmap[static_cast<std::size_t>(c / 8)] >> (c % 8)) & 1;
            const bool ref = (used[static_cast<std::size_t>(c / 8)] >> (c % 8)) & 1;
            marked += bm;
            if (ref && !bm) {
                error("cluster " + std::to_string(c) + " is in use but marked free in $Bitmap");
                break;
            }
            lost += bm && !ref;
        }
        if (lost) rep.warnings.push_back(std::to_string(lost) + " cluster(s) are marked in use but not referenced (chkdsk frees them)");
        rep.usedBytes = marked * cs;
    }
    (void)referenced;
    return true;
}

}  // namespace wparted::fstools::ntfs
