// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace wparted {

// A GUID as it is stored on disk in GPT structures (mixed endian: the first
// three fields are little endian, the last eight bytes are stored as-is).
struct Guid {
    std::array<std::uint8_t, 16> bytes{};

    bool isZero() const noexcept;
    // Canonical upper-case text form, e.g. "C12A7328-F81F-11D2-BA4B-00A0C93EC93B".
    std::string toString() const;
    // Parses the canonical text form; returns false on malformed input.
    static bool parse(const std::string& text, Guid& out);
    static Guid fromDisk(const std::uint8_t* p) noexcept;

    friend bool operator==(const Guid& a, const Guid& b) noexcept { return a.bytes == b.bytes; }
    friend bool operator!=(const Guid& a, const Guid& b) noexcept { return !(a == b); }
};

// Little-endian readers that never perform unaligned access.
inline std::uint16_t le16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
inline std::uint32_t le32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
inline std::uint64_t le64(const std::uint8_t* p) noexcept {
    return static_cast<std::uint64_t>(le32(p)) | (static_cast<std::uint64_t>(le32(p + 4)) << 32);
}
inline std::uint32_t be32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

// IEEE 802.3 CRC-32 as used by the UEFI specification for GPT headers/arrays.
std::uint32_t crc32(const std::uint8_t* data, std::size_t len, std::uint32_t seed = 0) noexcept;

// Converts UTF-16LE code units (as stored on disk) to UTF-8, stopping at the
// first NUL. Unpaired surrogates are replaced with U+FFFD.
std::string utf16leToUtf8(const std::uint8_t* p, std::size_t maxUnits);

// Encodes UTF-8 as UTF-16LE into `out` (maxUnits code units, zero padded).
// Returns false if the text is not valid UTF-8 or does not fit.
bool utf8ToUtf16le(const std::string& text, std::uint8_t* out, std::size_t maxUnits);

// Random version-4 GUID (RFC 4122), stored in GPT on-disk order.
Guid randomGuid();

// Formats a byte count like GParted does: binary units with two decimals
// ("512.00 MiB"). Values below 1 KiB are shown as plain bytes.
std::string formatSize(std::uint64_t bytes);

}  // namespace wparted
