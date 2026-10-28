// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Messages between WParted (GUI/CLI) and wparted-helper.exe (ADR-0002). One
// UTF-8 JSON object per pipe message, at most 1 MiB; unknown fields, wrong
// types and unknown message types are rejected, not ignored. There are no
// generic commands: the only input that leads to writes is a declarative plan,
// and the helper only executes it after a `confirm` that carries the SHA-256 of
// the exact plan text it validated.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wparted/core/partition_table.hpp"

namespace wparted::helper {

constexpr int kProtocol = 1;
constexpr std::size_t kMaxMessage = 1024 * 1024;

enum class Type { Hello, Plan, Validation, Confirm, Progress, Cancel, Log, Done };

struct Message {
    Type type = Type::Hello;
    int protocol = 0;               // hello
    std::string version;            // hello
    std::string plan;               // plan: the plan text (schema wparted-plan v1)
    std::string image;              // plan: target image file (the prototype executes on image files only)
    std::uint32_t sectorSize = 512; // plan
    bool ok = false;                // validation
    std::vector<Diagnostic> diagnostics;  // validation
    std::string planHash;           // validation, confirm: SHA-256 (hex) of `plan`
    std::uint64_t step = 0, done = 0, total = 0;  // progress
    std::string log;                // log: logToJson()
    std::string result;             // done: success | failed | cancelled
    std::string error;              // done: why it failed
};

const char* typeName(Type t) noexcept;
std::string encode(const Message& m);
bool decode(const std::string& text, Message& out, std::string& error);

std::string sha256Hex(const std::string& data);

}  // namespace wparted::helper
