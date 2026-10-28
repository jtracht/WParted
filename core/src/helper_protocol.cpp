// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#include "wparted/core/helper_protocol.hpp"

#include <cstdio>
#include <cstring>
#include <set>

#include "wparted/core/json.hpp"
#include "wparted/core/json_value.hpp"

namespace wparted::helper {
namespace {

const std::pair<Type, const char*> kTypes[] = {{Type::Hello, "hello"},       {Type::Plan, "plan"},
                                              {Type::Validation, "validation"}, {Type::Confirm, "confirm"},
                                              {Type::Progress, "progress"}, {Type::Cancel, "cancel"},
                                              {Type::Log, "log"},           {Type::Done, "done"}};

// Fields each message type may carry (besides "type").
std::set<std::string> fieldsOf(Type t) {
    switch (t) {
        case Type::Hello: return {"protocol", "version"};
        case Type::Plan: return {"plan", "image", "sectorSize"};
        case Type::Validation: return {"ok", "diagnostics", "planHash"};
        case Type::Confirm: return {"planHash"};
        case Type::Progress: return {"step", "done", "total"};
        case Type::Cancel: return {};
        case Type::Log: return {"log"};
        case Type::Done: return {"result", "error"};
    }
    return {};
}

const char* severityName(Severity s) { return s == Severity::Error ? "error" : s == Severity::Warning ? "warning" : "info"; }

// SHA-256 (FIPS 180-4).
struct Sha256 {
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    void block(const std::uint8_t* p) {
        static const std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
            0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
            0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        auto rotr = [](std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = std::uint32_t{p[4 * i]} << 24 | std::uint32_t{p[4 * i + 1]} << 16 | std::uint32_t{p[4 * i + 2]} << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
};

}  // namespace

const char* typeName(Type t) noexcept {
    for (const auto& x : kTypes)
        if (x.first == t) return x.second;
    return "?";
}

std::string sha256Hex(const std::string& data) {
    Sha256 s;
    const std::uint64_t bits = std::uint64_t{data.size()} * 8;
    std::string m = data;
    m += static_cast<char>(0x80);
    while (m.size() % 64 != 56) m += '\0';
    for (int i = 7; i >= 0; --i) m += static_cast<char>(bits >> (8 * i));
    for (std::size_t i = 0; i < m.size(); i += 64) s.block(reinterpret_cast<const std::uint8_t*>(m.data() + i));
    char out[65];
    for (int i = 0; i < 8; ++i) std::snprintf(out + 8 * i, 9, "%08x", s.h[i]);
    return std::string(out, 64);
}

std::string encode(const Message& m) {
    JsonWriter w;
    w.beginObject();
    w.key("type").value(typeName(m.type));
    switch (m.type) {
        case Type::Hello:
            w.key("protocol").value(m.protocol);
            w.key("version").value(m.version);
            break;
        case Type::Plan:
            w.key("plan").value(m.plan);
            w.key("image").value(m.image);
            w.key("sectorSize").value(m.sectorSize);
            break;
        case Type::Validation:
            w.key("ok").value(m.ok);
            w.key("planHash").value(m.planHash);
            w.key("diagnostics").beginArray();
            for (const auto& d : m.diagnostics) {
                w.beginObject();
                w.key("severity").value(severityName(d.severity));
                w.key("code").value(d.code);
                w.key("message").value(d.message);
                w.endObject();
            }
            w.endArray();
            break;
        case Type::Confirm: w.key("planHash").value(m.planHash); break;
        case Type::Progress:
            w.key("step").value(m.step);
            w.key("done").value(m.done);
            w.key("total").value(m.total);
            break;
        case Type::Cancel: break;
        case Type::Log: w.key("log").value(m.log); break;
        case Type::Done:
            w.key("result").value(m.result);
            w.key("error").value(m.error);
            break;
    }
    w.endObject();
    return w.str();
}

bool decode(const std::string& text, Message& out, std::string& error) {
    if (text.size() > kMaxMessage) {
        error = "message too large";
        return false;
    }
    JsonValue v;
    JsonParseLimits limits;
    limits.maxBytes = kMaxMessage;
    if (!parseJson(text, v, error, limits) || !v.isObject()) {
        error = "not a JSON object" + (error.empty() ? std::string() : ": " + error);
        return false;
    }
    const std::string type = v.getString("type");
    bool known = false;
    for (const auto& x : kTypes)
        if (type == x.second) {
            out = Message{};
            out.type = x.first;
            known = true;
        }
    if (!known) {
        error = "unknown message type '" + type + "'";
        return false;
    }
    const std::set<std::string> allowed = fieldsOf(out.type);
    for (const auto& field : v.object)
        if (field.first != "type" && !allowed.count(field.first)) {
            error = "unexpected field '" + field.first + "' in " + type;
            return false;
        }
    for (const auto& field : allowed)
        if (!v.find(field)) {
            error = "missing field '" + field + "' in " + type;
            return false;
        }
    bool ok = true;
    out.protocol = static_cast<int>(v.getUint("protocol", 0, &ok));
    out.version = v.getString("version", "", &ok);
    out.plan = v.getString("plan", "", &ok);
    out.image = v.getString("image", "", &ok);
    out.sectorSize = static_cast<std::uint32_t>(v.getUint("sectorSize", 512, &ok));
    out.ok = v.getBool("ok", false, &ok);
    out.planHash = v.getString("planHash", "", &ok);
    out.step = v.getUint("step", 0, &ok);
    out.done = v.getUint("done", 0, &ok);
    out.total = v.getUint("total", 0, &ok);
    out.log = v.getString("log", "", &ok);
    out.result = v.getString("result", "", &ok);
    out.error = v.getString("error", "", &ok);
    if (const JsonValue* ds = v.find("diagnostics")) {
        if (!ds->isArray()) ok = false;
        for (const auto& d : ds->array) {
            if (!d.isObject() || d.object.size() != 3) {
                ok = false;
                break;
            }
            const std::string sev = d.getString("severity", "", &ok);
            out.diagnostics.emplace_back(sev == "error" ? Severity::Error : sev == "warning" ? Severity::Warning : Severity::Info,
                                         d.getString("code", "", &ok), d.getString("message", "", &ok));
        }
    }
    if (!ok) {
        error = "a field of " + type + " has the wrong type";
        return false;
    }
    if (out.type == Type::Done && out.result != "success" && out.result != "failed" && out.result != "cancelled") {
        error = "unknown result '" + out.result + "'";
        return false;
    }
    return true;
}

}  // namespace wparted::helper
