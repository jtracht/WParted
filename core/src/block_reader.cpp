// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#include "wparted/core/block_reader.hpp"

#include <cstring>

namespace wparted {

bool MemoryBlockReader::read(std::uint64_t offset, void* buffer, std::size_t len) {
    if (offset > data_.size() || len > data_.size() - offset) return false;
    if (len) std::memcpy(buffer, data_.data() + offset, len);
    return true;
}

FileBlockReader::~FileBlockReader() {
    if (file_) std::fclose(file_);
}

bool FileBlockReader::open(const std::string& path, std::uint32_t sectorSize) {
    if (file_) std::fclose(file_);
    file_ = nullptr;
#ifdef _WIN32
    if (fopen_s(&file_, path.c_str(), "rb") != 0) file_ = nullptr;
#else
    file_ = std::fopen(path.c_str(), "rb");
#endif
    if (!file_) return false;
#ifdef _WIN32
    if (_fseeki64(file_, 0, SEEK_END) != 0) return false;
    long long end = _ftelli64(file_);
#else
    if (fseeko(file_, 0, SEEK_END) != 0) return false;
    long long end = ftello(file_);
#endif
    if (end < 0) return false;
    size_ = static_cast<std::uint64_t>(end);
    sectorSize_ = sectorSize;
    return true;
}

bool FileBlockReader::read(std::uint64_t offset, void* buffer, std::size_t len) {
    if (!file_ || offset > size_ || len > size_ - offset) return false;
#ifdef _WIN32
    if (_fseeki64(file_, static_cast<long long>(offset), SEEK_SET) != 0) return false;
#else
    if (fseeko(file_, static_cast<off_t>(offset), SEEK_SET) != 0) return false;
#endif
    return std::fread(buffer, 1, len, file_) == len;
}

bool SubRangeReader::read(std::uint64_t offset, void* buffer, std::size_t len) {
    if (offset > length_ || len > length_ - offset) return false;
    return parent_.read(offset_ + offset, buffer, len);
}

}  // namespace wparted
