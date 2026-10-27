// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#include "wparted/core/json_value.hpp"

#include <cstdint>
#include <cstdlib>

namespace wparted {

const JsonValue* JsonValue::find(const std::string& key) const {
    if (kind != Kind::Object) return nullptr;
    auto it = object.find(key);
    return it == object.end() ? nullptr : &it->second;
}

std::string JsonValue::getString(const std::string& key, const std::string& def, bool* ok) const {
    const JsonValue* v = find(key);
    if (!v) return def;
    if (v->kind != Kind::String) {
        if (ok) *ok = false;
        return def;
    }
    return v->string;
}

std::uint64_t JsonValue::getUint(const std::string& key, std::uint64_t def, bool* ok) const {
    const JsonValue* v = find(key);
    if (!v) return def;
    if (v->kind != Kind::Number || !v->isInteger || v->negative) {
        if (ok) *ok = false;
        return def;
    }
    return v->integer;
}

bool JsonValue::getBool(const std::string& key, bool def, bool* ok) const {
    const JsonValue* v = find(key);
    if (!v) return def;
    if (v->kind != Kind::Bool) {
        if (ok) *ok = false;
        return def;
    }
    return v->boolean;
}

namespace {

class Parser {
public:
    Parser(const std::string& s, const JsonParseLimits& l) : s_(s), limits_(l) {}

    bool parse(JsonValue& out, std::string& err) {
        if (s_.size() > limits_.maxBytes) return fail(err, "document too large");
        skipWs();
        if (!value(out, 0, err)) return false;
        skipWs();
        if (pos_ != s_.size()) return fail(err, "trailing characters");
        return true;
    }

private:
    bool fail(std::string& err, const char* msg) {
        err = std::string(msg) + " at offset " + std::to_string(pos_);
        return false;
    }
    void skipWs() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
    }
    bool literal(const char* lit) {
        std::size_t n = 0;
        while (lit[n]) ++n;
        if (s_.compare(pos_, n, lit) != 0) return false;
        pos_ += n;
        return true;
    }

    bool value(JsonValue& v, int depth, std::string& err) {
        if (depth > limits_.maxDepth) return fail(err, "nesting too deep");
        if (pos_ >= s_.size()) return fail(err, "unexpected end");
        char c = s_[pos_];
        if (c == '{') return object(v, depth, err);
        if (c == '[') return array(v, depth, err);
        if (c == '"') {
            v.kind = JsonValue::Kind::String;
            return string(v.string, err);
        }
        if (c == 't' && literal("true")) { v.kind = JsonValue::Kind::Bool; v.boolean = true; return true; }
        if (c == 'f' && literal("false")) { v.kind = JsonValue::Kind::Bool; v.boolean = false; return true; }
        if (c == 'n' && literal("null")) { v.kind = JsonValue::Kind::Null; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) return numberValue(v, err);
        return fail(err, "unexpected character");
    }

    bool object(JsonValue& v, int depth, std::string& err) {
        v.kind = JsonValue::Kind::Object;
        ++pos_;
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
        while (true) {
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != '"') return fail(err, "expected key");
            std::string key;
            if (!string(key, err)) return false;
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != ':') return fail(err, "expected ':'");
            ++pos_;
            skipWs();
            JsonValue child;
            if (!value(child, depth + 1, err)) return false;
            if (v.object.count(key)) return fail(err, "duplicate key");
            v.object.emplace(std::move(key), std::move(child));
            skipWs();
            if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
            return fail(err, "expected ',' or '}'");
        }
    }

    bool array(JsonValue& v, int depth, std::string& err) {
        v.kind = JsonValue::Kind::Array;
        ++pos_;
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
        while (true) {
            skipWs();
            JsonValue child;
            if (!value(child, depth + 1, err)) return false;
            v.array.push_back(std::move(child));
            skipWs();
            if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
            return fail(err, "expected ',' or ']'");
        }
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

    bool hex4(std::uint32_t& out, std::string& err) {
        if (pos_ + 4 > s_.size()) return fail(err, "short \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return fail(err, "bad \\u escape");
        }
        return true;
    }

    bool string(std::string& out, std::string& err) {
        ++pos_;  // opening quote
        while (pos_ < s_.size()) {
            unsigned char c = static_cast<unsigned char>(s_[pos_++]);
            if (c == '"') return true;
            if (c < 0x20) return fail(err, "control character in string");
            if (c != '\\') {
                out += static_cast<char>(c);
                continue;
            }
            if (pos_ >= s_.size()) break;
            char e = s_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    std::uint32_t cp;
                    if (!hex4(cp, err)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        std::uint32_t lo;
                        if (pos_ + 2 > s_.size() || s_[pos_] != '\\' || s_[pos_ + 1] != 'u') return fail(err, "unpaired surrogate");
                        pos_ += 2;
                        if (!hex4(lo, err)) return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) return fail(err, "invalid surrogate pair");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail(err, "unpaired surrogate");
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return fail(err, "bad escape");
            }
        }
        return fail(err, "unterminated string");
    }

    bool numberValue(JsonValue& v, std::string& err) {
        v.kind = JsonValue::Kind::Number;
        std::size_t start = pos_;
        if (s_[pos_] == '-') { v.negative = true; ++pos_; }
        if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') return fail(err, "bad number");
        if (s_[pos_] == '0' && pos_ + 1 < s_.size() && s_[pos_ + 1] >= '0' && s_[pos_ + 1] <= '9')
            return fail(err, "leading zero");
        bool overflow = false;
        std::uint64_t mag = 0;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
            std::uint64_t d = static_cast<std::uint64_t>(s_[pos_] - '0');
            if (mag > (UINT64_MAX - d) / 10) overflow = true;
            else mag = mag * 10 + d;
            ++pos_;
        }
        bool fractional = false;
        if (pos_ < s_.size() && s_[pos_] == '.') {
            fractional = true;
            ++pos_;
            if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') return fail(err, "bad fraction");
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            fractional = true;
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') return fail(err, "bad exponent");
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        v.isInteger = !fractional && !overflow;
        v.integer = mag;
        v.number = std::strtod(s_.substr(start, pos_ - start).c_str(), nullptr);
        return true;
    }

    const std::string& s_;
    const JsonParseLimits& limits_;
    std::size_t pos_ = 0;
};

}  // namespace

bool parseJson(const std::string& text, JsonValue& out, std::string& error, const JsonParseLimits& limits) {
    out = JsonValue{};
    Parser p(text, limits);
    return p.parse(out, error);
}

}  // namespace wparted
