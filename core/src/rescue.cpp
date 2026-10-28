// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#include "wparted/core/rescue.hpp"

#include <algorithm>
#include <set>

#include "wparted/core/layout_view.hpp"

namespace wparted {

namespace {

std::uint64_t be64(const std::uint8_t* p) {
    return (static_cast<std::uint64_t>(be32(p)) << 32) | be32(p + 4);
}

// Cheap first look at a candidate start: only positions with a plausible
// boot sector or superblock magic get the full signature probe.
bool promising(BlockReader& r, std::uint64_t offset) {
    std::uint8_t b[2048];
    if (!r.read(offset, b, sizeof b)) return false;
    if (b[510] == 0x55 && b[511] == 0xAA) return true;            // FAT/NTFS/exFAT/BitLocker boot sector
    if (le16(b + 1024 + 0x38) == 0xEF53) return true;             // ext2/3/4
    if (b[0] == 'X' && b[1] == 'F' && b[2] == 'S' && b[3] == 'B') return true;
    if (b[0] == 'L' && b[1] == 'U' && b[2] == 'K' && b[3] == 'S') return true;
    if (b[1024] == 'H' && (b[1025] == '+' || b[1025] == 'X')) return true;
    if (le32(b + 1024) == 0xF2F52010u) return true;               // f2fs
    for (int s = 0; s < 4; ++s)
        if (b[s * 512] == 'L' && b[s * 512 + 1] == 'A' && b[s * 512 + 2] == 'B') return true;  // LVM2 label
    return false;
}

}  // namespace

std::uint64_t filesystemSizeBytes(BlockReader& r, FsType type) {
    std::uint8_t b[4096];
    switch (type) {
        case FsType::Ntfs:
            if (!r.read(0, b, 512)) return 0;
            return le64(b + 0x28) * le16(b + 0x0B) + le16(b + 0x0B);  // + backup boot sector
        case FsType::Fat12:
        case FsType::Fat16:
        case FsType::Fat32: {
            if (!r.read(0, b, 512)) return 0;
            const std::uint64_t sectors = le16(b + 19) ? le16(b + 19) : le32(b + 32);
            return sectors * le16(b + 11);
        }
        case FsType::ExFat:
            if (!r.read(0, b, 512)) return 0;
            return b[108] < 13 ? le64(b + 72) << b[108] : 0;
        case FsType::Ext2:
        case FsType::Ext3:
        case FsType::Ext4: {
            if (!r.read(1024, b, 1024)) return 0;
            const std::uint32_t logBlock = le32(b + 24);
            if (logBlock > 6) return 0;
            std::uint64_t blocks = le32(b + 4);
            if (le32(b + 0x60) & 0x80) blocks |= static_cast<std::uint64_t>(le32(b + 0x150)) << 32;  // 64bit
            return blocks * (1024ull << logBlock);
        }
        case FsType::Xfs:
            if (!r.read(0, b, 512)) return 0;
            return be64(b + 8) * be32(b + 4);
        case FsType::Btrfs:
            if (!r.read(0x10000, b, 512)) return 0;
            return le64(b + 0x70);
        case FsType::LinuxSwap:
            if (!r.read(1024, b, 512)) return 0;
            return (static_cast<std::uint64_t>(le32(b + 4)) + 1) * 4096;
        default:
            return 0;
    }
}

std::vector<RescueCandidate> rescueScan(BlockReader& r, const DiskLayout& layout, const RescueOptions& o) {
    std::vector<RescueCandidate> out;
    const std::uint32_t ss = layout.sectorSize ? layout.sectorSize : r.sectorSize();
    const std::uint64_t total = r.size() / ss;
    if (total < 2) return out;

    // Regions to scan: unallocated gaps of a readable table, or the whole
    // device when there is no table at all.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> gaps;  // first, last (inclusive)
    if ((layout.type == TableType::Gpt || layout.type == TableType::Mbr)) {
        for (const auto& s : buildSegments(layout, 0)) {
            if (s.kind == Segment::Kind::Unallocated) gaps.push_back({s.firstLba, s.firstLba + s.sectorCount - 1});
            for (const auto& c : s.children)
                if (c.kind == Segment::Kind::Unallocated) gaps.push_back({c.firstLba, c.firstLba + c.sectorCount - 1});
        }
    } else if (layout.partitions.empty()) {
        gaps.push_back({1, total - 1});
    }

    std::uint64_t work = 0, done = 0;
    for (const auto& g : gaps) work += g.second - g.first + 1;
    const std::uint64_t step = std::max<std::uint64_t>(1, o.stepBytes / ss);

    for (const auto& g : gaps) {
        std::set<std::uint64_t> positions;
        for (std::uint64_t lba = (g.first + step - 1) / step * step; lba <= g.second; lba += step) positions.insert(lba);
        if (o.cylinderPositions) {
            // DOS-era partitions start at sector 63 or on 255x63 cylinder boundaries.
            if (g.first <= 63 && 63 <= g.second) positions.insert(63);
            const std::uint64_t cyl = 255ull * 63;
            for (std::uint64_t lba = (g.first + cyl - 1) / cyl * cyl; lba <= g.second; lba += cyl) positions.insert(lba);
        }
        std::uint64_t skipUntil = 0;
        for (std::uint64_t lba : positions) {
            if (o.cancel && o.cancel->load()) return out;
            if (lba < skipUntil) continue;
            if (o.progress) o.progress(done + (lba - g.first), work);
            const std::uint64_t offset = lba * ss;
            const std::uint64_t len = (g.second - lba + 1) * ss;
            if (len < 4096 || !promising(r, offset)) continue;
            SubRangeReader sub(r, offset, len);
            FsDetection fs = detectFilesystem(sub);
            if (fs.type == FsType::Unknown) continue;
            RescueCandidate c;
            c.firstLba = lba;
            c.fs = fs;
            const std::uint64_t bytes = filesystemSizeBytes(sub, fs.type);
            c.sectorCount = bytes ? std::min<std::uint64_t>((bytes + ss - 1) / ss, g.second - lba + 1) : 0;
            out.push_back(c);
            if (out.size() >= o.maxCandidates) return out;
            if (c.sectorCount) skipUntil = lba + c.sectorCount;  // do not report positions inside a found file system
        }
        done += g.second - g.first + 1;
    }
    if (o.progress) o.progress(work, work);
    return out;
}

std::size_t planRecreate(OperationQueue& queue, const std::vector<RescueCandidate>& found, std::string& error) {
    std::vector<RescueCandidate> sorted = found;
    std::sort(sorted.begin(), sorted.end(), [](const RescueCandidate& a, const RescueCandidate& b) { return a.firstLba < b.firstLba; });
    std::size_t added = 0;
    std::uint64_t end = 0;
    for (const auto& c : sorted) {
        if (!c.sectorCount || c.fs.type == FsType::Unknown || c.firstLba < end) continue;
        Operation op;
        op.type = OpType::CreatePartition;
        op.table = queue.current().type;
        op.kind = PartitionKind::Primary;
        op.firstLba = c.firstLba;
        op.sectorCount = c.sectorCount;
        op.fs = c.fs.type;
        op.keepContents = true;
        std::string err;
        if (!queue.add(op, err)) {
            if (error.empty()) error = "sector " + std::to_string(c.firstLba) + ": " + err;
            continue;
        }
        end = c.firstLba + c.sectorCount;
        ++added;
    }
    return added;
}

}  // namespace wparted
