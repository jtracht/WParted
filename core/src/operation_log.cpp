// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#include "wparted/core/operation_log.hpp"

#include "wparted/core/json.hpp"

namespace wparted {

const char* logStatusName(LogStep::Status s) noexcept {
    switch (s) {
        case LogStep::Status::Pending: return "pending";
        case LogStep::Status::Running: return "running";
        case LogStep::Status::Success: return "success";
        case LogStep::Status::Warning: return "warning";
        case LogStep::Status::Error: return "error";
        case LogStep::Status::Skipped: return "skipped";
    }
    return "pending";
}

LogStep planLog(const OperationQueue& queue, const ValidationResult& validation, const std::string& deviceName) {
    LogStep root;
    root.title = "Plan for " + deviceName;
    root.status = validation.ok() ? LogStep::Status::Skipped : LogStep::Status::Error;
    root.messages.push_back("Validated only: this build of WParted does not execute operations.");
    const auto descriptions = queue.descriptions();
    for (std::size_t i = 0; i < descriptions.size(); ++i) {
        LogStep step;
        step.title = std::to_string(i + 1) + ". " + descriptions[i];
        step.status = LogStep::Status::Skipped;
        step.messages.push_back(std::string("Risk: ") + riskName(riskOf(queue.operations()[i])));
        const std::string prefix = "Operation " + std::to_string(i + 1) + ":";
        for (const auto& d : validation.diagnostics)
            if (d.message.rfind(prefix, 0) == 0) {
                step.messages.push_back(d.message);
                if (d.severity == Severity::Error) step.status = LogStep::Status::Error;
                else if (step.status == LogStep::Status::Skipped) step.status = LogStep::Status::Warning;
            }
        root.children.push_back(std::move(step));
    }
    for (const auto& d : validation.diagnostics)
        if (d.message.rfind("Operation ", 0) != 0)
            root.messages.push_back(std::string(d.severity == Severity::Error ? "Error: " : "Warning: ") + d.message);
    return root;
}

namespace {

void stepJson(JsonWriter& w, const LogStep& s) {
    w.beginObject();
    w.key("title").value(s.title);
    w.key("status").value(logStatusName(s.status));
    w.key("startMs").value(s.startMs);
    w.key("endMs").value(s.endMs);
    w.key("messages").beginArray();
    for (const auto& m : s.messages) w.value(m);
    w.endArray();
    w.key("children").beginArray();
    for (const auto& c : s.children) stepJson(w, c);
    w.endArray();
    w.endObject();
}

void stepHtml(std::string& out, const LogStep& s) {
    out += "<li class=\"" + std::string(logStatusName(s.status)) + "\"><span class=\"st\">" +
           logStatusName(s.status) + "</span> " + htmlEscape(s.title);
    if (!s.messages.empty()) {
        out += "<ul class=\"msg\">";
        for (const auto& m : s.messages) out += "<li>" + htmlEscape(m) + "</li>";
        out += "</ul>";
    }
    if (!s.children.empty()) {
        out += "<ol>";
        for (const auto& c : s.children) stepHtml(out, c);
        out += "</ol>";
    }
    out += "</li>";
}

}  // namespace

std::string logToJson(const LogStep& root) {
    JsonWriter w;
    stepJson(w, root);
    return w.str();
}

std::string htmlEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&#39;"; break;
            default: o += c;
        }
    }
    return o;
}

std::string logToHtml(const LogStep& root, const std::string& title) {
    std::string out =
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>" + htmlEscape(title) +
        "</title><style>body{font:14px/1.5 'Segoe UI',system-ui,sans-serif;margin:24px;color:#1a1a1a}"
        "li{margin:4px 0}.st{display:inline-block;min-width:64px;padding:0 6px;border-radius:8px;font-size:12px;"
        "background:#eee}.error>.st{background:#fde7e9;color:#c42b1c}.warning>.st{background:#fff4ce;color:#9d5d00}"
        ".success>.st{background:#dff6dd;color:#0f7b0f}.msg{color:#5d5d5d}</style></head><body><h1>" +
        htmlEscape(title) + "</h1><ul>";
    stepHtml(out, root);
    out += "</ul></body></html>\n";
    return out;
}

}  // namespace wparted
