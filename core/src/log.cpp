// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#include "wparted/core/log.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>

namespace wparted::log {

namespace {

std::mutex g_mutex;
std::FILE* g_file = nullptr;
std::deque<std::string> g_ring;
constexpr std::size_t kRingSize = 500;

const char* levelName(Level l) {
    switch (l) {
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO ";
        case Level::Warning: return "WARN ";
        case Level::Error: return "ERROR";
    }
    return "INFO ";
}

std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[80];  // sized for the worst case of every %d field (GCC -Wformat-truncation)
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d.%03d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
    return buf;
}

}  // namespace

bool openFile(const std::string& utf8Path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
#ifdef _WIN32
    // Paths are UTF-8; convert for the wide CRT API.
    std::wstring w;
    for (std::size_t i = 0; i < utf8Path.size();) {
        unsigned char c = static_cast<unsigned char>(utf8Path[i]);
        std::uint32_t cp;
        int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; n = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; n = 3; }
        else { cp = c & 0x07; n = 4; }
        for (int k = 1; k < n && i + k < utf8Path.size(); ++k) cp = (cp << 6) | (utf8Path[i + k] & 0x3F);
        i += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            w += static_cast<wchar_t>(0xD800 + (cp >> 10));
            w += static_cast<wchar_t>(0xDC00 + (cp & 0x3FF));
        } else {
            w += static_cast<wchar_t>(cp);
        }
    }
    if (_wfopen_s(&g_file, w.c_str(), L"ab") != 0) g_file = nullptr;
#else
    g_file = std::fopen(utf8Path.c_str(), "ab");
#endif
    return g_file != nullptr;
}

void write(Level level, const std::string& message) {
    std::string line = timestamp() + " " + levelName(level) + " " + message;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_ring.push_back(line);
    if (g_ring.size() > kRingSize) g_ring.pop_front();
    if (g_file) {
        std::fputs(line.c_str(), g_file);
        std::fputs("\n", g_file);
        std::fflush(g_file);
    }
}

std::string recent() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string out;
    for (const auto& l : g_ring) {
        out += l;
        out += '\n';
    }
    return out;
}

}  // namespace wparted::log
