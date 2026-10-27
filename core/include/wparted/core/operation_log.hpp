// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Hierarchical operation log (GParted "Save Details"): one step per
// operation with status, timing and messages; exported as JSON and as a
// self-contained HTML document.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wparted/core/operations.hpp"
#include "wparted/core/validation.hpp"

namespace wparted {

struct LogStep {
    enum class Status { Pending, Running, Success, Warning, Error, Skipped };
    std::string title;
    Status status = Status::Pending;
    std::int64_t startMs = 0, endMs = 0;  // Unix epoch milliseconds, 0 = not started
    std::vector<std::string> messages;
    std::vector<LogStep> children;
};

const char* logStatusName(LogStep::Status s) noexcept;

// Log of a plan that was validated but not executed (this build has no
// executor): every operation is Skipped, validation messages are attached.
LogStep planLog(const OperationQueue& queue, const ValidationResult& validation, const std::string& deviceName);

std::string logToJson(const LogStep& root);
std::string logToHtml(const LogStep& root, const std::string& title);
std::string htmlEscape(const std::string& s);

}  // namespace wparted
