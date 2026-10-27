// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Internal entry points of the file-system tools (fs_tools.hpp).
#pragma once

#include <cstdint>
#include <string>

#include "wparted/core/executor.hpp"
#include "wparted/core/fs_tools.hpp"

namespace wparted::fstools::fat {

bool create(BlockDevice& d, FsType t, const std::string& label, std::uint64_t hidden, std::string& err);
bool setLabel(BlockDevice& d, const std::string& label, std::string& err);
bool setSerial(BlockDevice& d, std::uint32_t serial, std::string& err);
bool resize(BlockDevice& d, std::uint64_t newBytes, std::string& err);
bool check(BlockReader& r, CheckReport& rep, std::string& err);
std::uint64_t usedBytes(BlockReader& r);

}  // namespace wparted::fstools::fat
