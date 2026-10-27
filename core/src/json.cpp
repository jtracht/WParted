// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#include "wparted/core/json.hpp"

#include <cstdio>

namespace wparted {

void JsonWriter::separator() {
    if (afterKey_) {
        afterKey_ = false;
        return;
    }
    if (!first_.empty()) {
        if (!first_.back()) out_ += ',';
        first_.back() = false;
    }
}

JsonWriter& JsonWriter::beginObject() {
    separator();
    out_ += '{';
    first_.push_back(true);
    return *this;
}
JsonWriter& JsonWriter::endObject() {
    out_ += '}';
    first_.pop_back();
    return *this;
}
JsonWriter& JsonWriter::beginArray() {
    separator();
    out_ += '[';
    first_.push_back(true);
    return *this;
}
JsonWriter& JsonWriter::endArray() {
    out_ += ']';
    first_.pop_back();
    return *this;
}
JsonWriter& JsonWriter::key(const std::string& k) {
    separator();
    out_ += escape(k);
    out_ += ':';
    afterKey_ = true;
    return *this;
}
JsonWriter& JsonWriter::value(const std::string& v) {
    separator();
    out_ += escape(v);
    return *this;
}
JsonWriter& JsonWriter::value(std::uint64_t v) {
    separator();
    out_ += std::to_string(v);
    return *this;
}
JsonWriter& JsonWriter::value(std::int64_t v) {
    separator();
    out_ += std::to_string(v);
    return *this;
}
JsonWriter& JsonWriter::value(bool v) {
    separator();
    out_ += v ? "true" : "false";
    return *this;
}
JsonWriter& JsonWriter::null() {
    separator();
    out_ += "null";
    return *this;
}
JsonWriter& JsonWriter::raw(const std::string& json) {
    separator();
    out_ += json;
    return *this;
}

std::string JsonWriter::escape(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    o += buf;
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    o += '"';
    return o;
}

static const char* severityName(Severity s) {
    switch (s) {
        case Severity::Info: return "info";
        case Severity::Warning: return "warning";
        case Severity::Error: return "error";
    }
    return "info";
}

std::string layoutToJson(const DiskLayout& l) {
    JsonWriter w;
    w.beginObject();
    w.key("table").value(tableTypeName(l.type));
    if (!l.foreignTable.empty()) w.key("foreignTable").value(l.foreignTable);
    w.key("sectorSize").value(l.sectorSize);
    w.key("sizeBytes").value(l.diskBytes);
    if (l.type == TableType::Gpt) {
        w.key("diskGuid").value(l.diskGuid.toString());
        w.key("firstUsableLba").value(l.firstUsableLba);
        w.key("lastUsableLba").value(l.lastUsableLba);
        w.key("primaryHeaderValid").value(l.gptPrimaryValid);
        w.key("backupHeaderValid").value(l.gptBackupValid);
    } else if (l.type == TableType::Mbr) {
        char sig[16];
        std::snprintf(sig, sizeof sig, "%08X", l.mbrSignature);
        w.key("mbrSignature").value(sig);
    }
    w.key("partitions").beginArray();
    for (const auto& p : l.partitions) {
        w.beginObject();
        w.key("number").value(p.number);
        w.key("kind").value(partitionKindName(p.kind));
        w.key("firstLba").value(p.firstLba);
        w.key("sectorCount").value(p.sectorCount);
        w.key("sizeBytes").value(p.sectorCount * l.sectorSize);
        w.key("type").value(partitionTypeDescription(l, p));
        if (l.type == TableType::Gpt) {
            w.key("typeGuid").value(p.typeGuid.toString());
            w.key("partitionGuid").value(p.uniqueGuid.toString());
            w.key("name").value(p.name);
            w.key("attributes").value(p.gptAttributes);
        }
        w.key("filesystem").value(fsTypeName(p.fs.type));
        w.key("label").value(p.fs.label);
        w.key("fsUuid").value(p.fs.uuid);
        w.key("flags").beginArray();
        for (const auto& f : partitionFlags(l, p)) w.value(f);
        w.endArray();
        w.key("systemCritical").value(isSystemCriticalType(l, p));
        w.endObject();
    }
    w.endArray();
    w.key("diagnostics").beginArray();
    for (const auto& d : l.diagnostics) {
        w.beginObject();
        w.key("severity").value(severityName(d.severity));
        w.key("code").value(d.code);
        w.key("message").value(d.message);
        w.endObject();
    }
    w.endArray();
    w.endObject();
    return w.str();
}

}  // namespace wparted
