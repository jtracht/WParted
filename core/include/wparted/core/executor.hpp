// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Executes a validated plan on a disk *image file*. Physical disks are out of
// scope: they need the privileged helper of ADR-0002 and a human-reviewed
// write path. What runs here:
//   - partition table changes (create table, create/delete partition,
//     resize/move boundaries, flags, names) through layout_writer,
//   - raw block moves and copies of partition contents (any file system),
//     file-system steps through fs_tools (create, resize, label, UUID, check),
//   - boundary changes of extended partitions (the logical partitions stay;
//     the EBR chain is rewritten),
//   - clearing old signatures at the start of new or "cleared" partitions.
// Every step is re-read and compared with the simulated layout before the
// next one starts ("calibration"); the first mismatch stops the plan.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "wparted/core/block_reader.hpp"
#include "wparted/core/operation_log.hpp"
#include "wparted/core/operations.hpp"
#include "wparted/core/validation.hpp"

namespace wparted {

class BlockDevice : public BlockReader {
public:
    virtual bool write(std::uint64_t offset, const void* buffer, std::size_t len) = 0;
    virtual bool flush() = 0;
};

// In-memory device for tests.
class MemoryBlockDevice final : public BlockDevice {
public:
    MemoryBlockDevice(std::vector<std::uint8_t>& data, std::uint32_t sectorSize) : data_(data), sectorSize_(sectorSize) {}
    std::uint64_t size() const override { return data_.size(); }
    std::uint32_t sectorSize() const override { return sectorSize_; }
    bool read(std::uint64_t offset, void* buffer, std::size_t len) override;
    bool write(std::uint64_t offset, const void* buffer, std::size_t len) override;
    bool flush() override { return true; }

private:
    std::vector<std::uint8_t>& data_;
    std::uint32_t sectorSize_;
};

// A partition-sized window into a device: file-system tools get this, so
// they cannot read or write outside the partition.
class RegionDevice final : public BlockDevice {
public:
    RegionDevice(BlockDevice& parent, std::uint64_t offset, std::uint64_t length)
        : parent_(parent), offset_(offset), length_(length) {}
    std::uint64_t size() const override { return length_; }
    std::uint32_t sectorSize() const override { return parent_.sectorSize(); }
    bool read(std::uint64_t offset, void* buffer, std::size_t len) override {
        return offset <= length_ && len <= length_ - offset && parent_.read(offset_ + offset, buffer, len);
    }
    bool write(std::uint64_t offset, const void* buffer, std::size_t len) override {
        return offset <= length_ && len <= length_ - offset && parent_.write(offset_ + offset, buffer, len);
    }
    bool flush() override { return parent_.flush(); }

private:
    BlockDevice& parent_;
    std::uint64_t offset_;
    std::uint64_t length_;
};

// True for paths that name a device instead of a file (\\.\PhysicalDrive0,
// \\?\Volume{...}, /dev/sda, ...). Image execution refuses them.
bool isDevicePath(const std::string& path);

// An existing, regular image file opened for reading and writing. Never
// creates or truncates; on Windows the file is opened exclusively, so an
// image that another program (e.g. a VM) holds open is refused.
class ImageFileDevice final : public BlockDevice {
public:
    ImageFileDevice() = default;
    ImageFileDevice(const ImageFileDevice&) = delete;
    ImageFileDevice& operator=(const ImageFileDevice&) = delete;
    ~ImageFileDevice() override;
    bool open(const std::string& path, std::uint32_t sectorSize, std::string& error);
    std::uint64_t size() const override { return size_; }
    std::uint32_t sectorSize() const override { return sectorSize_; }
    bool read(std::uint64_t offset, void* buffer, std::size_t len) override;
    bool write(std::uint64_t offset, const void* buffer, std::size_t len) override;
    bool flush() override;

private:
    std::FILE* file_ = nullptr;
    std::uint64_t size_ = 0;
    std::uint32_t sectorSize_ = 512;
};

struct ExecuteOptions {
    std::uint64_t chunkBytes = 4 * 1024 * 1024;  // block copy buffer
    const std::atomic<bool>* cancel = nullptr;   // honoured between steps and between copy chunks
    // Progress of the current operation (0-based index), bytes copied so far.
    std::function<void(std::size_t op, std::uint64_t done, std::uint64_t total)> progress;
    // Execution journal (see journalPathFor); empty = no journal. With a
    // journal, an interrupted execution (crash, power loss, cancel during a
    // move) is detected next time and can be resumed where that is safe.
    std::string journalPath;
    // Fault injection for tests: stop after this many copy chunks without any
    // clean-up, as a crash would. 0 = off.
    std::uint64_t abortAfterChunks = 0;
};

struct ExecuteResult {
    bool ok = false;
    std::size_t completed = 0;  // operations fully applied and verified
    std::string error;          // first failure, empty on success
    ValidationResult validation;
    LogStep log;
};

// Fills ctx.state[planId].usedBytes for partitions whose usage the
// file-system tools can read (FAT), so that validation refuses shrinking
// below the data before anything is written.
void addImageUsage(BlockReader& image, const DiskLayout& layout, ValidationContext& ctx);

// Validates `plan` in ImageExecution mode against what is on `device` now and
// executes it. Nothing is written if the device does not match the plan's
// original layout or the validation reports an error.
ExecuteResult executePlanOnImage(BlockDevice& device, const OperationQueue& plan, const std::string& deviceName,
                                 const ExecuteOptions& options = {});

// --- Execution journal --------------------------------------------------------
// A sidecar file next to the image ("<image>.wparted-journal"). It holds the
// plan, a copy of every sector the layout detection read before the first
// write (so the plan can be rebuilt exactly), and a 512-byte progress header
// that is rewritten in place - after the image data it describes has been
// flushed to disk. It is removed when the plan finishes or fails cleanly.
//
// Block moves whose source and destination overlap are copied in chunks no
// larger than the shift distance, so repeating the chunk that was being
// written when the process died never reads data that chunk overwrote; this
// is what makes resuming a move safe. Steps that change data in place
// (file-system resize/create, table writes) cannot be resumed: an interruption
// there is reported, not repaired.
std::string journalPathFor(const std::string& imagePath);
bool journalExists(const std::string& journalPath);

struct JournalState {
    std::size_t completed = 0;      // operations fully applied and verified
    std::string phase;              // "idle", "copy" or "work"
    std::size_t operation = 0;      // running operation (phase copy/work), 0-based
    std::uint64_t copyDone = 0;     // bytes moved so far (phase copy)
    std::string step;               // what operation `operation` was doing (phase work)
    std::uint64_t imageBytes = 0;
    std::uint32_t sectorSize = 0;
    std::string planJson;
    std::size_t operationCount = 0;
    bool resumable() const { return phase == "idle" || phase == "copy"; }
};

bool readJournal(const std::string& journalPath, JournalState& out, std::string& error);
// Deletes the journal (after the user has decided not to resume).
bool discardJournal(const std::string& journalPath, std::string& error);

// Continues the execution recorded in options.journalPath. Refused unless the
// journal is resumable and the image holds exactly the layout the journal
// expects (the table after the completed operations).
ExecuteResult resumePlanOnImage(BlockDevice& device, const std::string& deviceName, const ExecuteOptions& options);

}  // namespace wparted
