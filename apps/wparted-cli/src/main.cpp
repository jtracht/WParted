// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// wparted-cli: command line front end for scripting, diagnostics reports and
// testing the core against disk images. Physical disks are only ever read;
// `apply-plan --execute` writes to a user-chosen disk image *file* only.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "wparted/core/executor.hpp"
#include "wparted/core/filesystem.hpp"
#include "wparted/core/json.hpp"
#include "wparted/core/partition_table.hpp"
#include "wparted/core/rescue.hpp"
#include "wparted/win/disk_discovery.hpp"
#include "wparted/win/report.hpp"
#include "wparted_version.h"

using namespace wparted;

namespace {

void usage() {
    std::printf(
        "WParted CLI %s\n\n"
        "Usage:\n"
        "  wparted-cli list [--json]                      List disks, partitions and volumes\n"
        "  wparted-cli inspect-image <file> [--sector-size N] [--json]\n"
        "                                                 Analyse a raw disk image file\n"
        "  wparted-cli fs-support [--json]                Show file system capabilities of this build\n"
        "  wparted-cli rescue-scan-image <file> [--sector-size N] [--json] [--plan out.json]\n"
        "                                                 Search unallocated space for lost file systems\n"
        "  wparted-cli apply-plan <image> <plan.json> [--sector-size N] [--execute] [--log file.html]\n"
        "                                                 Check a saved plan against a disk image file;\n"
        "                                                 with --execute, apply it to that image file\n"
        "  wparted-cli resume <image> [--execute | --discard] [--log file.html]\n"
        "                                                 Show an interrupted execution; continue it with\n"
        "                                                 --execute, or delete its journal with --discard\n"
        "  wparted-cli version                            Print version\n\n"
        "Only apply-plan --execute writes, and only to an image file; physical disks are never written.\n"
        "Raw partition table access to physical disks requires an elevated prompt.\n",
        WPARTED_VERSION_STRING);
}

const char* yesNo(win::Tristate t) { return win::tristateName(t); }

void printLayoutText(const DiskLayout& l, const win::PhysicalDisk* disk) {
    std::printf("  Partition table: %s, logical sector %u bytes\n", tableTypeName(l.type), l.sectorSize);
    if (l.type == TableType::Gpt) std::printf("  Disk GUID: %s\n", l.diskGuid.toString().c_str());
    std::printf("  %-4s %-9s %-14s %-12s %-12s %-28s %s\n", "#", "Kind", "File system", "Start (LBA)", "Size", "Type",
                "Label / Name / Mount");
    for (const auto& p : l.partitions) {
        std::string extra = p.fs.label;
        if (!p.name.empty()) extra += (extra.empty() ? "" : " / ") + p.name;
        if (disk) {
            if (const auto* v = win::volumeForPartition(*disk, p)) {
                for (const auto& m : v->mountPoints) extra += " [" + win::narrow(m) + "]";
                if (v->containsWindows) extra += " (Windows)";
                if (v->isSystemPartition) extra += " (boot manager)";
                if (v->isPageFileVolume) extra += " (page file)";
                if (v->bitLocker != BitLockerState::NotReported && v->bitLocker != BitLockerState::NotEncrypted)
                    extra += std::string(" (BitLocker: ") + bitLockerStateName(v->bitLocker) + ")";
            }
        }
        std::string flags;
        for (const auto& f : partitionFlags(l, p)) flags += (flags.empty() ? "" : ",") + f;
        if (!flags.empty()) extra += " {" + flags + "}";
        std::printf("  %-4s %-9s %-14s %-12llu %-12s %-28s %s\n", p.number > 0 ? std::to_string(p.number).c_str() : "-", partitionKindName(p.kind), fsTypeName(p.fs.type),
                    static_cast<unsigned long long>(p.firstLba), formatSize(p.sectorCount * l.sectorSize).c_str(),
                    partitionTypeDescription(l, p).substr(0, 28).c_str(), extra.c_str());
    }
    for (const auto& d : l.diagnostics)
        std::printf("  %s [%s] %s\n", d.severity == Severity::Error ? "ERROR" : d.severity == Severity::Warning ? "WARNING" : "INFO",
                    d.code.c_str(), d.message.c_str());
}

int cmdList(bool json) {
    win::DiscoveryResult r = win::discoverDisks();
    if (json) {
        std::printf("%s\n", win::discoveryToJson(r, WPARTED_VERSION_STRING).c_str());
        return 0;
    }
    if (!r.elevated)
        std::printf("Note: not elevated. Partition tables cannot be read; run from an administrator prompt for full details.\n\n");
    for (const auto& d : r.disks) {
        std::printf("%s  %s  %s  %s%s%s\n", d.displayName().c_str(), d.modelName().c_str(), formatSize(d.sizeBytes).c_str(),
                    d.busType.c_str(), d.containsWindows() ? "  [contains Windows]" : "",
                    d.layoutAvailable && isManagedByWindows(d.layout) ? "  [managed by Windows: dynamic disk / Storage Spaces]" : "");
        std::printf("  Sectors: logical %u, physical %s; SSD: %s; removable media: %s; write protected: %s\n",
                    d.logicalSectorSize, d.physicalSectorSize ? std::to_string(d.physicalSectorSize).c_str() : "unknown",
                    yesNo(d.solidState), d.removableMedia ? "yes" : "no", yesNo(d.writeProtected));
        if (d.layoutAvailable) {
            printLayoutText(d.layout, &d);
        } else {
            std::printf("  Partition table not readable: %s\n", d.accessError.c_str());
            printLayoutText(win::layoutFromVolumes(d), &d);
        }
        std::printf("\n");
    }
    std::printf("%zu disk(s), discovery took %.0f ms\n", r.disks.size(), r.durationMs);
    return 0;
}

int cmdInspectImage(const std::string& path, std::uint32_t sectorSize, bool json) {
    FileBlockReader reader;
    if (!reader.open(path, sectorSize)) {
        std::fprintf(stderr, "error: cannot open image '%s' for reading\n", path.c_str());
        return 2;
    }
    DiskLayout l = parseDiskLayout(reader, true);
    if (json) {
        std::printf("%s\n", layoutToJson(l).c_str());
    } else {
        std::printf("%s  %s\n", path.c_str(), formatSize(l.diskBytes).c_str());
        printLayoutText(l, nullptr);
    }
    return l.hasErrors() ? 1 : 0;
}

int cmdRescueImage(const std::string& path, std::uint32_t sectorSize, bool json, const std::string& planPath) {
    FileBlockReader reader;
    if (!reader.open(path, sectorSize)) {
        std::fprintf(stderr, "error: cannot open image '%s' for reading\n", path.c_str());
        return 2;
    }
    const DiskLayout l = parseDiskLayout(reader, true);
    const auto found = rescueScan(reader, l);
    if (!planPath.empty()) {  // a plan that recreates the found partitions (applied with apply-plan)
        OperationQueue q(l, identityOf(l, "", "image"));
        std::string err;
        const std::size_t n = planRecreate(q, found, err);
        if (!err.empty()) std::fprintf(stderr, "warning: %s\n", err.c_str());
        std::ofstream out(planPath, std::ios::binary);  // readonly-guard: allow (user-chosen plan file)
        out << q.toJson();
        if (!out) {
            std::fprintf(stderr, "error: cannot write plan '%s'\n", planPath.c_str());
            return 2;
        }
        std::printf("Plan with %zu partition(s) to recreate written to %s; check it with apply-plan.\n", n, planPath.c_str());
    }
    if (json) {
        JsonWriter w;
        w.beginArray();
        for (const auto& c : found) {
            w.beginObject();
            w.key("firstLba").value(c.firstLba);
            w.key("sectorCount").value(c.sectorCount);
            w.key("filesystem").value(fsTypeName(c.fs.type));
            w.key("label").value(c.fs.label);
            w.endObject();
        }
        w.endArray();
        std::printf("%s\n", w.str().c_str());
        return 0;
    }
    std::printf("%zu candidate(s) in unallocated space of %s (read-only scan)\n", found.size(), path.c_str());
    for (const auto& c : found)
        std::printf("  sector %-12llu %-12s %-14s %s\n", static_cast<unsigned long long>(c.firstLba), fsTypeName(c.fs.type),
                    c.sectorCount ? formatSize(c.sectorCount * l.sectorSize).c_str() : "size unknown", c.fs.label.c_str());
    return 0;
}

int printResult(const ExecuteResult& res, const std::string& image, const std::string& logPath);

ExecuteOptions executeOptions(const std::string& journal) {
    ExecuteOptions opt;
    opt.journalPath = journal;
    opt.progress = [](std::size_t op, std::uint64_t done, std::uint64_t total) {
        std::fprintf(stderr, "\r  operation %zu: %3u %%", op + 1, static_cast<unsigned>(total ? done * 100 / total : 100));
        if (done == total) std::fprintf(stderr, "\n");
    };
    return opt;
}

int cmdApplyPlan(const std::string& image, const std::string& planPath, std::uint32_t ss, bool execute,
                 const std::string& logPath) {
    if (isDevicePath(image)) {
        std::fprintf(stderr, "error: apply-plan works on disk image files only; '%s' is a device\n", image.c_str());
        return 2;
    }
    const std::string journal = journalPathFor(image);
    if (journalExists(journal)) {
        std::fprintf(stderr,
                     "error: an interrupted execution was found for %s.\n"
                     "       Run 'wparted-cli resume %s' to see it and continue it, or add --discard to delete the\n"
                     "       journal. Nothing was written.\n",
                     image.c_str(), image.c_str());
        return 4;
    }
    std::string json, err;
    {
        std::ifstream f(planPath, std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "error: cannot read plan '%s'\n", planPath.c_str());
            return 2;
        }
        std::ostringstream text;
        text << f.rdbuf();
        json = text.str();
    }
    DiskLayout original;
    ValidationContext ctx;
    OperationQueue q;
    {
        FileBlockReader r;  // closed before the exclusive write handle is opened
        if (!r.open(image, ss)) {
            std::fprintf(stderr, "error: cannot open image '%s'\n", image.c_str());
            return 2;
        }
        original = parseDiskLayout(r, true);
        if (!OperationQueue::fromJson(json, original, q, err)) {
            std::fprintf(stderr, "error: the plan does not fit this image: %s\n", err.c_str());
            return 3;
        }
        addImageUsage(r, q.original(), ctx);
    }
    if (!execute) {
        const auto descriptions = q.descriptions();
        for (std::size_t i = 0; i < descriptions.size(); ++i)
            std::printf("%zu. [%s] %s\n", i + 1, riskName(riskOf(q.operations()[i])), descriptions[i].c_str());
        ctx.mode = ValidationMode::ImageExecution;
        ctx.haveCurrentIdentity = true;
        ctx.currentIdentity = identityOf(original, q.identity().serial, q.identity().model);
        const ValidationResult vr = validatePlan(q, ctx);
        for (const auto& d : vr.diagnostics)
            std::printf("%s [%s] %s\n", d.severity == Severity::Error ? "ERROR" : d.severity == Severity::Warning ? "WARNING" : "INFO",
                        d.code.c_str(), d.message.c_str());
        std::printf(vr.ok() ? "Check passed. Nothing was written; run again with --execute to apply the plan to %s.\n"
                            : "Check failed. Nothing was written to %s.\n",
                    image.c_str());
        return vr.ok() ? 0 : 3;
    }
    ImageFileDevice dev;
    if (!dev.open(image, ss, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 2;
    }
    return printResult(executePlanOnImage(dev, q, image, executeOptions(journal)), image, logPath);
}

int cmdResume(const std::string& image, std::uint32_t ss, bool execute, bool discard, const std::string& logPath) {
    if (isDevicePath(image)) {
        std::fprintf(stderr, "error: resume works on disk image files only; '%s' is a device\n", image.c_str());
        return 2;
    }
    const std::string journal = journalPathFor(image);
    if (!journalExists(journal)) {
        std::printf("No interrupted execution for %s.\n", image.c_str());
        return 0;
    }
    JournalState s;
    std::string err;
    if (!readJournal(journal, s, err)) {
        std::fprintf(stderr, "error: %s (%s)\n", err.c_str(), journal.c_str());
        return 2;
    }
    std::printf("Interrupted execution on %s:\n  %zu of %zu operation(s) were applied and verified.\n", image.c_str(),
                s.completed, s.operationCount);
    if (s.phase == "copy")
        std::printf("  Operation %zu was interrupted during its block copy after %s.\n", s.operation + 1,
                    formatSize(s.copyDone).c_str());
    if (s.phase == "work") std::printf("  Operation %zu was interrupted while %s.\n", s.operation + 1, s.step.c_str());
    if (discard) {
        if (!discardJournal(journal, err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 2;
        }
        std::printf("The journal was deleted; the image was not changed.\n");
        return 0;
    }
    if (!s.resumable()) {
        std::printf("That step changes data in place and cannot be resumed; the image may be inconsistent.\n"
                    "Restore the image from a backup, then delete the journal with --discard.\n");
        return 3;
    }
    if (!execute) {
        std::printf("It can be resumed: run again with --execute to continue, or --discard to delete the journal.\n");
        return 0;
    }
    ImageFileDevice dev;
    if (!dev.open(image, ss, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 2;
    }
    return printResult(resumePlanOnImage(dev, image, executeOptions(journal)), image, logPath);
}

int printResult(const ExecuteResult& res, const std::string& image, const std::string& logPath) {
    for (const auto& d : res.validation.diagnostics)
        if (d.severity != Severity::Info)
            std::printf("%s [%s] %s\n", d.severity == Severity::Error ? "ERROR" : "WARNING", d.code.c_str(), d.message.c_str());
    for (std::size_t i = 0; i < res.log.children.size(); ++i) {
        const LogStep& s = res.log.children[i];
        std::printf("%zu. %-8s %s\n", i + 1, logStatusName(s.status), s.title.c_str());
        for (const auto& m : s.messages) std::printf("             %s\n", m.c_str());
    }
    if (!logPath.empty()) {
        std::ofstream out(logPath, std::ios::binary);  // readonly-guard: allow (user-chosen log file)
        out << logToHtml(res.log, "WParted - " + image);
    }
    if (!res.ok) {
        std::fprintf(stderr, "error: %s\n", res.error.c_str());
        return 1;
    }
    std::printf("Done: %zu operation(s) applied to %s and verified.\n", res.completed, image.c_str());
    return 0;
}

int cmdFsSupport(bool json) {
    if (json) {
        JsonWriter w;
        w.beginArray();
        for (FsType t : allFsTypes()) {
            const auto& c = capabilitiesFor(t);
            w.beginObject();
            w.key("filesystem").value(fsTypeName(t));
            w.key("detect").value(supportName(c.detect));
            w.key("readUsage").value(supportName(c.readUsage));
            w.key("create").value(supportName(c.create));
            w.key("grow").value(supportName(c.grow));
            w.key("shrink").value(supportName(c.shrink));
            w.key("move").value(supportName(c.move));
            w.key("copy").value(supportName(c.copy));
            w.key("check").value(supportName(c.check));
            w.key("label").value(supportName(c.label));
            w.key("uuid").value(supportName(c.uuid));
            w.endObject();
        }
        w.endArray();
        std::printf("%s\n", w.str().c_str());
        return 0;
    }
    std::printf("%-14s %-14s %-28s %s\n", "File system", "Detect", "Usage", "Changes on image files");
    for (FsType t : allFsTypes()) {
        const auto& c = capabilitiesFor(t);
        std::string actions;
        const std::pair<Support, const char*> list[] = {{c.create, "create"}, {c.grow, "grow"},   {c.shrink, "shrink"},
                                                        {c.move, "move"},     {c.copy, "copy"},   {c.check, "check"},
                                                        {c.label, "label"},   {c.uuid, "uuid"}};
        for (const auto& a : list)
            if (a.first == Support::Supported || a.first == Support::Limited)
                actions += (actions.empty() ? "" : ", ") + std::string(a.second) + (a.first == Support::Limited ? "*" : "");
        std::printf("%-14s %-14s %-28s %s\n", fsTypeName(t), supportName(c.detect), supportName(c.readUsage),
                    actions.empty() ? "-" : actions.c_str());
    }
    std::printf("\n* with limitations (moves and copies are raw and keep the size; see the user guide).\n"
                "Physical disks are never written by this version.\n");
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(win::narrow(argv[i]));
    auto has = [&](const char* flag) {
        for (const auto& a : args)
            if (a == flag) return true;
        return false;
    };
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        usage();
        return args.empty() ? 1 : 0;
    }
    const bool json = has("--json");
    if (args[0] == "version" || args[0] == "--version") {
        std::printf("%s\n", WPARTED_VERSION_STRING);
        return 0;
    }
    if (args[0] == "list") return cmdList(json);
    if (args[0] == "fs-support") return cmdFsSupport(json);
    if (args[0] == "rescue-scan-image") {
        if (args.size() < 2) {
            usage();
            return 2;
        }
        std::uint32_t ss = 512;
        std::string planPath;
        for (std::size_t i = 2; i + 1 < args.size(); ++i) {
            if (args[i] == "--sector-size") ss = static_cast<std::uint32_t>(std::strtoul(args[i + 1].c_str(), nullptr, 10));
            if (args[i] == "--plan") planPath = args[i + 1];
        }
        return cmdRescueImage(args[1], ss, json, planPath);
    }
    if (args[0] == "apply-plan") {
        if (args.size() < 3) {
            usage();
            return 2;
        }
        std::uint32_t ss = 512;
        std::string logPath;
        for (std::size_t i = 3; i + 1 < args.size(); ++i) {
            if (args[i] == "--sector-size") ss = static_cast<std::uint32_t>(std::strtoul(args[i + 1].c_str(), nullptr, 10));
            if (args[i] == "--log") logPath = args[i + 1];
        }
        return cmdApplyPlan(args[1], args[2], ss, has("--execute"), logPath);
    }
    if (args[0] == "resume") {
        if (args.size() < 2) {
            usage();
            return 2;
        }
        std::uint32_t ss = 512;
        std::string logPath;
        for (std::size_t i = 2; i + 1 < args.size(); ++i) {
            if (args[i] == "--sector-size") ss = static_cast<std::uint32_t>(std::strtoul(args[i + 1].c_str(), nullptr, 10));
            if (args[i] == "--log") logPath = args[i + 1];
        }
        return cmdResume(args[1], ss, has("--execute"), has("--discard"), logPath);
    }
    if (args[0] == "inspect-image") {
        if (args.size() < 2) {
            usage();
            return 2;
        }
        std::uint32_t ss = 512;
        for (std::size_t i = 2; i + 1 < args.size(); ++i)
            if (args[i] == "--sector-size") ss = static_cast<std::uint32_t>(std::strtoul(args[i + 1].c_str(), nullptr, 10));
        if (ss != 512 && ss != 1024 && ss != 2048 && ss != 4096) {
            std::fprintf(stderr, "error: unsupported sector size %u\n", ss);
            return 2;
        }
        return cmdInspectImage(args[1], ss, json);
    }
    std::fprintf(stderr, "error: unknown command '%s'\n\n", args[0].c_str());
    usage();
    return 2;
}
