// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Client side of ADR-0002, shared by wparted.exe and wparted-cli.exe: starts
// wparted-helper.exe next to the running executable, runs the message flow
// (hello, plan, validation, confirm with the plan hash, progress, log, done)
// and reports back. The caller never writes to the image itself.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "wparted/core/partition_table.hpp"

namespace wparted::win {

struct HelperRun {
    bool started = false;            // the helper ran and talked the protocol
    std::string helperVersion;
    bool validationOk = false;
    std::vector<Diagnostic> diagnostics;  // the helper's own validation
    std::string planHash;            // SHA-256 the helper validated and the caller confirmed
    std::string logJson;             // final operation log (logToJson), if any
    std::string result;              // success | failed | cancelled ("" if the helper never answered)
    std::string error;
};

struct HelperCallbacks {
    std::function<void(std::uint64_t step, std::uint64_t done, std::uint64_t total)> progress;
    const std::atomic<bool>* cancel = nullptr;  // sends `cancel` at the next progress message
};

bool helperAvailable();  // wparted-helper.exe exists next to the running executable

HelperRun applyViaHelper(const std::string& image, const std::string& planText, std::uint32_t sectorSize,
                         const HelperCallbacks& callbacks = {});
// Continues the execution recorded in the image's journal; `planText` is the
// plan stored there, which the helper must have read as well.
HelperRun resumeViaHelper(const std::string& image, const std::string& planText, std::uint32_t sectorSize,
                          const HelperCallbacks& callbacks = {});

}  // namespace wparted::win
