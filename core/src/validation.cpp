// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#include "wparted/core/validation.hpp"

#include <algorithm>
#include <set>

namespace wparted {

bool ValidationResult::ok() const noexcept {
    for (const auto& d : diagnostics)
        if (d.severity == Severity::Error) return false;
    return true;
}

std::uint64_t minimumSize(FsType t) noexcept {
    constexpr std::uint64_t MiB = 1024 * 1024;
    switch (t) {
        case FsType::Fat16: return 16 * MiB;
        case FsType::Fat32: return 33 * MiB;
        case FsType::Ntfs: return 8 * MiB;
        case FsType::Ext3:
        case FsType::Ext4: return 8 * MiB;
        case FsType::Ext2: return 2 * MiB;
        case FsType::Btrfs: return 256 * MiB;
        case FsType::Xfs: return 300 * MiB;
        case FsType::F2fs: return 64 * MiB;
        case FsType::ReFs: return 64 * MiB;
        default: return 1 * MiB;
    }
}

namespace {

class Checker {
public:
    Checker(const OperationQueue& q, const ValidationContext& c) : q_(q), c_(c) {}

    ValidationResult run() {
        global();
        perOperation();
        finalLayout();
        return std::move(r_);
    }

private:
    void add(Severity s, const char* code, std::string msg) { r_.diagnostics.push_back({s, code, std::move(msg)}); }
    // Gaps of this build: warnings while planning, errors for execution.
    Severity gapSeverity() const { return c_.mode == ValidationMode::Planning ? Severity::Warning : Severity::Error; }

    bool isOriginal(std::uint32_t id) const { return id >= 1 && id <= q_.original().partitions.size(); }
    PartitionState stateOf(std::uint32_t id) const {
        auto it = c_.state.find(id);
        return it == c_.state.end() ? PartitionState{} : it->second;
    }

    void global() {
        if (q_.empty()) return;
        bool createsTable = false;
        for (const auto& op : q_.operations())
            if (op.type == OpType::CreateTable) createsTable = true;
        if (q_.original().hasErrors())
            add(createsTable ? Severity::Warning : Severity::Error, "plan.source_errors",
                "The current partition table has errors. Partitions cannot be changed until they are resolved; "
                "only creating a new partition table is possible.");
        if (c_.deviceWriteProtected) add(Severity::Error, "device.write_protected", "The device is write protected.");
        if (c_.managedByWindows)
            add(Severity::Error, "device.managed_by_windows",
                "This disk is a dynamic disk or Storage Spaces member. Use Disk Management or Storage Spaces instead.");
        if (c_.mode == ValidationMode::Execution) {
            if (c_.haveCurrentIdentity) {
                std::string why;
                if (!q_.identity().matches(c_.currentIdentity, why))
                    add(Severity::Error, "plan.identity_mismatch",
                        "The plan was made for a different disk or the disk changed since planning: " + why + ".");
            } else {
                add(Severity::Error, "plan.identity_unknown", "The current identity of the device could not be determined.");
            }
            add(Severity::Error, "execution.unavailable",
                "This build of WParted cannot execute operations yet. The plan was validated only.");
        }
    }

    void perOperation() {
        DiskLayout layout = q_.original();
        std::uint32_t next = static_cast<std::uint32_t>(layout.partitions.size()) + 1;
        std::string err;
        int index = 0;
        for (Operation op : q_.operations()) {
            ++index;
            const std::string where = "Operation " + std::to_string(index) + ": ";
            const PartitionEntry* t = findPlanned(layout, op.target);
            if (op.type == OpType::CreateTable) {
                for (const auto& p : q_.original().partitions) protect(where, p, op, layout);
            } else if (t) {
                protect(where, *t, op, layout);
                capability(where, *t, op, layout);
                if (op.type == OpType::ResizeMove) {
                    std::uint64_t used = stateOf(op.target).usedBytes;
                    if (used && op.sectorCount * layout.sectorSize < used)
                        add(Severity::Error, "op.shrink_below_used",
                            where + "the new size is smaller than the space in use (" + formatSize(used) + ").");
                }
            }
            if (op.type == OpType::CreatePartition && op.fs != FsType::Unknown) {
                const auto& cap = capabilitiesFor(op.fs);
                if (cap.create != Support::Supported && cap.create != Support::Limited)
                    add(gapSeverity(), "op.capability",
                        where + "creating " + fsTypeName(op.fs) + " file systems is " + supportName(cap.create) +
                            " in this build.");
            }
            if (op.type == OpType::CreatePartition || op.type == OpType::ResizeMove || op.type == OpType::Copy)
                touched_.insert(op.type == OpType::ResizeMove ? op.target : 0);
            if (!simulate(op, layout, next, err)) {
                add(Severity::Error, "op.invalid", where + err);
                return;
            }
            if (op.result) touched_.insert(op.result);
        }
    }

    void protect(const std::string& where, const PartitionEntry& p, const Operation& op, const DiskLayout& layout) {
        if (!isOriginal(p.planId)) return;
        const PartitionState st = stateOf(p.planId);
        const bool destructive = op.type == OpType::CreateTable || op.type == OpType::DeletePartition ||
                                 op.type == OpType::ResizeMove || op.type == OpType::Format ||
                                 op.type == OpType::SetFlags || op.type == OpType::NewUuid;
        if (!destructive) return;
        const std::string what = "partition #" + std::to_string(p.number);
        if (st.containsWindows || st.pageFile)
            add(Severity::Error, "op.system_critical",
                where + what + " holds the running Windows or its page file and cannot be changed from Windows.");
        else if (isSystemCriticalType(layout, p))
            add(Severity::Error, "op.system_critical",
                where + what + " is a system partition (" + partitionTypeDescription(layout, p) +
                    ") and is protected in desktop mode.");
        if (st.mounted && op.type != OpType::SetFlags)
            add(gapSeverity(), "op.busy", where + what + " is mounted; it must be unmounted before execution.");
        if (p.fs.type == FsType::BitLocker &&
            (op.type == OpType::ResizeMove || op.type == OpType::Copy || op.type == OpType::NewUuid))
            add(Severity::Error, "op.encrypted", where + what + " is BitLocker encrypted; WParted never changes encrypted volumes.");
        if (p.fs.type == FsType::BitLocker && op.type == OpType::Format)
            add(Severity::Warning, "op.encrypted_format", where + "formatting destroys the encrypted data of " + what + ".");
    }

    void capability(const std::string& where, const PartitionEntry& p, const Operation& op, const DiskLayout& layout) {
        if (p.kind == PartitionKind::Extended) return;
        const FsType fs = p.fs.type;
        const auto& cap = capabilitiesFor(fs);
        auto need = [&](Support s, const char* action) {
            if (s == Support::Supported || s == Support::Limited) return;
            add(gapSeverity(), "op.capability",
                where + std::string(action) + " " + fsTypeName(fs) + " is " + supportName(s) + " in this build.");
        };
        switch (op.type) {
            case OpType::ResizeMove: {
                if (fs == FsType::Unknown) return;  // no file system: partition boundaries only
                if (op.sectorCount > p.sectorCount) need(cap.grow, "growing");
                if (op.sectorCount < p.sectorCount) need(cap.shrink, "shrinking");
                if (op.firstLba != p.firstLba) need(cap.move, "moving");
                (void)layout;
                return;
            }
            case OpType::Copy: if (fs != FsType::Unknown) need(cap.copy, "copying"); return;
            case OpType::Check: need(cap.check, "checking"); return;
            case OpType::SetLabel: need(cap.label, "labelling"); return;
            case OpType::NewUuid: need(cap.uuid, "changing the UUID of"); return;
            case OpType::Format:
                if (op.fs != FsType::Unknown) {
                    const auto& target = capabilitiesFor(op.fs);
                    if (target.create != Support::Supported && target.create != Support::Limited)
                        add(gapSeverity(), "op.capability",
                            where + std::string("creating ") + fsTypeName(op.fs) + " file systems is " +
                                supportName(target.create) + " in this build.");
                }
                return;
            default: return;
        }
    }

    void finalLayout() {
        const DiskLayout& l = q_.current();
        if (q_.empty() || (l.type != TableType::Gpt && l.type != TableType::Mbr)) return;
        const std::uint32_t ss = l.sectorSize;
        std::vector<const PartitionEntry*> top, logical;
        const PartitionEntry* ext = nullptr;
        int extendedCount = 0, bootable = 0;
        for (const auto& p : l.partitions) {
            (p.kind == PartitionKind::Logical ? logical : top).push_back(&p);
            if (p.kind == PartitionKind::Extended) { ext = &p; ++extendedCount; }
            if (p.mbrBootable) ++bootable;
            const std::string what = "Partition #" + std::to_string(p.number);
            if (p.firstLba < l.firstUsableLba || p.lastLba() > l.lastUsableLba || p.sectorCount == 0)
                add(Severity::Error, "layout.out_of_bounds", what + " lies outside the usable area of the disk.");
            if (l.type == TableType::Mbr && (p.firstLba > 0xFFFFFFFFull || p.sectorCount > 0xFFFFFFFFull))
                add(Severity::Error, "mbr.limit_2tib",
                    what + " exceeds the 2 TiB addressing limit of msdos partition tables (use GPT).");
            if (!touched_.count(p.planId)) continue;
            if (p.kind != PartitionKind::Extended && p.fs.type != FsType::Unknown &&
                p.sectorCount * ss < minimumSize(p.fs.type))
                add(Severity::Error, "op.too_small",
                    what + " is smaller than the minimum for " + fsTypeName(p.fs.type) + " (" +
                        formatSize(minimumSize(p.fs.type)) + ").");
            const std::uint64_t start = p.firstLba * ss;
            if (c_.alignmentBytes && start % c_.alignmentBytes != 0)
                add(Severity::Warning, "layout.unaligned", what + " does not start on a " + formatSize(c_.alignmentBytes) + " boundary.");
            if (c_.physicalSectorSize > ss && start % c_.physicalSectorSize != 0)
                add(Severity::Warning, "layout.physical_unaligned",
                    what + " does not start on a physical sector boundary; this reduces performance.");
        }
        overlaps(top, "partitions");
        overlaps(logical, "logical partitions");
        if (l.type == TableType::Mbr) {
            if (top.size() > 4) add(Severity::Error, "layout.too_many_primary", "An msdos table holds at most four primary/extended partitions.");
            if (extendedCount > 1) add(Severity::Error, "layout.multiple_extended", "Only one extended partition is allowed.");
            if (!logical.empty() && !ext) add(Severity::Error, "layout.logical_without_extended", "Logical partitions need an extended partition.");
            if (bootable > 1) add(Severity::Warning, "mbr.multiple_bootable", "More than one partition is marked bootable.");
            if (ext) {
                std::vector<const PartitionEntry*> sorted = logical;
                std::sort(sorted.begin(), sorted.end(), [](auto a, auto b) { return a->firstLba < b->firstLba; });
                std::uint64_t prevEnd = ext->firstLba;  // the first EBR occupies the first sector
                bool first = true;
                for (const auto* lp : sorted) {
                    const std::string what = "Logical partition #" + std::to_string(lp->number);
                    if (lp->firstLba < ext->firstLba || lp->lastLba() > ext->lastLba())
                        add(Severity::Error, "layout.logical_outside_extended", what + " is not inside the extended partition.");
                    // Each logical partition needs one free sector in front of it for its EBR.
                    const std::uint64_t ebr = first ? ext->firstLba : lp->firstLba - 1;
                    if (lp->firstLba == 0 || lp->firstLba <= ebr || (!first && ebr <= prevEnd))
                        add(Severity::Error, "layout.no_room_for_ebr", what + " leaves no room for its extended boot record.");
                    prevEnd = lp->lastLba();
                    first = false;
                }
            }
        }
    }

    void overlaps(std::vector<const PartitionEntry*> v, const char* what) {
        std::sort(v.begin(), v.end(), [](auto a, auto b) { return a->firstLba < b->firstLba; });
        for (std::size_t i = 1; i < v.size(); ++i)
            if (v[i]->firstLba <= v[i - 1]->lastLba())
                add(Severity::Error, "layout.overlap",
                    std::string("The ") + what + " #" + std::to_string(v[i - 1]->number) + " and #" +
                        std::to_string(v[i]->number) + " overlap.");
    }

    const OperationQueue& q_;
    const ValidationContext& c_;
    ValidationResult r_;
    std::set<std::uint32_t> touched_;
};

}  // namespace

ValidationResult validatePlan(const OperationQueue& queue, const ValidationContext& ctx) {
    return Checker(queue, ctx).run();
}

}  // namespace wparted
