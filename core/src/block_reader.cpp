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

namespace {
bool readAt(std::FILE* f, std::uint64_t offset, void* buffer, std::size_t len) {
#ifdef _WIN32
    if (_fseeki64(f, static_cast<long long>(offset), SEEK_SET) != 0) return false;
#else
    if (fseeko(f, static_cast<off_t>(offset), SEEK_SET) != 0) return false;
#endif
    return std::fread(buffer, 1, len, f) == len;
}
std::uint64_t be(const std::uint8_t* p, int n) {
    std::uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = v << 8 | p[i];
    return v;
}
}  // namespace

ImageContainer detectImageContainer(std::FILE* f, std::uint64_t fileBytes, std::uint64_t& diskBytes, std::string& why) {
    diskBytes = fileBytes;
    std::uint8_t head[8] = {};
    if (fileBytes >= 8 && readAt(f, 0, head, 8) && std::memcmp(head, "vhdxfile", 8) == 0) {
        why = "VHDX files are not supported yet (convert the image to a fixed-size VHD or a raw image)";
        return ImageContainer::Unsupported;
    }
    std::uint8_t footer[512];
    if (fileBytes < 1024 || !readAt(f, fileBytes - 512, footer, sizeof footer) || std::memcmp(footer, "conectix", 8) != 0)
        return ImageContainer::Raw;
    const std::uint64_t type = be(&footer[0x3C], 4), current = be(&footer[0x30], 8);
    if (type != 2) {
        why = "dynamic and differencing VHD files are not supported yet (convert the image to a fixed-size VHD)";
        return ImageContainer::Unsupported;
    }
    if (current != fileBytes - 512) {
        why = "the VHD footer does not match the file size";
        return ImageContainer::Unsupported;
    }
    diskBytes = fileBytes - 512;
    return ImageContainer::FixedVhd;
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
    error_.clear();
    container_ = detectImageContainer(file_, static_cast<std::uint64_t>(end), size_, error_);
    if (container_ == ImageContainer::Unsupported) {
        std::fclose(file_);
        file_ = nullptr;
        size_ = 0;
        return false;
    }
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
