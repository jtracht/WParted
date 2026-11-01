// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// wparted-helper.exe - the execution process of ADR-0002 (WP-023). Started by
// wparted.exe / wparted-cli.exe with `--pipe <name>`, it talks only to the
// process that started it, validates the plan itself against what it reads,
// and executes only after a `confirm` carrying the SHA-256 of the exact plan it
// validated. Image files need no special rights. For a disk the client starts it
// through UAC; the gate in disk_write_gate.hpp decides whether that disk may be
// written (real disks stay refused unless the build enables them).
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>

#include "wparted/core/executor.hpp"
#include "wparted/core/helper_protocol.hpp"
#include "wparted/core/operation_log.hpp"
#include "wparted/win/disk_discovery.hpp"
#include "wparted/win/disk_writer.hpp"
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

int runMount(win::Pipe& pipe, const helper::Message& m) {
    if (!win::isProcessElevated()) return finish(pipe, "failed", "changing mount points needs administrator rights");
    if (!isVolumeGuidPath(m.volume)) return finish(pipe, "failed", "not a volume name");
    if (!m.mountPoint.empty() && !isDriveLetterMountPoint(m.mountPoint)) return finish(pipe, "failed", "not a drive letter");
    const win::DiscoveryResult found = win::discoverDisks();
    for (const auto& d : found.disks)
        for (const auto& v : d.volumes) {
            if (win::narrow(v.guidPath) != m.volume) continue;
            if (v.containsWindows || v.isPageFileVolume || v.isSystemPartition)
                return finish(pipe, "failed", "this volume belongs to the running Windows and is not changed");
            std::string err;
            const bool ok = m.mountPoint.empty() ? win::removeMountPoints(v, err) : win::addMountPoint(m.volume, m.mountPoint, err);
            return ok ? finish(pipe, "success", "") : finish(pipe, "failed", err);
        }
    return finish(pipe, "failed", "the volume was not found");
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
    if (!receive(pipe, plan, kTimeout, err)) return finish(pipe, "failed", "expected a request: " + err);
    if (plan.type == helper::Type::Mount) return runMount(pipe, plan);
    if (plan.type != helper::Type::Plan && plan.type != helper::Type::Resume) return finish(pipe, "failed", "expected plan or resume");
    const bool resume = plan.type == helper::Type::Resume;
    if (plan.image.empty()) return finish(pipe, "failed", "no target");

    // A disk: everything about it comes from this process, never from the client.
    const bool isDisk = isDevicePath(plan.image);
    win::PhysicalDisk disk;
    win::PhysicalDiskDevice diskDevice;
    std::string journalPath;
    if (isDisk) {
        unsigned number = 0;
        if (!parsePhysicalDrivePath(plan.image, number)) return finish(pipe, "failed", "only \\\\.\\PhysicalDriveN can be written");
        bool foundDisk = false;
        win::DiscoveryResult found = win::discoverDisks();
        for (auto& d : found.disks)
            if (d.number == number) {
                disk = d;
                foundDisk = true;
            }
        if (!foundDisk) return finish(pipe, "failed", "the disk was not found");
        const std::string problem = diskWriteProblem(win::writeFactsFor(disk, win::isProcessElevated()));
        if (!problem.empty()) return finish(pipe, "failed", "execution.unavailable: " + problem + "; nothing was written");
        if (plan.sectorSize != disk.logicalSectorSize) return finish(pipe, "failed", "the sector size does not match the disk");
        if (!win::prepareJournalFolder(err)) return finish(pipe, "failed", err);
        if (!diskDevice.open(disk, err)) return finish(pipe, "failed", err);
        journalPath = win::journalPathForDisk(disk);
    } else {
        if (plan.sectorSize != 512 && plan.sectorSize != 4096) return finish(pipe, "failed", "unsupported sector size");
        journalPath = journalPathFor(plan.image);
    }
    auto diskContext = [&disk](const DiskLayout& layout) { return win::contextForDisk(disk, layout, ValidationMode::DeviceExecution); };

    // The helper's own validation, against what it reads now.
    helper::Message verdict;
    verdict.type = helper::Type::Validation;
    verdict.planHash = helper::sha256Hex(plan.plan);
    OperationQueue q;
    if (resume) {
        // resumePlanOnImage checks the target against the journal before it writes.
        JournalState journal;
        if (!readJournal(journalPath, journal, err)) return finish(pipe, "failed", err);
        verdict.planHash = helper::sha256Hex(journal.planJson);
        verdict.ok = journal.resumable();
        if (!verdict.ok) verdict.diagnostics.emplace_back(Severity::Error, "journal.not_resumable", "the interrupted step cannot be resumed");
    } else {
        FileBlockReader fileReader;
        if (!isDisk && !fileReader.open(plan.image, plan.sectorSize)) return finish(pipe, "failed", "the image file cannot be read");
        BlockReader& r = isDisk ? static_cast<BlockReader&>(diskDevice) : fileReader;
        const DiskLayout now = parseDiskLayout(r, true);
        if (!OperationQueue::fromJson(plan.plan, now, q, err)) return finish(pipe, "failed", "the plan does not fit the target: " + err);
        ValidationContext ctx;
        if (isDisk) {
            ctx = diskContext(now);
        } else {
            ctx.mode = ValidationMode::ImageExecution;
            ctx.haveCurrentIdentity = true;
            ctx.currentIdentity = identityOf(now, q.identity().serial, q.identity().model);
        }
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

    ImageFileDevice imageDevice;
    win::VolumeLocks locks;
    if (isDisk) {
        if (!locks.lockAll(disk, err)) return finish(pipe, "failed", err + "; nothing was written");
    } else if (!imageDevice.open(plan.image, plan.sectorSize, err)) {
        return finish(pipe, "failed", err);
    }
    BlockDevice& dev = isDisk ? static_cast<BlockDevice&>(diskDevice) : imageDevice;
    std::atomic<bool> cancel{false};
    ExecuteOptions opt;
    opt.journalPath = journalPath;
    opt.cancel = &cancel;
    if (isDisk) opt.deviceContext = diskContext;
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
    const ExecuteResult res = resume ? resumePlanOnImage(dev, plan.image, opt) : executePlanOnImage(dev, q, plan.image, opt);
    std::string refreshError;
    if (isDisk) {  // Windows must see the new table, also after a failure
        diskDevice.updateProperties(refreshError);
        locks.release();
    }
    helper::Message log;
    log.type = helper::Type::Log;
    log.log = logToJson(res.log);
    send(pipe, log);
    if (res.ok) return finish(pipe, "success", refreshError);
    return finish(pipe, cancel ? "cancelled" : "failed", res.error);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // Elevated from a user-writable folder: never load DLLs from there.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (argc == 3 && std::wstring(argv[1]) == L"--pipe") return run(argv[2]);
    std::fprintf(stderr, "wparted-helper %s: started by WParted only (wparted-helper --pipe <name>)\n", WPARTED_VERSION_STRING);
    return 2;
}
