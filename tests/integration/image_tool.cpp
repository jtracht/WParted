// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Test driver for plan execution on image files (WP-030/031/041/042).
//   image_tool plan <image> gpt-demo|mbr-demo <plan.json>   writes a plan for the image
//   image_tool apply <image> <plan.json>                      executes it with the core executor
// Used by tools/ci/image-apply-check.sh (verification with sgdisk, sfdisk,
// e2fsck, debugfs) and by the CLI end-to-end test. Only touches the given file.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "wparted/core/executor.hpp"
#include "wparted/core/fs_tools.hpp"

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
        const bool ok = std::strcmp(argv[3], "gpt-demo") == 0 ? gptDemo(q)
                        : std::strcmp(argv[3], "mbr-demo") == 0 ? mbrDemo(q)
                                                                : false;
        if (!ok) return 1;
        std::ofstream out(argv[4], std::ios::binary);
        out << q.toJson();
        return out ? 0 : 1;
    }
    if (argc == 4 && std::strcmp(argv[1], "apply") == 0) {
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
        const ExecuteResult res = executePlanOnImage(dev, q, argv[2]);
        for (std::size_t i = 0; i < res.log.children.size(); ++i)
            std::printf("%zu. [%s] %s\n", i + 1, logStatusName(res.log.children[i].status),
                        res.log.children[i].title.c_str());
        if (!res.ok) std::fprintf(stderr, "apply: %s\n", res.error.c_str());
        return res.ok ? 0 : 1;
    }
    std::fprintf(stderr, "usage: image_tool plan <image> gpt-demo|mbr-demo <plan.json>\n"
                         "       image_tool apply <image> <plan.json>\n");
    return 2;
}
