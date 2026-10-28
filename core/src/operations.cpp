// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#include "wparted/core/operations.hpp"

#include <algorithm>
#include <cstdio>

#include "wparted/core/json.hpp"

namespace wparted {

// ---------------------------------------------------------------------------
// Identity

bool DiskIdentity::matches(const DiskIdentity& o, std::string& why) const {
    if (serial != o.serial) { why = "serial number differs"; return false; }
    if (sizeBytes != o.sizeBytes) { why = "device size differs"; return false; }
    if (sectorSize != o.sectorSize) { why = "logical sector size differs"; return false; }
    if (table != o.table) { why = "partition table type differs"; return false; }
    if (table == TableType::Gpt && diskGuid != o.diskGuid) { why = "GPT disk GUID differs"; return false; }
    if (table == TableType::Mbr && mbrSignature != o.mbrSignature) { why = "MBR disk signature differs"; return false; }
    return true;
}

DiskIdentity identityOf(const DiskLayout& l, const std::string& serial, const std::string& model) {
    DiskIdentity id;
    id.serial = serial;
    id.model = model;
    id.sizeBytes = l.diskBytes;
    id.sectorSize = l.sectorSize;
    id.table = l.type;
    id.diskGuid = l.diskGuid;
    id.mbrSignature = l.mbrSignature;
    return id;
}

// ---------------------------------------------------------------------------
// Names

namespace {

struct OpName {
    OpType t;
    const char* n;
};
const OpName kOpNames[] = {
    {OpType::CreateTable, "create-table"}, {OpType::CreatePartition, "create-partition"},
    {OpType::DeletePartition, "delete-partition"}, {OpType::ResizeMove, "resize-move"}, {OpType::Copy, "copy"},
    {OpType::Format, "format"}, {OpType::Check, "check"}, {OpType::SetLabel, "set-label"},
    {OpType::SetName, "set-name"}, {OpType::SetFlags, "set-flags"}, {OpType::NewUuid, "new-uuid"},
};

bool fsFromName(const std::string& n, FsType& out) {
    if (n == fsTypeName(FsType::Unknown)) { out = FsType::Unknown; return true; }
    for (FsType t : allFsTypes())
        if (n == fsTypeName(t)) { out = t; return true; }
    return false;
}

bool tableFromName(const std::string& n, TableType& out) {
    if (n == "gpt") { out = TableType::Gpt; return true; }
    if (n == "msdos") { out = TableType::Mbr; return true; }
    return false;
}

// Identity of the disk the plan was made for: also a blank disk ("none").
bool identityTableFromName(const std::string& n, TableType& out) {
    for (TableType t : {TableType::None, TableType::Mbr, TableType::Gpt, TableType::Unknown})
        if (n == tableTypeName(t)) { out = t; return true; }
    return false;
}

bool kindFromName(const std::string& n, PartitionKind& out) {
    for (PartitionKind k : {PartitionKind::Primary, PartitionKind::Extended, PartitionKind::Logical})
        if (n == partitionKindName(k)) { out = k; return true; }
    return false;
}

// GPT type GUIDs used for flags and defaults.
const char* const kEsp = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B";
const char* const kMsr = "E3C9E316-0B5C-4DB8-817D-F92DF00215AE";
const char* const kBasic = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7";
const char* const kWinRe = "DE94BBA4-06D1-4D40-A16A-BFD50179D6AC";
const char* const kLinuxFs = "0FC63DAF-8483-4772-8E79-3D69D8477DE4";
const char* const kLinuxHome = "933AC7E1-2EB4-4F13-B844-0E14E2AEF915";
const char* const kSwap = "0657FD6D-A4AB-43C4-84E5-0933C84B4F4F";
const char* const kLvm = "E6D6D379-F507-44C2-A23C-238F2A3DF928";
const char* const kRaid = "A19D880F-05FC-4D3B-A006-743F0F84911E";
const char* const kBiosBoot = "21686148-6449-6E6F-744E-656564454649";
const char* const kLuks = "CA7D7CCB-63ED-4C53-861C-1742536059CC";
const char* const kHfs = "48465300-0000-11AA-AA11-00306543ECAC";

Guid guid(const char* text) {
    Guid g;
    Guid::parse(text, g);
    return g;
}

bool isWindowsFs(FsType t) {
    return t == FsType::Ntfs || t == FsType::Fat12 || t == FsType::Fat16 || t == FsType::Fat32 || t == FsType::ExFat ||
           t == FsType::ReFs || t == FsType::Udf || t == FsType::BitLocker;
}

std::string partText(const DiskLayout& l, const PartitionEntry& p) {
    std::string s = p.kind == PartitionKind::Extended ? "extended partition" : "partition";
    s += " #" + std::to_string(p.number) + " (";
    s += p.kind == PartitionKind::Extended ? "extended" : fsTypeName(p.fs.type);
    s += ", " + formatSize(p.sectorCount * l.sectorSize) + ")";
    return s;
}

PartitionEntry* findMutable(DiskLayout& l, std::uint32_t id) {
    for (auto& p : l.partitions)
        if (p.planId == id) return &p;
    return nullptr;
}

void sortPartitions(DiskLayout& l) {
    std::stable_sort(l.partitions.begin(), l.partitions.end(),
                     [](const PartitionEntry& a, const PartitionEntry& b) { return a.number < b.number; });
}

void renumberLogicals(DiskLayout& l) {
    std::vector<PartitionEntry*> logicals;
    for (auto& p : l.partitions)
        if (p.kind == PartitionKind::Logical) logicals.push_back(&p);
    std::sort(logicals.begin(), logicals.end(), [](auto a, auto b) { return a->firstLba < b->firstLba; });
    int n = 5;
    for (auto* p : logicals) p->number = n++;
    sortPartitions(l);
}

int nextNumber(const DiskLayout& l, PartitionKind kind) {
    auto used = [&](int n) {
        for (const auto& p : l.partitions)
            if (p.number == n) return true;
        return false;
    };
    if (l.type == TableType::Gpt) {
        for (int n = 1; n <= 128; ++n)
            if (!used(n)) return n;
        return 0;
    }
    if (kind == PartitionKind::Logical) {
        int maxLogical = 4;
        for (const auto& p : l.partitions)
            if (p.kind == PartitionKind::Logical) maxLogical = std::max(maxLogical, p.number);
        return maxLogical + 1;
    }
    for (int n = 1; n <= 4; ++n)
        if (!used(n)) return n;
    return 0;
}

std::uint8_t hiddenVariant(std::uint8_t t, bool hidden) {
    static const std::uint8_t pairs[][2] = {{0x01, 0x11}, {0x04, 0x14}, {0x06, 0x16}, {0x07, 0x17},
                                            {0x0B, 0x1B}, {0x0C, 0x1C}, {0x0E, 0x1E}};
    for (const auto& pr : pairs) {
        if (hidden && t == pr[0]) return pr[1];
        if (!hidden && t == pr[1]) return pr[0];
    }
    return t;
}

std::uint8_t lbaVariant(std::uint8_t t, bool lba) {
    static const std::uint8_t pairs[][2] = {{0x0B, 0x0C}, {0x06, 0x0E}, {0x05, 0x0F}, {0x1B, 0x1C}, {0x16, 0x1E}};
    for (const auto& pr : pairs) {
        if (lba && t == pr[0]) return pr[1];
        if (!lba && t == pr[1]) return pr[0];
    }
    return t;
}

bool isGenericType(TableType t, const PartitionEntry& p) {
    if (t == TableType::Gpt)
        return p.typeGuid.isZero() || p.typeGuid == guid(kBasic) || p.typeGuid == guid(kLinuxFs) ||
               p.typeGuid == guid(kSwap);
    switch (p.mbrType) {
        case 0x00: case 0x01: case 0x04: case 0x06: case 0x07: case 0x0B: case 0x0C: case 0x0E: case 0x82: case 0x83:
            return true;
        default:
            return false;
    }
}

}  // namespace

const char* opTypeName(OpType t) noexcept {
    for (const auto& n : kOpNames)
        if (n.t == t) return n.n;
    return "unknown";
}

bool opTypeFromName(const std::string& name, OpType& out) {
    for (const auto& n : kOpNames)
        if (name == n.n) { out = n.t; return true; }
    return false;
}

const char* riskName(Risk r) noexcept {
    switch (r) {
        case Risk::Low: return "low";
        case Risk::Medium: return "medium";
        case Risk::High: return "high";
        case Risk::Critical: return "critical";
    }
    return "high";
}

Risk riskOf(const Operation& op) noexcept {
    switch (op.type) {
        case OpType::CreateTable: return Risk::Critical;
        case OpType::ResizeMove: return Risk::Critical;
        case OpType::DeletePartition:
        case OpType::Format:
        case OpType::Copy: return Risk::High;
        case OpType::CreatePartition:
        case OpType::SetFlags:
        case OpType::NewUuid:
        case OpType::Check: return Risk::Medium;
        case OpType::SetLabel:
        case OpType::SetName: return Risk::Low;
    }
    return Risk::High;
}

const PartitionEntry* findPlanned(const DiskLayout& layout, std::uint32_t id) {
    for (const auto& p : layout.partitions)
        if (p.planId == id) return &p;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Flags

std::vector<std::string> settableFlags(TableType t) {
    if (t == TableType::Gpt)
        return {"boot", "esp", "msftres", "msftdata", "diag", "bios_grub", "lvm", "raid", "swap", "linux-home",
                "legacy_boot", "hidden", "no_automount", "required"};
    if (t == TableType::Mbr) return {"boot", "hidden", "lba", "lvm", "raid", "swap", "diag", "esp"};
    return {};
}

void applyDefaultType(TableType t, PartitionEntry& p, FsType fs) {
    if (t == TableType::Gpt) {
        if (isWindowsFs(fs)) p.typeGuid = guid(kBasic);
        else if (fs == FsType::LinuxSwap) p.typeGuid = guid(kSwap);
        else if (fs == FsType::Lvm2Pv) p.typeGuid = guid(kLvm);
        else if (fs == FsType::LinuxRaid) p.typeGuid = guid(kRaid);
        else if (fs == FsType::Luks) p.typeGuid = guid(kLuks);
        else if (fs == FsType::HfsPlus || fs == FsType::Hfs) p.typeGuid = guid(kHfs);
        else if (fs == FsType::Unknown) p.typeGuid = guid(kBasic);
        else p.typeGuid = guid(kLinuxFs);
        return;
    }
    if (p.kind == PartitionKind::Extended) { p.mbrType = 0x0F; return; }
    switch (fs) {
        case FsType::Ntfs: case FsType::ExFat: case FsType::ReFs: p.mbrType = 0x07; break;
        case FsType::Fat32: p.mbrType = 0x0C; break;
        case FsType::Fat16: p.mbrType = 0x0E; break;
        case FsType::Fat12: p.mbrType = 0x01; break;
        case FsType::LinuxSwap: p.mbrType = 0x82; break;
        case FsType::Lvm2Pv: p.mbrType = 0x8E; break;
        case FsType::LinuxRaid: p.mbrType = 0xFD; break;
        case FsType::HfsPlus: case FsType::Hfs: p.mbrType = 0xAF; break;
        case FsType::Unknown: p.mbrType = 0x83; break;
        default: p.mbrType = isWindowsFs(fs) ? 0x07 : 0x83; break;
    }
}

bool setPartitionFlag(TableType t, PartitionEntry& p, const std::string& flag, bool on, std::string& error) {
    auto known = settableFlags(t);
    if (std::find(known.begin(), known.end(), flag) == known.end()) {
        error = "flag '" + flag + "' is not available on " + tableTypeName(t) + " partition tables";
        return false;
    }
    if (p.kind == PartitionKind::Extended && flag != "lba") {
        error = "flags cannot be set on an extended partition";
        return false;
    }
    if (t == TableType::Gpt) {
        struct TypeFlag { const char* flag; const char* guid; };
        static const TypeFlag typeFlags[] = {{"boot", kEsp}, {"esp", kEsp}, {"msftres", kMsr}, {"msftdata", kBasic},
                                             {"diag", kWinRe}, {"bios_grub", kBiosBoot}, {"lvm", kLvm},
                                             {"raid", kRaid}, {"swap", kSwap}, {"linux-home", kLinuxHome}};
        for (const auto& tf : typeFlags) {
            if (flag != tf.flag) continue;
            if (on) p.typeGuid = guid(tf.guid);
            else if (p.typeGuid == guid(tf.guid)) applyDefaultType(t, p, p.fs.type);
            return true;
        }
        int bit = flag == "required" ? 0 : flag == "legacy_boot" ? 2 : flag == "hidden" ? 62 : 63;
        if (on) p.gptAttributes |= (1ull << bit);
        else p.gptAttributes &= ~(1ull << bit);
        return true;
    }
    // MBR
    if (flag == "boot") { p.mbrBootable = on; return true; }
    if (flag == "hidden") { p.mbrType = hiddenVariant(p.mbrType, on); return true; }
    if (flag == "lba") { p.mbrType = lbaVariant(p.mbrType, on); return true; }
    struct TypeFlag { const char* flag; std::uint8_t type; };
    static const TypeFlag typeFlags[] = {{"lvm", 0x8E}, {"raid", 0xFD}, {"swap", 0x82}, {"diag", 0x27}, {"esp", 0xEF}};
    for (const auto& tf : typeFlags) {
        if (flag != tf.flag) continue;
        if (on) p.mbrType = tf.type;
        else if (p.mbrType == tf.type) applyDefaultType(t, p, p.fs.type);
        return true;
    }
    error = "unhandled flag '" + flag + "'";
    return false;
}

// ---------------------------------------------------------------------------
// Simulation

bool simulate(Operation& op, DiskLayout& l, std::uint32_t& nextPlanId, std::string& error) {
    auto needTable = [&]() {
        if (l.type == TableType::Gpt || l.type == TableType::Mbr) return true;
        error = "the device has no partition table that WParted can edit; create one first";
        return false;
    };
    auto assignId = [&]() {
        if (op.result == 0) op.result = nextPlanId++;
        else nextPlanId = std::max(nextPlanId, op.result + 1);
        return op.result;
    };

    if (op.type == OpType::CreateTable) {
        if (op.table != TableType::Gpt && op.table != TableType::Mbr) {
            error = "only gpt and msdos partition tables can be created";
            return false;
        }
        const std::uint64_t total = l.totalSectors();
        const std::uint64_t arraySectors = (128ull * 128 + l.sectorSize - 1) / l.sectorSize;
        if (total < 2 * arraySectors + 8) {
            error = "device too small for a partition table";
            return false;
        }
        l.type = op.table;
        l.partitions.clear();
        l.diagnostics.clear();
        l.diskGuid = Guid{};
        l.mbrSignature = 0;
        if (op.table == TableType::Gpt) {
            l.firstUsableLba = 2 + arraySectors;
            l.lastUsableLba = total - 2 - arraySectors;
            l.gptPrimaryValid = l.gptBackupValid = true;
        } else {
            l.firstUsableLba = 1;
            l.lastUsableLba = total - 1;
        }
        return true;
    }

    if (op.type == OpType::CreatePartition) {
        if (!needTable()) return false;
        if (op.sectorCount == 0) { error = "partition size must not be zero"; return false; }
        if (l.type == TableType::Gpt && op.kind != PartitionKind::Primary) {
            error = "GPT has no extended or logical partitions";
            return false;
        }
        if (op.kind == PartitionKind::Extended && op.fs != FsType::Unknown) {
            error = "an extended partition cannot contain a file system";
            return false;
        }
        PartitionEntry p;
        p.kind = op.kind;
        p.number = nextNumber(l, op.kind);
        if (p.number == 0) {
            error = l.type == TableType::Gpt ? "all 128 GPT entries are in use" : "all four primary entries are in use";
            return false;
        }
        p.firstLba = op.firstLba;
        p.sectorCount = op.sectorCount;
        p.fs.type = op.kind == PartitionKind::Extended ? FsType::Unknown : op.fs;
        p.fs.label = op.label;
        if (l.type == TableType::Gpt) p.name = op.name;
        applyDefaultType(l.type, p, p.fs.type);
        p.planId = assignId();
        l.partitions.push_back(p);
        sortPartitions(l);
        return true;
    }

    PartitionEntry* t = findMutable(l, op.target);
    if (!t) {
        error = "the target partition does not exist in the planned layout";
        return false;
    }
    if (t->kind == PartitionKind::WholeDevice) {
        error = "the device has no partition table; whole-device file systems cannot be changed here";
        return false;
    }
    const bool extended = t->kind == PartitionKind::Extended;

    switch (op.type) {
        case OpType::DeletePartition: {
            if (extended)
                for (const auto& p : l.partitions)
                    if (p.kind == PartitionKind::Logical) {
                        error = "delete the logical partitions before the extended partition";
                        return false;
                    }
            const bool wasLogical = t->kind == PartitionKind::Logical;
            l.partitions.erase(l.partitions.begin() + (t - l.partitions.data()));
            if (wasLogical) renumberLogicals(l);
            return true;
        }
        case OpType::ResizeMove:
            if (op.sectorCount == 0) { error = "partition size must not be zero"; return false; }
            t->firstLba = op.firstLba;
            t->sectorCount = op.sectorCount;
            if (t->kind == PartitionKind::Logical) renumberLogicals(l);
            return true;
        case OpType::Copy: {
            if (extended) { error = "an extended partition cannot be copied"; return false; }
            if (op.sectorCount < t->sectorCount) { error = "the destination is smaller than the source partition"; return false; }
            if (l.type == TableType::Gpt && op.kind != PartitionKind::Primary) {
                error = "GPT has no logical partitions";
                return false;
            }
            PartitionEntry c = *t;
            c.kind = op.kind;
            c.number = nextNumber(l, op.kind);
            if (c.number == 0) { error = "no free partition entry for the copy"; return false; }
            c.firstLba = op.firstLba;
            c.sectorCount = op.sectorCount;
            c.uniqueGuid = Guid{};
            c.mbrBootable = false;
            c.fs.uuid.clear();
            c.planId = assignId();
            l.partitions.push_back(c);
            if (c.kind == PartitionKind::Logical) renumberLogicals(l);
            else sortPartitions(l);
            return true;
        }
        case OpType::Format:
            if (extended) { error = "an extended partition cannot be formatted"; return false; }
            if (isGenericType(l.type, *t)) applyDefaultType(l.type, *t, op.fs);
            t->fs.type = op.fs;
            t->fs.label = op.label;
            t->fs.uuid.clear();
            return true;
        case OpType::Check:
            if (extended || t->fs.type == FsType::Unknown) { error = "there is no file system to check"; return false; }
            return true;
        case OpType::SetLabel:
            if (extended || t->fs.type == FsType::Unknown) { error = "there is no file system to label"; return false; }
            t->fs.label = op.label;
            return true;
        case OpType::SetName: {
            if (l.type != TableType::Gpt) { error = "partition names exist only on GPT"; return false; }
            std::uint8_t buf[72];
            if (!utf8ToUtf16le(op.name, buf, 36)) { error = "the name is longer than 36 UTF-16 characters"; return false; }
            t->name = op.name;
            return true;
        }
        case OpType::SetFlags:
            for (const auto& f : op.flagsOn)
                if (!setPartitionFlag(l.type, *t, f, true, error)) return false;
            for (const auto& f : op.flagsOff)
                if (!setPartitionFlag(l.type, *t, f, false, error)) return false;
            return true;
        case OpType::NewUuid:
            if (extended || t->fs.type == FsType::Unknown) { error = "there is no file system UUID to change"; return false; }
            t->fs.uuid = "(new)";
            if (l.type == TableType::Gpt) t->uniqueGuid = Guid{};
            return true;
        case OpType::CreateTable:
        case OpType::CreatePartition:
            break;
    }
    error = "unsupported operation";
    return false;
}

std::string describe(const Operation& op, const DiskLayout& before) {
    const PartitionEntry* t = findPlanned(before, op.target);
    const std::string target = t ? partText(before, *t) : "partition";
    const std::uint32_t ss = before.sectorSize;
    switch (op.type) {
        case OpType::CreateTable:
            return std::string("Create ") + tableTypeName(op.table) +
                   " partition table (all existing partitions on this disk will be lost)";
        case OpType::CreatePartition:
            if (op.keepContents)
                return std::string("Recreate lost ") + partitionKindName(op.kind) + " partition (" + fsTypeName(op.fs) + ", " +
                       formatSize(op.sectorCount * ss) + ") at sector " + std::to_string(op.firstLba);
            return std::string("Create ") + partitionKindName(op.kind) + " partition (" +
                   (op.kind == PartitionKind::Extended ? "extended" : fsTypeName(op.fs)) + ", " +
                   formatSize(op.sectorCount * ss) + ") at sector " + std::to_string(op.firstLba);
        case OpType::DeletePartition: return "Delete " + target;
        case OpType::ResizeMove: {
            std::string s = "Resize/move " + target + " to sectors " + std::to_string(op.firstLba) + "-" +
                            std::to_string(op.firstLba + op.sectorCount - 1) + " (" + formatSize(op.sectorCount * ss) + ")";
            return s;
        }
        case OpType::Copy:
            return "Copy " + target + " to sector " + std::to_string(op.firstLba) + " (" + formatSize(op.sectorCount * ss) + ")";
        case OpType::Format: return "Format " + target + " as " + fsTypeName(op.fs);
        case OpType::Check: return "Check the file system of " + target + " (read-only)";
        case OpType::SetLabel: return "Set file system label of " + target + " to \"" + op.label + "\"";
        case OpType::SetName: return "Set name of " + target + " to \"" + op.name + "\"";
        case OpType::SetFlags: {
            std::string s = "Set flags of " + target + ":";
            for (const auto& f : op.flagsOn) s += " +" + f;
            for (const auto& f : op.flagsOff) s += " -" + f;
            return s;
        }
        case OpType::NewUuid: return "Set a new UUID on " + target;
    }
    return "Unknown operation";
}

// ---------------------------------------------------------------------------
// Queue

OperationQueue::OperationQueue(DiskLayout original, DiskIdentity identity)
    : original_(std::move(original)), identity_(std::move(identity)) {
    std::uint32_t id = 1;
    for (auto& p : original_.partitions) p.planId = id++;
    nextPlanId_ = id;
    current_ = original_;
}

void OperationQueue::rebuild() {
    current_ = original_;
    nextPlanId_ = static_cast<std::uint32_t>(original_.partitions.size()) + 1;
    std::string err;
    for (auto& op : ops_) simulate(op, current_, nextPlanId_, err);  // all ops were validated by add()
}

bool OperationQueue::add(Operation op, std::string& error) {
    const PartitionEntry* orig = findPlanned(original_, op.target);
    if (op.target != 0 && orig) {
        op.hasOriginal = true;
        op.original = OriginalRef{orig->number, orig->firstLba, orig->sectorCount, orig->uniqueGuid};
    }
    std::vector<Operation> candidate = ops_;
    bool merged = false;
    if (!candidate.empty() && op.target != 0) {
        Operation& last = candidate.back();
        const bool sameTarget = last.target == op.target;
        if (sameTarget && last.type == op.type &&
            (op.type == OpType::ResizeMove || op.type == OpType::SetLabel || op.type == OpType::SetName)) {
            Operation m = op;
            m.hasOriginal = last.hasOriginal;
            m.original = last.original;
            last = m;
            merged = true;
        } else if (op.type == OpType::Format &&
                   (last.type == OpType::CreatePartition) && last.result == op.target) {
            last.fs = op.fs;
            last.label = op.label;
            merged = true;
        }
    }
    if (!merged && op.type == OpType::DeletePartition) {
        // Deleting a partition created in this plan cancels its creation and
        // every later operation on it (as GParted does).
        auto creator = std::find_if(candidate.begin(), candidate.end(), [&](const Operation& o) {
            return (o.type == OpType::CreatePartition || o.type == OpType::Copy) && o.result == op.target;
        });
        if (creator != candidate.end()) {
            const std::uint32_t id = op.target;
            candidate.erase(std::remove_if(candidate.begin(), candidate.end(),
                                           [&](const Operation& o) { return o.result == id || o.target == id; }),
                            candidate.end());
            merged = true;
        }
    }
    if (!merged) candidate.push_back(op);

    // Simulate the whole candidate plan from the original layout.
    DiskLayout trial = original_;
    std::uint32_t next = static_cast<std::uint32_t>(original_.partitions.size()) + 1;
    for (auto& o : candidate) {
        if (!simulate(o, trial, next, error)) return false;
    }
    history_.push_back(ops_);
    ops_ = std::move(candidate);
    current_ = std::move(trial);
    nextPlanId_ = next;
    return true;
}

bool OperationQueue::undoLast() {
    if (history_.empty()) return false;
    ops_ = history_.back();
    history_.pop_back();
    rebuild();
    return true;
}

void OperationQueue::clear() {
    if (ops_.empty()) return;
    history_.push_back(ops_);
    ops_.clear();
    rebuild();
}

std::vector<std::string> OperationQueue::descriptions() const {
    std::vector<std::string> out;
    DiskLayout l = original_;
    std::uint32_t next = static_cast<std::uint32_t>(original_.partitions.size()) + 1;
    std::string err;
    for (Operation op : ops_) {
        out.push_back(describe(op, l));
        simulate(op, l, next, err);
    }
    return out;
}

std::string OperationQueue::toJson() const {
    JsonWriter w;
    w.beginObject();
    w.key("schema").value("wparted-plan");
    w.key("version").value(1);
    w.key("identity").beginObject();
    w.key("serial").value(identity_.serial);
    w.key("model").value(identity_.model);
    w.key("sizeBytes").value(identity_.sizeBytes);
    w.key("sectorSize").value(identity_.sectorSize);
    w.key("table").value(tableTypeName(identity_.table));
    w.key("diskGuid").value(identity_.diskGuid.toString());
    w.key("mbrSignature").value(identity_.mbrSignature);
    w.endObject();
    w.key("operations").beginArray();
    for (const auto& op : ops_) {
        w.beginObject();
        w.key("type").value(opTypeName(op.type));
        w.key("risk").value(riskName(riskOf(op)));
        w.key("target").value(op.target);
        w.key("result").value(op.result);
        w.key("table").value(tableTypeName(op.table));
        w.key("kind").value(partitionKindName(op.kind));
        w.key("firstLba").value(op.firstLba);
        w.key("sectorCount").value(op.sectorCount);
        w.key("fs").value(fsTypeName(op.fs));
        w.key("label").value(op.label);
        w.key("name").value(op.name);
        if (op.keepContents) w.key("keepContents").value(true);
        w.key("flagsOn").beginArray();
        for (const auto& f : op.flagsOn) w.value(f);
        w.endArray();
        w.key("flagsOff").beginArray();
        for (const auto& f : op.flagsOff) w.value(f);
        w.endArray();
        if (op.hasOriginal) {
            w.key("original").beginObject();
            w.key("number").value(op.original.number);
            w.key("firstLba").value(op.original.firstLba);
            w.key("sectorCount").value(op.original.sectorCount);
            w.key("uniqueGuid").value(op.original.uniqueGuid.toString());
            w.endObject();
        }
        w.endObject();
    }
    w.endArray();
    w.endObject();
    return w.str();
}

bool OperationQueue::fromJson(const std::string& json, const DiskLayout& original, OperationQueue& out,
                              std::string& error) {
    JsonValue doc;
    if (!parseJson(json, doc, error)) return false;
    if (!doc.isObject() || doc.getString("schema") != "wparted-plan" || doc.getUint("version") != 1) {
        error = "not a wparted-plan version 1 document";
        return false;
    }
    const JsonValue* idv = doc.find("identity");
    const JsonValue* opsv = doc.find("operations");
    if (!idv || !idv->isObject() || !opsv || !opsv->isArray()) {
        error = "plan is missing identity or operations";
        return false;
    }
    if (opsv->array.size() > 256) {
        error = "plan has too many operations";
        return false;
    }
    bool ok = true;
    DiskIdentity id;
    id.serial = idv->getString("serial", "", &ok);
    id.model = idv->getString("model", "", &ok);
    id.sizeBytes = idv->getUint("sizeBytes", 0, &ok);
    id.sectorSize = static_cast<std::uint32_t>(idv->getUint("sectorSize", 0, &ok));
    if (!identityTableFromName(idv->getString("table"), id.table)) id.table = TableType::Unknown;
    Guid::parse(idv->getString("diskGuid", "", &ok), id.diskGuid);
    id.mbrSignature = static_cast<std::uint32_t>(idv->getUint("mbrSignature", 0, &ok));
    if (!ok) { error = "invalid identity fields"; return false; }

    OperationQueue q(original, id);
    std::size_t index = 0;
    for (const auto& ov : opsv->array) {
        ++index;
        const std::string where = "operation " + std::to_string(index) + ": ";
        if (!ov.isObject()) { error = where + "not an object"; return false; }
        Operation op;
        if (!opTypeFromName(ov.getString("type"), op.type)) { error = where + "unknown type"; return false; }
        op.target = static_cast<std::uint32_t>(ov.getUint("target", 0, &ok));
        op.result = static_cast<std::uint32_t>(ov.getUint("result", 0, &ok));
        if (!tableFromName(ov.getString("table", "gpt"), op.table)) { error = where + "bad table"; return false; }
        if (!kindFromName(ov.getString("kind", "primary"), op.kind)) { error = where + "bad kind"; return false; }
        op.firstLba = ov.getUint("firstLba", 0, &ok);
        op.sectorCount = ov.getUint("sectorCount", 0, &ok);
        if (!fsFromName(ov.getString("fs", "unknown"), op.fs)) { error = where + "bad file system"; return false; }
        op.label = ov.getString("label", "", &ok);
        op.keepContents = ov.getBool("keepContents", false, &ok);
        op.name = ov.getString("name", "", &ok);
        for (const char* key : {"flagsOn", "flagsOff"}) {
            if (const JsonValue* fv = ov.find(key)) {
                if (!fv->isArray()) { error = where + "flags must be an array"; return false; }
                for (const auto& f : fv->array) {
                    if (!f.isString()) { error = where + "flag must be a string"; return false; }
                    (std::string(key) == "flagsOn" ? op.flagsOn : op.flagsOff).push_back(f.string);
                }
            }
        }
        if (!ok) { error = where + "invalid field types"; return false; }
        // An operation on a pre-existing partition must still match the device.
        if (const JsonValue* orv = ov.find("original")) {
            const PartitionEntry* p = findPlanned(q.original(), op.target);
            Guid g;
            Guid::parse(orv->getString("uniqueGuid"), g);
            if (!p || p->number != static_cast<int>(orv->getUint("number")) || p->firstLba != orv->getUint("firstLba") ||
                p->sectorCount != orv->getUint("sectorCount") || p->uniqueGuid != g) {
                error = where + "the referenced partition no longer matches the device";
                return false;
            }
        }
        if (!q.add(op, error)) {
            error = where + error;
            return false;
        }
    }
    out = std::move(q);
    return true;
}

}  // namespace wparted
