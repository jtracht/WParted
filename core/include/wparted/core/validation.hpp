// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Validation engine: decides whether a plan may be shown as executable. It
// runs in the GUI (feedback while planning) and - authoritatively - in the
// privileged helper before anything would be executed.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "wparted/core/operations.hpp"

namespace wparted {

enum class ValidationMode {
    Planning,   // capability gaps of this build are warnings: the plan can be made, not executed
    Execution,  // everything that cannot be executed safely is an error
    // Execution on a disk image file (executor.hpp): partition-table changes and
    // raw block moves/copies are possible; file-system changes are not. System
    // partitions are warnings only, because an image is not the running system.
    ImageExecution,
};

// Facts about the real device that the core cannot know by itself.
// BitLocker state as Windows reports it (Win32_EncryptableVolume; needs
// administrator rights, otherwise NotReported). WParted never suspends,
// disables or decrypts BitLocker; these states only make validation stricter.
enum class BitLockerState {
    NotReported,    // unknown (not elevated, no BitLocker on this Windows, not a Windows volume)
    NotEncrypted,
    ProtectionOff,  // encrypted, protection suspended
    ProtectionOn,
    Locked,
    Converting,     // encryption or decryption in progress or paused
};
const char* bitLockerStateName(BitLockerState s) noexcept;

struct PartitionState {
    bool mounted = false;          // has a drive letter / mount point
    bool containsWindows = false;  // the running Windows lives here
    bool pageFile = false;
    bool bootManager = false;      // holds the boot manager Windows started from (ESP or "System Reserved")
    BitLockerState bitLocker = BitLockerState::NotReported;
    std::uint64_t usedBytes = 0;   // 0 = unknown
};

struct ValidationContext {
    ValidationMode mode = ValidationMode::Planning;
    bool deviceWriteProtected = false;
    bool managedByWindows = false;              // dynamic disk / Storage Spaces member
    std::uint32_t physicalSectorSize = 0;       // 0 = unknown
    std::uint64_t alignmentBytes = 1024 * 1024; // MiB alignment (GParted default); 0 = none
    std::map<std::uint32_t, PartitionState> state;  // by planId of original partitions
    // Execution mode only: identity of the device as it is *now*.
    bool haveCurrentIdentity = false;
    DiskIdentity currentIdentity;
};

struct ValidationResult {
    std::vector<Diagnostic> diagnostics;  // codes documented in docs/architecture/validation-rules.md
    bool ok() const noexcept;             // no Error diagnostics
};

ValidationResult validatePlan(const OperationQueue& queue, const ValidationContext& ctx);

// Minimum partition size WParted accepts for a new file system (bytes).
std::uint64_t minimumSize(FsType t) noexcept;
// Largest partition a file system can fill (0 = not checked).
std::uint64_t maximumSize(FsType t, std::uint32_t sectorSize) noexcept;

}  // namespace wparted
