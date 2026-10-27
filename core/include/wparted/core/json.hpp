// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wparted/core/partition_table.hpp"

namespace wparted {

// Minimal streaming JSON writer (no external dependency). Produces compact,
// valid JSON; callers are responsible for correct begin/end nesting.
class JsonWriter {
public:
    JsonWriter& beginObject();
    JsonWriter& endObject();
    JsonWriter& beginArray();
    JsonWriter& endArray();
    JsonWriter& key(const std::string& k);
    JsonWriter& value(const std::string& v);
    JsonWriter& value(const char* v) { return value(std::string(v)); }
    JsonWriter& value(std::uint64_t v);
    JsonWriter& value(std::int64_t v);
    JsonWriter& value(int v) { return value(static_cast<std::int64_t>(v)); }
    JsonWriter& value(std::uint32_t v) { return value(static_cast<std::uint64_t>(v)); }
    JsonWriter& value(bool v);
    JsonWriter& null();
    // Inserts an already-serialised JSON fragment as a value.
    JsonWriter& raw(const std::string& json);

    const std::string& str() const { return out_; }
    static std::string escape(const std::string& s);

private:
    void separator();
    std::string out_;
    std::vector<bool> first_;  // per nesting level: no element written yet
    bool afterKey_ = false;
};

// Serialises a parsed layout (table, partitions, diagnostics) to JSON.
std::string layoutToJson(const DiskLayout& layout);

}  // namespace wparted
