// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Test driver for plan execution on image files (WP-030/031/041/042).
//   image_tool plan <image> gpt-demo|mbr-demo <plan.json>   writes a plan for the image
//   image_tool apply <image> <plan.json> [--journal] [--chunk BYTES] [--crash-after-chunks N]
//                                                             executes it with the core executor;
//                                                             --crash-after-chunks ends the process
//                                                             mid-copy without clean-up (fault injection)
//   image_tool resume <image>                                 continues an interrupted execution
// Used by tools/ci/image-apply-check.sh (verification with sgdisk, sfdisk,
// e2fsck, debugfs) and by the CLI end-to-end test. Only touches the given file.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "wparted/core/executor.hpp"
#include "wparted/core/fs_tools.hpp"
#include "wparted/core/rescue.hpp"

using namespace wparted;

namespace {

constexpr std::uint64_t kMiB = 1024 * 1024;

bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream s;
    s << f.rdbuf();
    out = s.str();
    return true;
}

bool add(OperationQueue& q, Operation op) {
    std::string err;
    if (q.add(op, err)) return true;
    std::fprintf(stderr, "plan: %s\n", err.c_str());
    return false;
}

const PartitionEntry* byNumber(const DiskLayout& l, int n) {
    for (const auto& p : l.partitions)
        if (p.number == n) return &p;
    return nullptr;
}

// GPT image with partitions #1 and #2 and free space behind #2:
// delete #1, create an unformatted partition in its place, move #2 by
// +16 MiB, copy it behind itself, set a flag.
bool gptDemo(OperationQueue& q) {
    const DiskLayout& l = q.original();
    const PartitionEntry* p1 = byNumber(l, 1);
    const PartitionEntry* p2 = byNumber(l, 2);
    if (l.type != TableType::Gpt || !p1 || !p2) {
        std::fprintf(stderr, "plan: gpt-demo needs a GPT image with partitions 1 and 2\n");
        return false;
    }
    const std::uint64_t spm = kMiB / l.sectorSize;
    Operation del;
    del.type = OpType::DeletePartition;
    del.target = p1->planId;
    Operation mk;
    mk.type = OpType::CreatePartition;
    mk.firstLba = p1->firstLba;
    mk.sectorCount = p1->sectorCount;
    mk.name = "wparted-new";
    Operation mv;
    mv.type = OpType::ResizeMove;
    mv.target = p2->planId;
    mv.firstLba = p2->firstLba + 16 * spm;
    mv.sectorCount = p2->sectorCount;
    Operation cp;
    cp.type = OpType::Copy;
    cp.target = p2->planId;
    cp.firstLba = (mv.firstLba + mv.sectorCount + spm - 1) / spm * spm;
    cp.sectorCount = p2->sectorCount;
    Operation fl;
    fl.type = OpType::SetFlags;
    fl.target = p2->planId;
    fl.flagsOn = {"linux-home"};
    return add(q, del) && add(q, mk) && add(q, mv) && add(q, cp) && add(q, fl);
}

// GPT image: new FAT32 partition "WPFAT" (40 MiB) behind the last partition.
bool fatCreate(OperationQueue& q) {
    const DiskLayout& l = q.original();
    const std::uint64_t spm = kMiB / l.sectorSize;
    std::uint64_t end = 0;
    for (const auto& p : l.partitions) end = std::max(end, p.firstLba + p.sectorCount);
    Operation c;
    c.type = OpType::CreatePartition;
    c.firstLba = (end + spm - 1) / spm * spm;
    c.sectorCount = 40 * spm;
    c.fs = FsType::Fat32;
    c.label = "WPFAT";
    return add(q, c);
}

// Grows the last FAT32 partition by 16 MiB and gives it a new label and serial.
bool fatGrow(OperationQueue& q) {
    const PartitionEntry* last = nullptr;
    for (const auto& p : q.original().partitions)
        if (p.fs.type == FsType::Fat32 && (!last || p.firstLba > last->firstLba)) last = &p;
    if (!last) {
        std::fprintf(stderr, "plan: fat-grow needs a FAT32 partition\n");
        return false;
    }
    Operation g;
    g.type = OpType::ResizeMove;
    g.target = last->planId;
    g.firstLba = last->firstLba;
    g.sectorCount = last->sectorCount + 16 * (kMiB / q.original().sectorSize);
    Operation lb;
    lb.type = OpType::SetLabel;
    lb.target = last->planId;
    lb.label = "WPGROWN";
    Operation u;
    u.type = OpType::NewUuid;
    u.target = last->planId;
    Operation ck;
    ck.type = OpType::Check;
    ck.target = last->planId;
    return add(q, g) && add(q, lb) && add(q, u) && add(q, ck);
}

// Blank image: msdos table, primary, extended with two logicals.
bool mbrDemo(OperationQueue& q) {
    const std::uint64_t spm = kMiB / q.original().sectorSize;
    Operation t;
    t.type = OpType::CreateTable;
    t.table = TableType::Mbr;
    if (!add(q, t)) return false;
    auto part = [&](PartitionKind k, std::uint64_t startMiB, std::uint64_t sizeMiB) {
        Operation c;
        c.type = OpType::CreatePartition;
        c.kind = k;
        c.firstLba = startMiB * spm;
        c.sectorCount = sizeMiB * spm;
        return add(q, c);
    };
    return part(PartitionKind::Primary, 1, 32) && part(PartitionKind::Extended, 34, 64) &&
           part(PartitionKind::Logical, 35, 20) && part(PartitionKind::Logical, 57, 20);
}

// msdos image from mbr-demo: the extended partition starts 1 MiB earlier and
// grows to 123 MiB; the logical partitions stay where they are.
bool extMove(OperationQueue& q) {
    const std::uint64_t spm = kMiB / q.original().sectorSize;
    for (const auto& p : q.original().partitions)
        if (p.kind == PartitionKind::Extended) {
            Operation m;
            m.type = OpType::ResizeMove;
            m.target = p.planId;
            m.firstLba = p.firstLba - spm;
            m.sectorCount = 123 * spm - m.firstLba;
            return add(q, m);
        }
    std::fprintf(stderr, "plan: ext-move needs an extended partition\n");
    return false;
}

int printLog(const ExecuteResult& res) {
    for (std::size_t i = 0; i < res.log.children.size(); ++i)
        std::printf("%zu. [%s] %s\n", i + 1, logStatusName(res.log.children[i].status),
                    res.log.children[i].title.c_str());
    if (!res.ok) std::fprintf(stderr, "apply: %s\n", res.error.c_str());
    return res.ok ? 0 : 1;
}

}  // namespace

FsType fsByName(const char* n) {
    for (FsType t : allFsTypes())
        if (std::strcmp(fsTypeName(t), n) == 0) return t;
    return FsType::Unknown;
}

// image_tool fs <file> create <fs> [label] | label <fs> <label> | uuid <fs> | resize <fs> <bytes> | check <fs>
// The whole file is the partition.
int fsCommand(int argc, char** argv) {
    if (argc < 5) return 2;
    const std::string file = argv[2], cmd = argv[3];
    const FsType t = fsByName(argv[4]);
    ImageFileDevice dev;
    std::string err;
    if (!dev.open(file, 512, err)) {
        std::fprintf(stderr, "fs: %s\n", err.c_str());
        return 2;
    }
    RegionDevice part(dev, 0, dev.size());
    bool ok = false;
    if (cmd == "create") ok = fstools::create(part, t, argc > 5 ? argv[5] : "", 0, err);
    else if (cmd == "label" && argc > 5) ok = fstools::setLabel(part, t, argv[5], err);
    else if (cmd == "uuid") {
        std::string uuid;
        ok = fstools::setNewUuid(part, t, uuid, err);
        if (ok) std::printf("%s\n", uuid.c_str());
    } else if (cmd == "resize" && argc > 5) {
        ok = fstools::resize(part, t, std::strtoull(argv[5], nullptr, 10), err);
    } else if (cmd == "check") {
        fstools::CheckReport rep;
        ok = fstools::check(part, t, rep, err);
        for (const auto& e : rep.errors) std::printf("ERROR %s\n", e.c_str());
        for (const auto& w : rep.warnings) std::printf("WARNING %s\n", w.c_str());
        std::printf("used %llu of %llu bytes\n", static_cast<unsigned long long>(rep.usedBytes),
                    static_cast<unsigned long long>(rep.totalBytes));
        ok = ok && rep.ok;
    } else {
        err = "unknown fs command";
    }
    if (!ok) std::fprintf(stderr, "fs %s: %s\n", cmd.c_str(), err.c_str());
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "fs") == 0) return fsCommand(argc, argv);
    if (argc == 5 && std::strcmp(argv[1], "plan") == 0) {
        FileBlockReader r;
        if (!r.open(argv[2], 512)) return 2;
        const DiskLayout l = parseDiskLayout(r, true);
        OperationQueue q(l, identityOf(l, "", "image"));
        const std::string scenario = argv[3];
        const bool ok = scenario == "gpt-demo"   ? gptDemo(q)
                        : scenario == "mbr-demo" ? mbrDemo(q)
                        : scenario == "fat-create" ? fatCreate(q)
                        : scenario == "fat-grow"   ? fatGrow(q)
                        : scenario == "ext-move"   ? extMove(q)
                                                   : false;
        if (!ok) return 1;
        std::ofstream out(argv[4], std::ios::binary);
        out << q.toJson();
        return out ? 0 : 1;
    }
    if (argc >= 4 && std::strcmp(argv[1], "apply") == 0) {
        ExecuteOptions opt;
        for (int i = 4; i < argc; ++i) {
            if (std::strcmp(argv[i], "--journal") == 0) opt.journalPath = journalPathFor(argv[2]);
            if (std::strcmp(argv[i], "--chunk") == 0 && i + 1 < argc) opt.chunkBytes = std::strtoull(argv[++i], nullptr, 10);
            if (std::strcmp(argv[i], "--crash-after-chunks") == 0 && i + 1 < argc)
                opt.abortAfterChunks = std::strtoull(argv[++i], nullptr, 10);
        }
        std::string json, err;
        if (!readFile(argv[3], json)) return 2;
        DiskLayout original;
        {
            FileBlockReader r;  // closed before the exclusive write handle is opened
            if (!r.open(argv[2], 512)) return 2;
            original = parseDiskLayout(r, true);
        }
        OperationQueue q;
        if (!OperationQueue::fromJson(json, original, q, err)) {
            std::fprintf(stderr, "apply: %s\n", err.c_str());
            return 1;
        }
        ImageFileDevice dev;
        if (!dev.open(argv[2], 512, err)) {
            std::fprintf(stderr, "apply: %s\n", err.c_str());
            return 2;
        }
        const ExecuteResult res = executePlanOnImage(dev, q, argv[2], opt);
        if (opt.abortAfterChunks && !res.ok && res.error.find("interrupted") != std::string::npos) {
            std::fflush(stdout);
            std::_Exit(75);  // like a crash: buffered image data and handles are dropped
        }
        return printLog(res);
    }
    if (argc == 4 && std::strcmp(argv[1], "rescue") == 0) {  // plan that recreates lost partitions
        FileBlockReader r;
        if (!r.open(argv[2], 512)) return 2;
        const DiskLayout l = parseDiskLayout(r, true);
        const auto found = rescueScan(r, l);
        OperationQueue q(l, identityOf(l, "", "image"));
        std::string err;
        const std::size_t n = planRecreate(q, found, err);
        for (const auto& c : found)
            std::printf("found %s at sector %llu\n", fsTypeName(c.fs.type), static_cast<unsigned long long>(c.firstLba));
        if (!err.empty()) std::fprintf(stderr, "rescue: %s\n", err.c_str());
        std::ofstream out(argv[3], std::ios::binary);
        out << q.toJson();
        std::printf("%zu partition(s) to recreate\n", n);
        return out && n ? 0 : 1;
    }
    if (argc == 3 && std::strcmp(argv[1], "resume") == 0) {
        ImageFileDevice dev;
        std::string err;
        if (!dev.open(argv[2], 512, err)) {
            std::fprintf(stderr, "resume: %s\n", err.c_str());
            return 2;
        }
        ExecuteOptions opt;
        opt.journalPath = journalPathFor(argv[2]);
        return printLog(resumePlanOnImage(dev, argv[2], opt));
    }
    std::fprintf(stderr, "usage: image_tool plan <image> gpt-demo|mbr-demo|fat-create|fat-grow|ext-move <plan.json>\n"
                         "       image_tool apply <image> <plan.json> [--journal] [--chunk BYTES] [--crash-after-chunks N]\n"
                         "       image_tool resume <image>\n");
    return 2;
}
