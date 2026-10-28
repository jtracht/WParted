// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// wparted-helper.exe - the execution process of ADR-0002 (prototype, WP-023).
// Started by wparted.exe / wparted-cli.exe with `--pipe <name>`, it talks only
// to the process that started it, validates the plan itself against what it
// reads, and executes only after a `confirm` carrying the SHA-256 of the exact
// plan it validated. This prototype executes on disk image files only:
// physical disks are refused (they need the human-reviewed write path, the
// manifest change to requireAdministrator and the UAC start of ADR-0002).
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>

#include "wparted/core/executor.hpp"
#include "wparted/core/helper_protocol.hpp"
#include "wparted/core/operation_log.hpp"
#include "wparted/win/pipe.hpp"
#include "wparted_version.h"

using namespace wparted;

namespace {

constexpr unsigned long kTimeout = 30 * 1000;
constexpr unsigned long kConfirmTimeout = 10 * 60 * 1000;  // the user reads the result first

bool send(win::Pipe& p, const helper::Message& m) {
    std::string err;
    return p.send(helper::encode(m), err);
}

bool receive(win::Pipe& p, helper::Message& m, unsigned long timeout, std::string& err) {
    std::string text;
    return p.receive(text, timeout, err) && helper::decode(text, m, err);
}

int finish(win::Pipe& p, const char* result, const std::string& error) {
    helper::Message done;
    done.type = helper::Type::Done;
    done.result = result;
    done.error = error;
    send(p, done);
    return std::string(result) == "success" ? 0 : 1;
}

int run(const std::wstring& pipeName) {
    win::Pipe pipe;
    std::string err;
    if (!pipe.open(pipeName, err) || !pipe.verifyServerIsParent(err)) {
        std::fprintf(stderr, "wparted-helper: %s\n", err.c_str());
        return 2;  // nothing was sent or read
    }
    helper::Message m;
    if (!receive(pipe, m, kTimeout, err) || m.type != helper::Type::Hello) return finish(pipe, "failed", "expected hello: " + err);
    helper::Message hello;
    hello.type = helper::Type::Hello;
    hello.protocol = helper::kProtocol;
    hello.version = WPARTED_VERSION_STRING;
    if (!send(pipe, hello)) return 2;
    if (m.protocol != helper::kProtocol) return finish(pipe, "failed", "protocol version mismatch");

    helper::Message plan;
    if (!receive(pipe, plan, kTimeout, err) || plan.type != helper::Type::Plan) return finish(pipe, "failed", "expected plan: " + err);
    if (plan.image.empty() || isDevicePath(plan.image))
        return finish(pipe, "failed", "execution.unavailable: this helper executes on disk image files only");
    if (plan.sectorSize != 512 && plan.sectorSize != 4096) return finish(pipe, "failed", "unsupported sector size");

    // The helper's own validation, against what it reads now.
    helper::Message verdict;
    verdict.type = helper::Type::Validation;
    verdict.planHash = helper::sha256Hex(plan.plan);
    OperationQueue q;
    {
        FileBlockReader r;
        if (!r.open(plan.image, plan.sectorSize)) return finish(pipe, "failed", "the image file cannot be read");
        const DiskLayout now = parseDiskLayout(r, true);
        if (!OperationQueue::fromJson(plan.plan, now, q, err)) return finish(pipe, "failed", "the plan does not fit the image: " + err);
        ValidationContext ctx;
        ctx.mode = ValidationMode::ImageExecution;
        ctx.haveCurrentIdentity = true;
        ctx.currentIdentity = identityOf(now, q.identity().serial, q.identity().model);
        addImageUsage(r, q.original(), ctx);
        const ValidationResult vr = validatePlan(q, ctx);
        verdict.ok = vr.ok();
        verdict.diagnostics = vr.diagnostics;
    }
    if (!send(pipe, verdict)) return 2;
    if (!verdict.ok) return finish(pipe, "failed", "the plan did not pass the helper's validation; nothing was written");

    helper::Message confirm;
    if (!receive(pipe, confirm, kConfirmTimeout, err)) return finish(pipe, "cancelled", "no confirmation; nothing was written");
    if (confirm.type == helper::Type::Cancel) return finish(pipe, "cancelled", "cancelled before execution; nothing was written");
    if (confirm.type != helper::Type::Confirm || confirm.planHash != verdict.planHash)
        return finish(pipe, "failed", "the confirmation does not match the validated plan; nothing was written");

    ImageFileDevice dev;
    if (!dev.open(plan.image, plan.sectorSize, err)) return finish(pipe, "failed", err);
    std::atomic<bool> cancel{false};
    ExecuteOptions opt;
    opt.journalPath = journalPathFor(plan.image);
    opt.cancel = &cancel;
    opt.progress = [&](std::size_t op, std::uint64_t done, std::uint64_t total) {
        helper::Message pr;
        pr.type = helper::Type::Progress;
        pr.step = op;
        pr.done = done;
        pr.total = total;
        send(pipe, pr);
        if (pipe.pending()) {  // only `cancel` is allowed during execution
            helper::Message c;
            std::string e;
            if (receive(pipe, c, 1000, e) && c.type == helper::Type::Cancel) cancel = true;
        }
    };
    const ExecuteResult res = executePlanOnImage(dev, q, plan.image, opt);
    helper::Message log;
    log.type = helper::Type::Log;
    log.log = logToJson(res.log);
    send(pipe, log);
    if (res.ok) return finish(pipe, "success", "");
    return finish(pipe, cancel ? "cancelled" : "failed", res.error);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring(argv[1]) == L"--pipe") return run(argv[2]);
    std::fprintf(stderr, "wparted-helper %s: started by WParted only (wparted-helper --pipe <name>)\n", WPARTED_VERSION_STRING);
    return 2;
}
