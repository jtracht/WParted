// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#include "wparted/core/validation.hpp"

#include "wparted/core/fs_tools.hpp"

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

std::uint64_t maximumSize(FsType t, std::uint32_t sectorSize) noexcept {
    switch (t) {
        case FsType::Fat16: return 4ull * 1024 * 1024 * 1024;               // 65524 clusters of 64 KiB
        case FsType::Fat32: return 0xFFFFFFFFull * sectorSize;              // 32-bit sector count
        default: return 0;                                                   // no limit checked
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
    // `op` is the 1-based operation number (0 = plan/layout wide); `args` are
    // the message parameters for translations (see the comment at each code).
    void add(Severity s, const char* code, std::string msg, int op = 0, std::vector<std::string> args = {}) {
        r_.diagnostics.emplace_back(s, code, std::move(msg), op, std::move(args));
    }
    static std::string where(int op) { return "Operation " + std::to_string(op) + ": "; }
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
        if (c_.mode != ValidationMode::Planning) {
            if (c_.haveCurrentIdentity) {
                std::string why;
                if (!q_.identity().matches(c_.currentIdentity, why))
                    add(Severity::Error, "plan.identity_mismatch",
                        "The plan was made for a different disk or the disk changed since planning: " + why + ".", 0,
                        {why});
            } else {
                add(Severity::Error, "plan.identity_unknown", "The current identity of the device could not be determined.");
            }
            if (c_.mode == ValidationMode::Execution)
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
            const PartitionEntry* t = findPlanned(layout, op.target);
            if (op.type == OpType::CreateTable) {
                for (const auto& p : q_.original().partitions) protect(index, p, op, layout);
            } else if (t) {
                protect(index, *t, op, layout);
                capability(index, *t, op, layout);
                if (op.type == OpType::ResizeMove) {
                    std::uint64_t used = stateOf(op.target).usedBytes;
                    if (used && op.sectorCount * layout.sectorSize < used)
                        add(Severity::Error, "op.shrink_below_used",
                            where(index) + "the new size is smaller than the space in use (" + formatSize(used) + ").",
                            index, {formatSize(used)});
                }
            }
            if (op.type == OpType::CreatePartition && op.fs != FsType::Unknown) {
                const auto& cap = capabilitiesFor(op.fs);
                if (cap.create != Support::Supported && cap.create != Support::Limited)
                    add(gapSeverity(), "op.capability",
                        where(index) + "creating " + fsTypeName(op.fs) + " file systems is " + supportName(cap.create) +
                            " in this build.",
                        index, {"create", fsTypeName(op.fs), supportName(cap.create)});
            }
            // op.label_invalid args: {reason}
            {
                const bool labelled = op.type == OpType::SetLabel ||
                                      ((op.type == OpType::CreatePartition || op.type == OpType::Format) &&
                                       op.fs != FsType::Unknown && !op.label.empty());
                const FsType lfs = op.type == OpType::SetLabel ? (t ? t->fs.type : FsType::Unknown) : op.fs;
                if (labelled && fstools::canLabel(lfs)) {
                    const std::string why = fstools::labelProblem(lfs, op.label);
                    if (!why.empty()) add(Severity::Error, "op.label_invalid", where(index) + "invalid label: " + why + ".", index, {why});
                }
            }
            if (op.type == OpType::CreatePartition || op.type == OpType::ResizeMove || op.type == OpType::Copy ||
                op.type == OpType::Format)
                touched_.insert(op.type == OpType::ResizeMove || op.type == OpType::Format ? op.target : 0);
            if (!simulate(op, layout, next, err)) {
                add(Severity::Error, "op.invalid", where(index) + err, index, {err});
                return;
            }
            if (op.result) touched_.insert(op.result);
        }
    }

    void protect(int index, const PartitionEntry& p, const Operation& op, const DiskLayout& layout) {
        if (!isOriginal(p.planId)) return;
        const PartitionState st = stateOf(p.planId);
        const bool destructive = op.type == OpType::CreateTable || op.type == OpType::DeletePartition ||
                                 op.type == OpType::ResizeMove || op.type == OpType::Format ||
                                 op.type == OpType::SetFlags || op.type == OpType::NewUuid;
        if (!destructive) return;
        const std::string num = std::to_string(p.number);
        const std::string what = "partition #" + num, at = where(index);
        // op.system_critical args: {"windows", number} or {"type", number, type description}
        if (st.containsWindows || st.pageFile)
            add(Severity::Error, "op.system_critical",
                at + what + " holds the running Windows or its page file and cannot be changed from Windows.", index,
                {"windows", num});
        else if (isSystemCriticalType(layout, p))
            add(c_.mode == ValidationMode::ImageExecution ? Severity::Warning : Severity::Error, "op.system_critical",
                at + what + " is a system partition (" + partitionTypeDescription(layout, p) +
                    ") and is protected in desktop mode.",
                index, {"type", num, partitionTypeDescription(layout, p)});
        if (st.mounted && op.type != OpType::SetFlags)  // args: {number}
            add(gapSeverity(), "op.busy", at + what + " is mounted; it must be unmounted before execution.", index, {num});
        if (p.fs.type == FsType::BitLocker &&
            (op.type == OpType::ResizeMove || op.type == OpType::Copy || op.type == OpType::NewUuid))
            add(Severity::Error, "op.encrypted", at + what + " is BitLocker encrypted; WParted never changes encrypted volumes.",
                index, {num});
        if (p.fs.type == FsType::BitLocker && op.type == OpType::Format)
            add(Severity::Warning, "op.encrypted_format", at + "formatting destroys the encrypted data of " + what + ".",
                index, {num});
    }

    // op.capability args: {action, file system, support level}; action is one of
    // create, grow, shrink, move, copy, check, label, uuid.
    void capability(int index, const PartitionEntry& p, const Operation& op, const DiskLayout& layout) {
        // An extended partition only changes its boundaries (the logical
        // partitions stay where they are; layout checks keep them inside).
        if (p.kind == PartitionKind::Extended) return;
        const FsType fs = p.fs.type;
        const auto& cap = capabilitiesFor(fs);
        auto need = [&](Support s, const char* key, const char* action) {
            if (s == Support::Supported || s == Support::Limited) return;
            add(gapSeverity(), "op.capability",
                where(index) + std::string(action) + " " + fsTypeName(fs) + " is " + supportName(s) + " in this build.",
                index, {key, fsTypeName(fs), supportName(s)});
        };
        switch (op.type) {
            case OpType::ResizeMove: {
                if (fs == FsType::Unknown) return;  // no file system: partition boundaries only
                if (op.sectorCount > p.sectorCount) need(cap.grow, "grow", "growing");
                if (op.sectorCount < p.sectorCount) need(cap.shrink, "shrink", "shrinking");
                // A move is a raw block move ("Limited" in the capability table).
                if (op.firstLba != p.firstLba) need(cap.move, "move", "moving");
                (void)layout;
                return;
            }
            case OpType::Copy:  // raw block copy; not offered for LUKS, LVM, RAID, ...
                if (fs != FsType::Unknown) need(cap.copy, "copy", "copying");
                return;
            case OpType::Check: need(cap.check, "check", "checking"); return;
            case OpType::SetLabel: need(cap.label, "label", "labelling"); return;
            case OpType::NewUuid: need(cap.uuid, "uuid", "changing the UUID of"); return;
            case OpType::Format:
                if (op.fs != FsType::Unknown) {
                    const auto& target = capabilitiesFor(op.fs);
                    if (target.create != Support::Supported && target.create != Support::Limited)
                        add(gapSeverity(), "op.capability",
                            where(index) + std::string("creating ") + fsTypeName(op.fs) + " file systems is " +
                                supportName(target.create) + " in this build.",
                            index, {"create", fsTypeName(op.fs), supportName(target.create)});
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
            const std::string num = std::to_string(p.number);
            const std::string what = "Partition #" + num;
            // Layout diagnostics: args start with the partition number.
            if (p.firstLba < l.firstUsableLba || p.lastLba() > l.lastUsableLba || p.sectorCount == 0)
                add(Severity::Error, "layout.out_of_bounds", what + " lies outside the usable area of the disk.", 0, {num});
            if (l.type == TableType::Mbr && (p.firstLba > 0xFFFFFFFFull || p.sectorCount > 0xFFFFFFFFull))
                add(Severity::Error, "mbr.limit_2tib",
                    what + " exceeds the 2 TiB addressing limit of msdos partition tables (use GPT).", 0, {num});
            if (!touched_.count(p.planId)) continue;
            if (p.kind != PartitionKind::Extended && p.fs.type != FsType::Unknown &&
                p.sectorCount * ss < minimumSize(p.fs.type))
                add(Severity::Error, "op.too_small",
                    what + " is smaller than the minimum for " + fsTypeName(p.fs.type) + " (" +
                        formatSize(minimumSize(p.fs.type)) + ").",
                    0, {num, fsTypeName(p.fs.type), formatSize(minimumSize(p.fs.type))});
            if (p.kind != PartitionKind::Extended && maximumSize(p.fs.type, ss) &&
                p.sectorCount * ss > maximumSize(p.fs.type, ss))  // args: {number, file system, maximum}
                add(Severity::Error, "op.too_large",
                    what + " is larger than " + fsTypeName(p.fs.type) + " allows (" +
                        formatSize(maximumSize(p.fs.type, ss)) + ").",
                    0, {num, fsTypeName(p.fs.type), formatSize(maximumSize(p.fs.type, ss))});
            const std::uint64_t start = p.firstLba * ss;
            if (c_.alignmentBytes && start % c_.alignmentBytes != 0)
                add(Severity::Warning, "layout.unaligned", what + " does not start on a " + formatSize(c_.alignmentBytes) + " boundary.",
                    0, {num, formatSize(c_.alignmentBytes)});
            if (c_.physicalSectorSize > ss && start % c_.physicalSectorSize != 0)
                add(Severity::Warning, "layout.physical_unaligned",
                    what + " does not start on a physical sector boundary; this reduces performance.", 0, {num});
        }
        overlaps(top, "partitions", "primary");
        overlaps(logical, "logical partitions", "logical");
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
                    const std::string lnum = std::to_string(lp->number);
                    const std::string what = "Logical partition #" + lnum;
                    if (lp->firstLba < ext->firstLba || lp->lastLba() > ext->lastLba())
                        add(Severity::Error, "layout.logical_outside_extended", what + " is not inside the extended partition.",
                            0, {lnum});
                    // Each logical partition needs one free sector in front of it for its EBR.
                    const std::uint64_t ebr = first ? ext->firstLba : lp->firstLba - 1;
                    if (lp->firstLba == 0 || lp->firstLba <= ebr || (!first && ebr <= prevEnd))
                        add(Severity::Error, "layout.no_room_for_ebr", what + " leaves no room for its extended boot record.",
                            0, {lnum});
                    prevEnd = lp->lastLba();
                    first = false;
                }
            }
        }
    }

    // layout.overlap args: {"primary" | "logical", first number, second number}
    void overlaps(std::vector<const PartitionEntry*> v, const char* what, const char* key) {
        std::sort(v.begin(), v.end(), [](auto a, auto b) { return a->firstLba < b->firstLba; });
        for (std::size_t i = 1; i < v.size(); ++i)
            if (v[i]->firstLba <= v[i - 1]->lastLba())
                add(Severity::Error, "layout.overlap",
                    std::string("The ") + what + " #" + std::to_string(v[i - 1]->number) + " and #" +
                        std::to_string(v[i]->number) + " overlap.",
                    0, {key, std::to_string(v[i - 1]->number), std::to_string(v[i]->number)});
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
