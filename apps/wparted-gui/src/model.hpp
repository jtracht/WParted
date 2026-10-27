// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// View model shared by the disk map and the partition list: one Row per
// partition or unallocated region, in left-to-right disk order.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "theme.hpp"
#include "wparted/core/layout_view.hpp"
#include "wparted/win/disk_discovery.hpp"

namespace wparted::ui {

struct Row {
    Segment::Kind kind = Segment::Kind::Unallocated;
    int partitionIndex = -1;  // into layout.partitions
    std::uint64_t firstLba = 0;
    std::uint64_t sectorCount = 0;
    int depth = 0;            // 1 for rows inside an extended partition
    int parentRow = -1;       // extended partition row for depth 1
    bool isExtended = false;
};

struct DiskView {
    const win::PhysicalDisk* disk = nullptr;
    // False when only Windows volumes are known: gaps are then *unknown*, not free.
    bool tableRead = false;
    const DiskLayout* layout = nullptr;    // planned layout, disk->layout or a volumes-only fallback
    const DiskLayout* original = nullptr;  // layout before planned operations (for volume mapping)
    std::vector<Row> rows;
};

// Windows volume of a partition shown in the view. Planned partitions keep the
// volume of the partition they originate from; new partitions have none.
inline const win::VolumeInfo* viewVolume(const DiskView& v, const PartitionEntry& p) {
    if (!v.disk) return nullptr;
    if (v.original && p.planId > 0) {
        for (const auto& op : v.original->partitions)
            if (op.planId == p.planId) return win::volumeForPartition(*v.disk, op);
        return nullptr;
    }
    return win::volumeForPartition(*v.disk, p);
}

inline std::vector<Row> flattenSegments(const DiskLayout& layout, const std::vector<Segment>& segs) {
    std::vector<Row> rows;
    for (const auto& s : segs) {
        Row r;
        r.kind = s.kind;
        r.partitionIndex = s.partitionIndex;
        r.firstLba = s.firstLba;
        r.sectorCount = s.sectorCount;
        r.isExtended = s.partitionIndex >= 0 &&
                       layout.partitions[static_cast<std::size_t>(s.partitionIndex)].kind == PartitionKind::Extended;
        rows.push_back(r);
        const int parent = static_cast<int>(rows.size()) - 1;
        for (const auto& c : s.children) {
            Row cr;
            cr.kind = c.kind;
            cr.partitionIndex = c.partitionIndex;
            cr.firstLba = c.firstLba;
            cr.sectorCount = c.sectorCount;
            cr.depth = 1;
            cr.parentRow = parent;
            rows.push_back(cr);
        }
    }
    return rows;
}

// Messages between the disk map and its parent window.
constexpr UINT DM_SETVIEW = WM_APP + 1;       // lParam: const DiskView*, wParam: const Palette*
constexpr UINT DM_SETSELECTION = WM_APP + 2;  // wParam: row index or -1
constexpr UINT DMN_SELECT = WM_APP + 3;       // sent to parent, wParam: row index
constexpr UINT DMN_ACTIVATE = WM_APP + 4;     // sent to parent, wParam: row index

bool registerDiskMapClass(HINSTANCE inst);
constexpr wchar_t kDiskMapClass[] = L"WPartedDiskMap";

}  // namespace wparted::ui
