// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// The only module with write access to disks and volumes. Used by
// wparted-helper.exe after it has validated the plan itself (ADR-0002).
// tools/ci/check_readonly.py allows these APIs in this file and nowhere else.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "wparted/core/disk_write_gate.hpp"
#include "wparted/core/executor.hpp"
#include "wparted/win/disk_discovery.hpp"

namespace wparted::win {

DiskWriteFacts writeFactsFor(const PhysicalDisk& disk, bool elevated);
// Validation context with what Windows reports about this disk's volumes.
ValidationContext contextForDisk(const PhysicalDisk& disk, const DiskLayout& layout, ValidationMode mode);

// %ProgramData%\WParted\journal\disk-<hash of serial and size>.journal
std::string journalPathForDisk(const PhysicalDisk& disk);
// Creates the journal folder, owned by Administrators and writable only by
// administrators and SYSTEM. Refuses reparse points. Helper only.
bool prepareJournalFolder(std::string& error);

class PhysicalDiskDevice final : public BlockDevice {
public:
    PhysicalDiskDevice() = default;
    PhysicalDiskDevice(const PhysicalDiskDevice&) = delete;
    PhysicalDiskDevice& operator=(const PhysicalDiskDevice&) = delete;
    ~PhysicalDiskDevice() override;
    bool open(const PhysicalDisk& disk, std::string& error);
    std::uint64_t size() const override { return size_; }
    std::uint32_t sectorSize() const override { return sectorSize_; }
    bool read(std::uint64_t offset, void* buffer, std::size_t len) override;
    bool write(std::uint64_t offset, const void* buffer, std::size_t len) override;
    bool flush() override;
    // Makes Windows re-read the partition table.
    bool updateProperties(std::string& error);

private:
    bool transfer(std::uint64_t offset, void* readTo, const void* writeFrom, std::size_t len);
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::uint64_t size_ = 0;
    std::uint32_t sectorSize_ = 512;
    void* bounce_ = nullptr;
};

// Locks and dismounts every volume of a disk until destroyed. If one cannot be
// locked (something has files open on it), nothing stays locked.
class VolumeLocks {
public:
    VolumeLocks() = default;
    VolumeLocks(const VolumeLocks&) = delete;
    VolumeLocks& operator=(const VolumeLocks&) = delete;
    ~VolumeLocks();
    bool lockAll(const PhysicalDisk& disk, std::string& error);
    void release();

private:
    std::vector<HANDLE> handles_;
};

// `mountPoint` is a drive letter "E:\" (validated by the caller).
bool addMountPoint(const std::string& volumeGuidPath, const std::string& mountPoint, std::string& error);
// Removes all mount points of the volume and dismounts it where possible.
bool removeMountPoints(const VolumeInfo& volume, std::string& error);

}  // namespace wparted::win
