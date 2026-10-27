// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#pragma once

#include <string>

namespace wparted::log {

enum class Level { Debug, Info, Warning, Error };

// Opens (appends to) the log file. Without a file, messages are kept only in
// the in-memory ring buffer that the UI can display or export.
bool openFile(const std::string& utf8Path);
void write(Level level, const std::string& message);
// Recent messages, oldest first, one per line.
std::string recent();

inline void info(const std::string& m) { write(Level::Info, m); }
inline void warn(const std::string& m) { write(Level::Warning, m); }
inline void error(const std::string& m) { write(Level::Error, m); }

}  // namespace wparted::log
