// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace wparted {

// Read-only access to a block device or an image. There is intentionally no
// write method: everything built on top of this interface (discovery, table
// parsing, signature detection) cannot modify data by construction.
class BlockReader {
public:
    virtual ~BlockReader() = default;

    // Total size in bytes.
    virtual std::uint64_t size() const = 0;
    // Logical sector size in bytes (512, 4096, ...).
    virtual std::uint32_t sectorSize() const = 0;
    // Reads exactly `len` bytes at byte `offset`. Implementations may require
    // nothing about alignment from the caller; returns false on any failure,
    // including reads that extend past the end.
    virtual bool read(std::uint64_t offset, void* buffer, std::size_t len) = 0;
};

// Reader over an in-memory buffer. Used by tests and for already-read data.
class MemoryBlockReader final : public BlockReader {
public:
    MemoryBlockReader(std::vector<std::uint8_t> data, std::uint32_t sectorSize)
        : data_(std::move(data)), sectorSize_(sectorSize) {}

    std::uint64_t size() const override { return data_.size(); }
    std::uint32_t sectorSize() const override { return sectorSize_; }
    bool read(std::uint64_t offset, void* buffer, std::size_t len) override;

private:
    std::vector<std::uint8_t> data_;
    std::uint32_t sectorSize_;
};

// Reader over a raw disk image file (opened read-only).
class FileBlockReader final : public BlockReader {
public:
    FileBlockReader() = default;
    ~FileBlockReader() override;
    FileBlockReader(const FileBlockReader&) = delete;
    FileBlockReader& operator=(const FileBlockReader&) = delete;

    bool open(const std::string& path, std::uint32_t sectorSize);
    std::uint64_t size() const override { return size_; }
    std::uint32_t sectorSize() const override { return sectorSize_; }
    bool read(std::uint64_t offset, void* buffer, std::size_t len) override;

private:
    std::FILE* file_ = nullptr;
    std::uint64_t size_ = 0;
    std::uint32_t sectorSize_ = 512;
};

// A window into another reader, e.g. one partition of a disk.
class SubRangeReader final : public BlockReader {
public:
    SubRangeReader(BlockReader& parent, std::uint64_t offset, std::uint64_t length)
        : parent_(parent), offset_(offset), length_(length) {}

    std::uint64_t size() const override { return length_; }
    std::uint32_t sectorSize() const override { return parent_.sectorSize(); }
    bool read(std::uint64_t offset, void* buffer, std::size_t len) override;

private:
    BlockReader& parent_;
    std::uint64_t offset_;
    std::uint64_t length_;
};

}  // namespace wparted
