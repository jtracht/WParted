// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// File system signature detection. Offsets come from the public on-disk
// format descriptions of each file system (and match what util-linux blkid
// probes). This file contains no code derived from GParted.
#include "wparted/core/filesystem.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "wparted/core/types.hpp"

namespace wparted {

const char* fsTypeName(FsType t) noexcept {
    switch (t) {
        case FsType::Unknown: return "unknown";
        case FsType::Ntfs: return "ntfs";
        case FsType::Fat12: return "fat12";
        case FsType::Fat16: return "fat16";
        case FsType::Fat32: return "fat32";
        case FsType::ExFat: return "exfat";
        case FsType::ReFs: return "refs";
        case FsType::BitLocker: return "bitlocker";
        case FsType::Ext2: return "ext2";
        case FsType::Ext3: return "ext3";
        case FsType::Ext4: return "ext4";
        case FsType::Btrfs: return "btrfs";
        case FsType::Xfs: return "xfs";
        case FsType::F2fs: return "f2fs";
        case FsType::Jfs: return "jfs";
        case FsType::ReiserFs: return "reiserfs";
        case FsType::Reiser4: return "reiser4";
        case FsType::Nilfs2: return "nilfs2";
        case FsType::Udf: return "udf";
        case FsType::Ufs: return "ufs";
        case FsType::Hfs: return "hfs";
        case FsType::HfsPlus: return "hfs+";
        case FsType::Apfs: return "apfs";
        case FsType::LinuxSwap: return "linux-swap";
        case FsType::LinuxSuspend: return "linux-suspend";
        case FsType::Luks: return "luks";
        case FsType::Lvm2Pv: return "lvm2 pv";
        case FsType::LinuxRaid: return "linux-raid";
        case FsType::Zfs: return "zfs";
        case FsType::Bcachefs: return "bcachefs";
        case FsType::Minix: return "minix";
    }
    return "unknown";
}

std::vector<FsType> allFsTypes() {
    std::vector<FsType> v;
    for (int i = static_cast<int>(FsType::Ntfs); i <= static_cast<int>(FsType::Minix); ++i)
        v.push_back(static_cast<FsType>(i));
    return v;
}

const char* supportName(Support s) noexcept {
    switch (s) {
        case Support::Supported: return "Supported";
        case Support::Limited: return "Supported with limitations";
        case Support::OfflineOnly: return "Requires offline mode";
        case Support::Experimental: return "Experimental";
        case Support::ReadOnly: return "Read-only";
        case Support::Unsupported: return "Unsupported";
        case Support::Planned: return "Planned";
    }
    return "Unknown";
}

namespace {

// Bytes probed from the start of a partition. 136 KiB covers every signature
// below (the furthest is the ZFS uberblock array at 128 KiB).
constexpr std::size_t kProbeBytes = 136 * 1024;

class Probe {
public:
    explicit Probe(BlockReader& r) {
        std::uint64_t n = std::min<std::uint64_t>(r.size(), kProbeBytes);
        // Read in sector multiples where possible; fall back to the largest
        // readable prefix so that tiny partitions are still probed.
        buf_.resize(static_cast<std::size_t>(n));
        while (n > 0 && !r.read(0, buf_.data(), static_cast<std::size_t>(n))) n /= 2;
        buf_.resize(static_cast<std::size_t>(n));
    }
    const std::uint8_t* at(std::size_t off, std::size_t len) const {
        if (off > buf_.size() || len > buf_.size() - off) return nullptr;
        return buf_.data() + off;
    }
    bool match(std::size_t off, const char* sig, std::size_t len) const {
        const std::uint8_t* p = at(off, len);
        return p && std::memcmp(p, sig, len) == 0;
    }
    bool has(std::size_t off, std::size_t len) const { return at(off, len) != nullptr; }

private:
    std::vector<std::uint8_t> buf_;
};

// exFAT keeps its label as a directory entry (type 0x83) in the root
// directory; read the first root cluster only (the label entry comes first
// in practice and in mkfs.exfat / Windows format output).
std::string exfatLabel(BlockReader& r, const std::uint8_t* bs) {
    const unsigned bpsShift = bs[108], spcShift = bs[109];
    if (bpsShift < 9 || bpsShift > 12 || spcShift > 25 - bpsShift) return {};
    const std::uint64_t bps = 1ull << bpsShift, cluster = bps << spcShift;
    const std::uint32_t root = le32(bs + 96);
    if (root < 2) return {};
    const std::uint64_t off = le32(bs + 88) * bps + (root - 2) * cluster;
    std::vector<std::uint8_t> dir(static_cast<std::size_t>(std::min<std::uint64_t>(cluster, 64 * 1024)));
    if (!r.read(off, dir.data(), dir.size())) return {};
    for (std::size_t i = 0; i + 32 <= dir.size(); i += 32) {
        if (dir[i] == 0x00) break;  // end of directory
        if (dir[i] == 0x83) return utf16leToUtf8(&dir[i + 2], std::min<std::size_t>(dir[i + 1], 11));
    }
    return {};
}

// NTFS keeps its label in the $VOLUME_NAME attribute (0x60) of MFT record 3
// ($Volume). Reads one MFT record and applies the update sequence fixups.
std::string ntfsLabel(BlockReader& r, const std::uint8_t* bs) {
    const std::uint32_t bps = le16(bs + 11);
    const std::uint8_t spc = bs[13];
    if (bps < 256 || bps > 4096 || (bps & (bps - 1)) || spc == 0 || (spc > 0x80 && 256 - spc > 20)) return {};
    const std::uint64_t cluster = spc <= 0x80 ? std::uint64_t{spc} * bps : std::uint64_t{bps} << (256 - spc);
    const auto recRaw = static_cast<std::int8_t>(bs[0x40]);
    const std::uint64_t recSize = recRaw > 0 ? recRaw * cluster : (recRaw > -31 ? 1ull << -recRaw : 0);
    if (recSize < 512 || recSize > 64 * 1024 || cluster == 0) return {};
    const std::uint64_t mftLcn = le64(bs + 0x30);
    if (mftLcn > r.size() / cluster) return {};
    std::vector<std::uint8_t> rec(static_cast<std::size_t>(recSize));
    if (!r.read(mftLcn * cluster + 3 * recSize, rec.data(), rec.size()) || std::memcmp(rec.data(), "FILE", 4) != 0)
        return {};
    const std::size_t usaOff = le16(&rec[4]), usaCount = le16(&rec[6]);
    if (usaCount == 0 || usaOff + 2 * usaCount > rec.size()) return {};
    for (std::size_t i = 1; i < usaCount; ++i) {  // fixup stride is 512 bytes
        const std::size_t pos = i * 512 - 2;
        if (pos + 2 > rec.size()) break;
        if (rec[pos] != rec[usaOff] || rec[pos + 1] != rec[usaOff + 1]) return {};  // torn record
        rec[pos] = rec[usaOff + 2 * i];
        rec[pos + 1] = rec[usaOff + 2 * i + 1];
    }
    for (std::size_t a = le16(&rec[0x14]); a + 24 <= rec.size();) {
        const std::uint32_t type = le32(&rec[a]), len = le32(&rec[a + 4]);
        if (type == 0xFFFFFFFFu || len < 24 || a + len > rec.size()) break;
        if (type == 0x60 && rec[a + 8] == 0) {  // resident $VOLUME_NAME
            const std::uint32_t vlen = le32(&rec[a + 0x10]);
            const std::size_t voff = le16(&rec[a + 0x14]);
            if (std::uint64_t{voff} + vlen <= len) return utf16leToUtf8(&rec[a + voff], vlen / 2);
            break;
        }
        a += len;
    }
    return {};
}

std::string trimmedAscii(const std::uint8_t* p, std::size_t len) {
    std::string s;
    for (std::size_t i = 0; i < len && p[i]; ++i) s += (p[i] >= 0x20 && p[i] < 0x7F) ? static_cast<char>(p[i]) : '?';
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

std::string uuidText(const std::uint8_t* p) {  // RFC 4122 byte order (big endian)
    char buf[40];
    std::snprintf(buf, sizeof buf,
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", p[0], p[1],
                  p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14],
                  p[15]);
    return buf;
}

std::string serialText(const std::uint8_t* p, int bytes) {  // FAT/NTFS volume serials
    char buf[24];
    if (bytes == 4)
        std::snprintf(buf, sizeof buf, "%04X-%04X", le16(p + 2), le16(p));
    else
        std::snprintf(buf, sizeof buf, "%016llX", static_cast<unsigned long long>(le64(p)));
    return buf;
}

bool detectFat(const Probe& pr, FsDetection& out) {
    const std::uint8_t* bs = pr.at(0, 512);
    if (!bs || bs[510] != 0x55 || bs[511] != 0xAA) return false;
    if (bs[0] != 0xEB && bs[0] != 0xE9) return false;
    std::uint16_t bytesPerSector = le16(bs + 11);
    std::uint8_t secPerCluster = bs[13];
    std::uint16_t reserved = le16(bs + 14);
    std::uint8_t fats = bs[16];
    if (bytesPerSector < 512 || bytesPerSector > 4096 || (bytesPerSector & (bytesPerSector - 1))) return false;
    if (secPerCluster == 0 || (secPerCluster & (secPerCluster - 1)) || reserved == 0 || fats == 0) return false;

    bool fat32Label = std::memcmp(bs + 82, "FAT32   ", 8) == 0;
    std::uint16_t rootEntries = le16(bs + 17);
    std::uint32_t totalSectors = le16(bs + 19) ? le16(bs + 19) : le32(bs + 32);
    std::uint32_t fatSize = le16(bs + 22) ? le16(bs + 22) : le32(bs + 36);
    if (fatSize == 0 || totalSectors == 0) return false;
    std::uint32_t rootDirSectors = (rootEntries * 32u + bytesPerSector - 1) / bytesPerSector;
    std::uint64_t meta = reserved + static_cast<std::uint64_t>(fats) * fatSize + rootDirSectors;
    if (meta >= totalSectors) return false;
    // Cluster count thresholds from the Microsoft FAT specification.
    std::uint64_t clusters = (totalSectors - meta) / secPerCluster;
    if (le16(bs + 22) == 0 || fat32Label) {
        out.type = FsType::Fat32;
        if (bs[66] == 0x29) {
            out.label = trimmedAscii(bs + 71, 11);
            out.uuid = serialText(bs + 67, 4);
        }
    } else {
        out.type = clusters < 4085 ? FsType::Fat12 : (clusters < 65525 ? FsType::Fat16 : FsType::Fat32);
        if (bs[38] == 0x29) {
            out.label = trimmedAscii(bs + 43, 11);
            out.uuid = serialText(bs + 39, 4);
        }
    }
    if (out.label == "NO NAME") out.label.clear();
    return true;
}

bool detectExt(const Probe& pr, FsDetection& out) {
    const std::uint8_t* sb = pr.at(1024, 1024);
    if (!sb || le16(sb + 0x38) != 0xEF53) return false;
    std::uint32_t compat = le32(sb + 0x5C), incompat = le32(sb + 0x60), roCompat = le32(sb + 0x64);
    const std::uint32_t kExt4Incompat = 0x0040 /*extents*/ | 0x0080 /*64bit*/ | 0x0200 /*flex_bg*/;
    const std::uint32_t kExt4RoCompat = 0x0008 /*huge_file*/ | 0x0010 /*gdt_csum*/ | 0x0020 /*dir_nlink*/ |
                                        0x0040 /*extra_isize*/ | 0x0400 /*metadata_csum*/;
    if ((incompat & kExt4Incompat) || (roCompat & kExt4RoCompat))
        out.type = FsType::Ext4;
    else if (compat & 0x0004 /*has_journal*/)
        out.type = FsType::Ext3;
    else
        out.type = FsType::Ext2;
    out.uuid = uuidText(sb + 0x68);
    out.label = trimmedAscii(sb + 0x78, 16);
    return true;
}

}  // namespace

FsDetection detectFilesystem(BlockReader& reader) {
    FsDetection d;
    Probe pr(reader);

    // Boot-sector based (offset 3 OEM id) first: these are unambiguous.
    if (pr.match(3, "-FVE-FS-", 8)) { d.type = FsType::BitLocker; return d; }
    if (pr.match(3, "NTFS    ", 8)) {
        d.type = FsType::Ntfs;
        if (const auto* p = pr.at(0x48, 8)) d.uuid = serialText(p, 8);
        if (const auto* bs = pr.at(0, 512)) d.label = ntfsLabel(reader, bs);
        return d;
    }
    if (pr.match(3, "EXFAT   ", 8)) {
        d.type = FsType::ExFat;
        if (const auto* p = pr.at(100, 4)) d.uuid = serialText(p, 4);
        if (const auto* bs = pr.at(0, 512)) d.label = exfatLabel(reader, bs);
        return d;
    }
    if (pr.match(3, "ReFS\0\0\0\0", 8)) { d.type = FsType::ReFs; return d; }

    if (pr.match(0, "LUKS\xBA\xBE", 6)) {
        d.type = FsType::Luks;
        if (const auto* p = pr.at(168, 40)) d.uuid = trimmedAscii(p, 40);
        return d;
    }
    if (pr.match(0, "XFSB", 4)) {
        d.type = FsType::Xfs;
        if (const auto* p = pr.at(32, 16)) d.uuid = uuidText(p);
        if (const auto* p = pr.at(108, 12)) d.label = trimmedAscii(p, 12);
        return d;
    }
    if (pr.match(32, "NXSB", 4)) { d.type = FsType::Apfs; return d; }
    for (std::size_t sec = 0; sec < 4; ++sec) {
        if (pr.match(sec * 512, "LABELONE", 8) && pr.match(sec * 512 + 24, "LVM2 001", 8)) {
            d.type = FsType::Lvm2Pv;
            return d;
        }
    }
    if (pr.match(0x10040, "_BHRfS_M", 8)) {
        d.type = FsType::Btrfs;
        if (const auto* p = pr.at(0x10020, 16)) d.uuid = uuidText(p);
        if (const auto* p = pr.at(0x1012B, 256)) d.label = trimmedAscii(p, 256);
        return d;
    }
    {
        static const std::uint8_t kBcachefsMagic[16] = {0xc6, 0x85, 0x73, 0xf6, 0x4e, 0x1a, 0x45, 0xca,
                                                        0x82, 0x65, 0xf5, 0x7f, 0x48, 0xba, 0x6d, 0x81};
        if (const auto* p = pr.at(4096 + 24, 16)) {
            if (std::memcmp(p, kBcachefsMagic, 16) == 0) { d.type = FsType::Bcachefs; return d; }
        }
    }
    if (const auto* p = pr.at(1024, 4)) {
        if (le32(p) == 0xF2F52010u) {  // superblock at 1 KiB: uuid at +108, UTF-16 volume_name[512] at +124
            d.type = FsType::F2fs;
            if (const auto* u = pr.at(1024 + 108, 16)) d.uuid = uuidText(u);
            if (const auto* n = pr.at(1024 + 124, 1024)) d.label = utf16leToUtf8(n, 512);
            return d;
        }
    }
    // Linux MD RAID superblock v1.1 (offset 0) and v1.2 (offset 4 KiB).
    for (std::size_t off : {std::size_t{0}, std::size_t{4096}}) {
        if (const auto* p = pr.at(off, 4)) {
            if (le32(p) == 0xA92B4EFCu) { d.type = FsType::LinuxRaid; return d; }
        }
    }
    if (pr.match(32768, "JFS1", 4)) {
        d.type = FsType::Jfs;
        if (const auto* p = pr.at(32768 + 136, 16)) d.uuid = uuidText(p);
        if (const auto* p = pr.at(32768 + 152, 16)) d.label = trimmedAscii(p, 16);
        return d;
    }
    if (pr.match(65536, "ReIsEr4", 7)) { d.type = FsType::Reiser4; return d; }
    if (pr.match(65536 + 52, "ReIsErFs", 8) || pr.match(65536 + 52, "ReIsEr2Fs", 9) ||
        pr.match(65536 + 52, "ReIsEr3Fs", 9)) {
        d.type = FsType::ReiserFs;
        return d;
    }
    if (detectExt(pr, d)) return d;
    if (const auto* p = pr.at(1024 + 6, 2)) {
        if (le16(p) == 0x3434) { d.type = FsType::Nilfs2; return d; }
    }
    if (pr.match(1024, "H+", 2) || pr.match(1024, "HX", 2)) { d.type = FsType::HfsPlus; return d; }
    // Linux swap / hibernation image: signature at the end of the first page.
    for (std::size_t page : {std::size_t{4096}, std::size_t{8192}, std::size_t{16384}, std::size_t{65536}}) {
        if (pr.match(page - 10, "SWAPSPACE2", 10) || pr.match(page - 10, "SWAP-SPACE", 10)) {
            d.type = FsType::LinuxSwap;
            if (const auto* p = pr.at(1024 + 12, 16)) d.uuid = uuidText(p);
            if (const auto* p = pr.at(1024 + 28, 16)) d.label = trimmedAscii(p, 16);
            return d;
        }
        if (pr.match(page - 10, "S1SUSPEND", 9) || pr.match(page - 10, "S2SUSPEND", 9) ||
            pr.match(page - 10, "ULSUSPEND", 9)) {
            d.type = FsType::LinuxSuspend;
            return d;
        }
    }
    // UDF: volume structure descriptors at 32 KiB in 2 KiB steps.
    for (std::size_t i = 0; i < 16; ++i) {
        std::size_t off = 32768 + i * 2048 + 1;
        if (pr.match(off, "NSR02", 5) || pr.match(off, "NSR03", 5)) { d.type = FsType::Udf; return d; }
    }
    // UFS1/UFS2 superblock magic.
    for (std::size_t sbOff : {std::size_t{8192}, std::size_t{65536}}) {
        if (const auto* p = pr.at(sbOff + 1372, 4)) {
            if (le32(p) == 0x00011954u || be32(p) == 0x00011954u || le32(p) == 0x19540119u ||
                be32(p) == 0x19540119u) {
                d.type = FsType::Ufs;
                return d;
            }
        }
    }
    // ZFS: uberblock magic in the first vdev label's uberblock array.
    for (std::size_t off = 128 * 1024; off + 8 <= kProbeBytes; off += 1024) {
        if (const auto* p = pr.at(off, 8)) {
            if (le64(p) == 0x00bab10cull || (be32(p) == 0 && be32(p + 4) == 0x00bab10cu)) {
                d.type = FsType::Zfs;
                return d;
            }
        }
    }
    if (detectFat(pr, d)) return d;
    if (pr.match(1024, "BD", 2)) { d.type = FsType::Hfs; return d; }
    // Minix: weak 16-bit magics, checked last to limit false positives.
    if (const auto* p = pr.at(1024 + 16, 2)) {
        std::uint16_t m = le16(p);
        if (m == 0x137F || m == 0x138F || m == 0x2468 || m == 0x2478) { d.type = FsType::Minix; return d; }
    }
    if (const auto* p = pr.at(1024 + 24, 2)) {
        if (le16(p) == 0x4D5A) { d.type = FsType::Minix; return d; }
    }
    return d;
}

const FsCapabilities& capabilitiesFor(FsType t) {
    // What this build can do on disk *image files* (core/src/executor.cpp,
    // fs_tools.cpp). Physical disks are never written; validation refuses
    // execution on them (execution.unavailable). Move/copy are raw block
    // operations of the same size ("Limited"); copies are not offered where a
    // duplicate identity confuses the system (LUKS, LVM, RAID, ZFS, ...).
    using S = Support;
    static const FsCapabilities table[] = {
        {FsType::Unknown, S::Unsupported, S::Unsupported, S::Unsupported, S::Unsupported, S::Unsupported, S::Unsupported, S::Unsupported, S::Unsupported, S::Unsupported, S::Unsupported},
        {FsType::Ntfs, S::Supported, S::Supported, S::Planned, S::Supported, S::Limited, S::Limited, S::Limited, S::Planned, S::Supported, S::Supported},
        {FsType::Fat12, S::Supported, S::Limited, S::Planned, S::Planned, S::Planned, S::Limited, S::Limited, S::Planned, S::Supported, S::Supported},
        {FsType::Fat16, S::Supported, S::Supported, S::Supported, S::Limited, S::Supported, S::Limited, S::Limited, S::Limited, S::Supported, S::Supported},
        {FsType::Fat32, S::Supported, S::Supported, S::Supported, S::Limited, S::Supported, S::Limited, S::Limited, S::Limited, S::Supported, S::Supported},
        {FsType::ExFat, S::Supported, S::Limited, S::Planned, S::Unsupported, S::Unsupported, S::Limited, S::Limited, S::Planned, S::Supported, S::Supported},
        {FsType::ReFs, S::Supported, S::Limited, S::Unsupported, S::Unsupported, S::Unsupported, S::Limited, S::Limited, S::Unsupported, S::Unsupported, S::Unsupported},
        {FsType::BitLocker, S::Supported, S::Unsupported, S::Unsupported, S::Unsupported, S::Unsupported, S::Limited, S::Planned, S::Unsupported, S::Unsupported, S::Unsupported},
        {FsType::Ext2, S::Supported, S::Planned, S::Planned, S::Planned, S::Planned, S::Limited, S::Limited, S::Planned, S::Supported, S::Supported},
        {FsType::Ext3, S::Supported, S::Planned, S::Planned, S::Planned, S::Planned, S::Limited, S::Limited, S::Planned, S::Supported, S::Limited},
        {FsType::Ext4, S::Supported, S::Planned, S::Planned, S::Planned, S::Planned, S::Limited, S::Limited, S::Planned, S::Supported, S::Limited},
        {FsType::Btrfs, S::Supported, S::Planned, S::Planned, S::Planned, S::Planned, S::Limited, S::Limited, S::Planned, S::Planned, S::Planned},
        {FsType::Xfs, S::Supported, S::Planned, S::Planned, S::Planned, S::Unsupported, S::Limited, S::Limited, S::Planned, S::Planned, S::Planned},
        {FsType::F2fs, S::Experimental, S::Planned, S::Planned, S::Planned, S::Unsupported, S::Limited, S::Limited, S::Planned, S::Unsupported, S::Unsupported},
        {FsType::Jfs, S::Experimental, S::Planned, S::Planned, S::Planned, S::Unsupported, S::Limited, S::Limited, S::Planned, S::Planned, S::Planned},
        {FsType::ReiserFs, S::Experimental, S::Planned, S::Planned, S::Planned, S::Planned, S::Limited, S::Limited, S::Planned, S::Planned, S::Planned},
        {FsType::Reiser4, S::Experimental, S::Planned, S::Planned, S::Unsupported, S::Unsupported, S::Limited, S::Limited, S::Planned, S::Unsupported, S::Unsupported},
        {FsType::Nilfs2, S::Experimental, S::Planned, S::Planned, S::Planned, S::Planned, S::Limited, S::Limited, S::Unsupported, S::Planned, S::Planned},
        {FsType::Udf, S::Experimental, S::Planned, S::Planned, S::Unsupported, S::Unsupported, S::Limited, S::Limited, S::Unsupported, S::Planned, S::Planned},
        {FsType::Ufs, S::Experimental, S::Unsupported, S::Unsupported, S::Unsupported, S::Unsupported, S::Limited, S::Limited, S::Unsupported, S::Unsupported, S::Unsupported},
        {FsType::Hfs, S::Experimental, S::Planned, S::Planned, S::Unsupported, S::Planned, S::Limited, S::Limited, S::Unsupported, S::Unsupported, S::Unsupported},
        {FsType::HfsPlus, S::Experimental, S::Planned, S::Planned, S::Unsupported, S::Planned, S::Limited, S::Limited, S::Planned, S::Unsupported, S::Unsupported},
        {FsType::Apfs, S::Supported, S::Unsupported, S::Unsupported, S::Unsupported, S::Unsupported, S::Limited, S::Limited, S::Unsupported, S::Unsupported, S::Unsupported},
        {FsType::LinuxSwap, S::Supported, S::Unsupported, S::Planned, S::Planned, S::Planned, S::Limited, S::Limited, S::Unsupported, S::Supported, S::Supported},
        {FsType::LinuxSuspend, S::Supported, S::Unsupported, S::Unsupported, S::Unsupported, S::Unsupported, S::Limited, S::Planned, S::Unsupported, S::Unsupported, S::Unsupported},
        {FsType::Luks, S::Supported, S::Unsupported, S::Unsupported, S::Planned, S::Planned, S::Limited, S::Planned, S::Unsupported, S::Unsupported, S::Unsupported},
        {FsType::Lvm2Pv, S::Supported, S::Unsupported, S::Planned, S::Planned, S::Planned, S::Limited, S::Unsupported, S::Planned, S::Unsupported, S::Unsupported},
        {FsType::LinuxRaid, S::Supported, S::Unsupported, S::Unsupported, S::Unsupported, S::Unsupported, S::Limited, S::Planned, S::Unsupported, S::Unsupported, S::Unsupported},
        {FsType::Zfs, S::Experimental, S::Unsupported, S::Unsupported, S::Unsupported, S::Unsupported, S::Limited, S::Planned, S::Unsupported, S::Unsupported, S::Unsupported},
        {FsType::Bcachefs, S::Experimental, S::Unsupported, S::Planned, S::Planned, S::Unsupported, S::Limited, S::Planned, S::Planned, S::Unsupported, S::Unsupported},
        {FsType::Minix, S::Experimental, S::Unsupported, S::Planned, S::Unsupported, S::Unsupported, S::Limited, S::Limited, S::Planned, S::Unsupported, S::Unsupported},
    };
    for (const auto& c : table)
        if (c.type == t) return c;
    return table[0];
}

}  // namespace wparted
