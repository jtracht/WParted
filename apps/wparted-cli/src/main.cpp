// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// wparted-cli: read-only command line front end. Useful for scripting,
// diagnostics reports and for testing the core against disk images.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "wparted/core/filesystem.hpp"
#include "wparted/core/json.hpp"
#include "wparted/core/partition_table.hpp"
#include "wparted/win/disk_discovery.hpp"
#include "wparted/win/report.hpp"
#include "wparted_version.h"

using namespace wparted;

namespace {

void usage() {
    std::printf(
        "WParted CLI %s (read-only)\n\n"
        "Usage:\n"
        "  wparted-cli list [--json]                      List disks, partitions and volumes\n"
        "  wparted-cli inspect-image <file> [--sector-size N] [--json]\n"
        "                                                 Analyse a raw disk image file\n"
        "  wparted-cli fs-support [--json]                Show file system capabilities of this build\n"
        "  wparted-cli version                            Print version\n\n"
        "No command of this version modifies any disk or file.\n"
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
        std::printf("%s  %s  %s  %s%s\n", d.displayName().c_str(), d.modelName().c_str(), formatSize(d.sizeBytes).c_str(),
                    d.busType.c_str(), d.containsWindows() ? "  [contains Windows]" : "");
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
    std::printf("%-14s %-14s %-28s %s\n", "File system", "Detect", "Usage", "Modifying actions");
    for (FsType t : allFsTypes()) {
        const auto& c = capabilitiesFor(t);
        std::printf("%-14s %-14s %-28s %s\n", fsTypeName(t), supportName(c.detect), supportName(c.readUsage),
                    "not available in this version");
    }
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
