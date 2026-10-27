// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#include "wparted/core/types.hpp"

#include <cstdio>

namespace wparted {

bool Guid::isZero() const noexcept {
    for (auto b : bytes)
        if (b != 0) return false;
    return true;
}

Guid Guid::fromDisk(const std::uint8_t* p) noexcept {
    Guid g;
    for (int i = 0; i < 16; ++i) g.bytes[i] = p[i];
    return g;
}

std::string Guid::toString() const {
    const auto& b = bytes;
    char buf[40];
    std::snprintf(buf, sizeof buf,
                  "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                  b[3], b[2], b[1], b[0], b[5], b[4], b[7], b[6], b[8], b[9], b[10], b[11], b[12],
                  b[13], b[14], b[15]);
    return buf;
}

static int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool Guid::parse(const std::string& text, Guid& out) {
    if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-')
        return false;
    std::uint8_t canon[16];
    int n = 0;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '-') {
            ++i;
            continue;
        }
        int hi = hexValue(text[i]), lo = hexValue(text[i + 1]);
        if (hi < 0 || lo < 0) return false;
        canon[n++] = static_cast<std::uint8_t>(hi * 16 + lo);
        i += 2;
    }
    // Canonical order -> on-disk mixed-endian order.
    static const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    for (int i = 0; i < 16; ++i) out.bytes[i] = canon[order[i]];
    return true;
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t len, std::uint32_t seed) noexcept {
    static std::uint32_t table[256];
    static const bool init = [] {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        return true;
    }();
    (void)init;
    std::uint32_t c = seed ^ 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string utf16leToUtf8(const std::uint8_t* p, std::size_t maxUnits) {
    std::string out;
    for (std::size_t i = 0; i < maxUnits; ++i) {
        std::uint32_t u = le16(p + 2 * i);
        if (u == 0) break;
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < maxUnits) {
            std::uint32_t lo = le16(p + 2 * (i + 1));
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                appendUtf8(out, 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00));
                ++i;
                continue;
            }
            u = 0xFFFD;
        } else if (u >= 0xD800 && u <= 0xDFFF) {
            u = 0xFFFD;
        }
        appendUtf8(out, u);
    }
    return out;
}

std::string formatSize(std::uint64_t bytes) {
    static const char* units[] = {"KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    if (bytes < 1024) return std::to_string(bytes) + " B";
    double v = static_cast<double>(bytes);
    int u = -1;
    while (v >= 1024.0 && u < 5) {
        v /= 1024.0;
        ++u;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f %s", v, units[u]);
    return buf;
}

}  // namespace wparted
