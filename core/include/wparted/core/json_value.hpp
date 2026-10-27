// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace wparted {

// Small JSON document model used to read operation plans (also by the
// privileged helper, so the parser is strict and bounded: depth and size
// limits, no exceptions, integers kept exact up to 2^64-1).
class JsonValue {
public:
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool boolean = false;
    bool isInteger = false;      // number without fraction/exponent
    bool negative = false;
    std::uint64_t integer = 0;   // magnitude when isInteger
    double number = 0;
    std::string string;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;

    bool isObject() const { return kind == Kind::Object; }
    bool isArray() const { return kind == Kind::Array; }
    bool isString() const { return kind == Kind::String; }
    const JsonValue* find(const std::string& key) const;

    // Typed accessors with defaults; `ok` (if given) is cleared on a type mismatch.
    std::string getString(const std::string& key, const std::string& def = {}, bool* ok = nullptr) const;
    std::uint64_t getUint(const std::string& key, std::uint64_t def = 0, bool* ok = nullptr) const;
    bool getBool(const std::string& key, bool def = false, bool* ok = nullptr) const;
};

struct JsonParseLimits {
    std::size_t maxBytes = 1024 * 1024;
    int maxDepth = 32;
};

// Parses `text`. Returns false and an error message on malformed input,
// trailing garbage or limit violations.
bool parseJson(const std::string& text, JsonValue& out, std::string& error, const JsonParseLimits& limits = {});

}  // namespace wparted
