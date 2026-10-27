// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#include "wparted/core/layout_view.hpp"

#include <algorithm>

namespace wparted {

namespace {

void fillGaps(std::vector<Segment>& out, std::vector<Segment> parts, std::uint64_t first, std::uint64_t last,
              std::uint64_t minGapSectors) {
    std::sort(parts.begin(), parts.end(), [](const Segment& a, const Segment& b) { return a.firstLba < b.firstLba; });
    std::uint64_t cursor = first;
    auto gap = [&](std::uint64_t from, std::uint64_t to) {  // inclusive range
        if (to >= from && to - from + 1 >= minGapSectors) {
            Segment g;
            g.kind = Segment::Kind::Unallocated;
            g.firstLba = from;
            g.sectorCount = to - from + 1;
            out.push_back(g);
        }
    };
    for (auto& s : parts) {
        if (s.firstLba > cursor) gap(cursor, s.firstLba - 1);
        std::uint64_t end = s.firstLba + (s.sectorCount ? s.sectorCount - 1 : 0);
        cursor = std::max(cursor, end + 1);
        out.push_back(std::move(s));
    }
    if (last >= cursor) gap(cursor, last);
}

}  // namespace

std::vector<Segment> buildSegments(const DiskLayout& layout, std::uint64_t minGapBytes) {
    std::vector<Segment> result;
    const std::uint64_t total = layout.totalSectors();
    if (total == 0) return result;
    const std::uint64_t minGap = std::max<std::uint64_t>(1, minGapBytes / layout.sectorSize);

    std::vector<Segment> top, logical;
    for (std::size_t i = 0; i < layout.partitions.size(); ++i) {
        const auto& p = layout.partitions[i];
        Segment s;
        s.kind = Segment::Kind::Partition;
        s.partitionIndex = static_cast<int>(i);
        s.firstLba = p.firstLba;
        s.sectorCount = p.sectorCount;
        (p.kind == PartitionKind::Logical ? logical : top).push_back(s);
    }

    if (layout.type == TableType::None && !top.empty()) return top;  // whole-device file system

    std::uint64_t first = 0, last = total - 1;
    if (layout.type == TableType::Gpt || layout.type == TableType::Mbr) {
        first = layout.firstUsableLba;
        last = std::min(layout.lastUsableLba, total - 1);
    }
    fillGaps(result, std::move(top), first, last, minGap);

    for (auto& s : result) {
        if (s.kind != Segment::Kind::Partition) continue;
        const auto& p = layout.partitions[static_cast<std::size_t>(s.partitionIndex)];
        if (p.kind != PartitionKind::Extended) continue;
        std::vector<Segment> inside;
        for (const auto& l : logical)
            if (l.firstLba >= p.firstLba && l.firstLba <= p.lastLba()) inside.push_back(l);
        fillGaps(s.children, std::move(inside), p.firstLba, p.lastLba(), minGap);
    }
    return result;
}

}  // namespace wparted
