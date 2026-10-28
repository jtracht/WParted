// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Planned operations (GParted's pending-operation model). Everything here is
// a pure simulation on DiskLayout: nothing in this module touches a device.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wparted/core/json_value.hpp"
#include "wparted/core/partition_table.hpp"

namespace wparted {

// Identifies the disk a plan was made for. The executor must re-identify the
// device and refuse to run if anything differs (TOCTOU protection).
struct DiskIdentity {
    std::string serial;
    std::string model;
    std::uint64_t sizeBytes = 0;
    std::uint32_t sectorSize = 0;
    TableType table = TableType::None;
    Guid diskGuid;                 // GPT
    std::uint32_t mbrSignature = 0;  // MBR

    // Field-by-field comparison; `why` names the first difference.
    bool matches(const DiskIdentity& other, std::string& why) const;
};

DiskIdentity identityOf(const DiskLayout& layout, const std::string& serial, const std::string& model);

enum class OpType {
    CreateTable,
    CreatePartition,
    DeletePartition,
    ResizeMove,
    Copy,
    Format,
    Check,
    SetLabel,
    SetName,
    SetFlags,
    NewUuid,
};

const char* opTypeName(OpType t) noexcept;  // "create-partition", ...
bool opTypeFromName(const std::string& name, OpType& out);

enum class Risk { Low, Medium, High, Critical };
const char* riskName(Risk r) noexcept;

// How an operation refers to a partition that existed before the plan, so
// that the executor can find it again on the real device.
struct OriginalRef {
    int number = 0;
    std::uint64_t firstLba = 0;
    std::uint64_t sectorCount = 0;
    Guid uniqueGuid;  // GPT only
};

struct Operation {
    OpType type = OpType::Check;
    std::uint32_t target = 0;    // planId of the affected partition (0 for CreateTable/CreatePartition)
    std::uint32_t result = 0;    // planId assigned to a created/copied partition
    // Parameters (meaning depends on type)
    TableType table = TableType::Gpt;                 // CreateTable
    PartitionKind kind = PartitionKind::Primary;      // CreatePartition
    std::uint64_t firstLba = 0;                       // CreatePartition, ResizeMove, Copy (destination)
    std::uint64_t sectorCount = 0;                    // CreatePartition, ResizeMove, Copy
    FsType fs = FsType::Unknown;                      // CreatePartition, Format (Unknown = unformatted)
    std::string label;                                // CreatePartition, Format, SetLabel
    std::string name;                                 // CreatePartition (GPT), SetName
    std::vector<std::string> flagsOn, flagsOff;       // SetFlags
    // CreatePartition: recreate a lost partition around an existing file system
    // of type `fs` (data rescue). Nothing inside the partition is written.
    bool keepContents = false;
    bool hasOriginal = false;                         // target existed before the plan
    OriginalRef original;
};

Risk riskOf(const Operation& op) noexcept;
std::string describe(const Operation& op, const DiskLayout& before);

// Applies `op` to `layout`. Returns false (layout unchanged) with an error if
// the operation is structurally impossible (unknown target, wrong table type,
// deleting an extended partition that still has logicals, ...). Geometric
// constraints are checked by the validation engine, not here.
bool simulate(Operation& op, DiskLayout& layout, std::uint32_t& nextPlanId, std::string& error);

// Flags in GParted vocabulary that can be set on a table type.
std::vector<std::string> settableFlags(TableType t);
// Sets or clears a flag by changing the type GUID/attributes (GPT) or the
// boot indicator/type byte (MBR).
bool setPartitionFlag(TableType t, PartitionEntry& p, const std::string& flag, bool on, std::string& error);
// Default partition type for a new partition holding `fs`.
void applyDefaultType(TableType t, PartitionEntry& p, FsType fs);

// Pending operations for one disk. Every add() is simulated immediately; the
// resulting layout is what the UI shows ("future layout").
class OperationQueue {
public:
    OperationQueue() = default;
    OperationQueue(DiskLayout original, DiskIdentity identity);

    bool add(Operation op, std::string& error);  // simulates, merges with the previous op where GParted does
    bool undoLast();
    void clear();

    const DiskLayout& original() const { return original_; }
    const DiskLayout& current() const { return current_; }
    const std::vector<Operation>& operations() const { return ops_; }
    const DiskIdentity& identity() const { return identity_; }
    bool empty() const { return ops_.empty(); }
    // One human-readable line per operation (each described against the layout before it).
    std::vector<std::string> descriptions() const;

    // Plan exchange format (schema "wparted-plan", version 1). Declarative:
    // no commands or paths, only partition references and parameters.
    std::string toJson() const;
    static bool fromJson(const std::string& json, const DiskLayout& original, OperationQueue& out, std::string& error);

private:
    void rebuild();
    DiskLayout original_;
    DiskLayout current_;
    DiskIdentity identity_;
    std::vector<Operation> ops_;
    std::vector<std::vector<Operation>> history_;  // ops_ before each add(), for exact undo
    std::uint32_t nextPlanId_ = 1;
};

// Returns the partition with planId `id` or nullptr.
const PartitionEntry* findPlanned(const DiskLayout& layout, std::uint32_t id);

}  // namespace wparted
