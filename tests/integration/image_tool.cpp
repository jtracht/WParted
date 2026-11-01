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
#include <cctype>
#include <iterator>
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

// "create:<fs>": a new 64 MiB partition with that file system behind the last one.
bool createFs(OperationQueue& q, FsType fs) {
    if (fs == FsType::Unknown) return false;
    const std::uint64_t spm = kMiB / q.original().sectorSize;
    std::uint64_t end = 0;
    for (const auto& p : q.current().partitions) end = std::max(end, p.firstLba + p.sectorCount);
    Operation c;
    c.type = OpType::CreatePartition;
    c.table = q.current().type;
    c.firstLba = (end + spm - 1) / spm * spm;
    c.sectorCount = 64 * spm;
    c.fs = fs;
    c.label = "WP" + std::string(fsTypeName(fs)).substr(0, 6);
    return add(q, c);
}


// "grow-last": the last partition (and its file system) grows by 16 MiB, then
// a read-only check; "delete-last": the last partition is deleted (its data stays).
bool lastPartition(OperationQueue& q, bool grow) {
    const PartitionEntry* last = nullptr;
    for (const auto& p : q.original().partitions)
        if (!last || p.firstLba > last->firstLba) last = &p;
    if (!last) return false;
    Operation o;
    o.target = last->planId;
    if (!grow) {
        o.type = OpType::DeletePartition;
        return add(q, o);
    }
    o.type = OpType::ResizeMove;
    o.firstLba = last->firstLba;
    o.sectorCount = last->sectorCount + 16 * (kMiB / q.original().sectorSize);
    Operation ck;
    ck.type = OpType::Check;
    ck.target = last->planId;
    return add(q, o) && (!fstools::canCheck(last->fs.type) || add(q, ck));
}

// exFAT fixture: no Linux tool writes exFAT without mounting, so the test places
// files at the end of the heap itself: /a.bin (contiguous, no FAT chain) and
// /dir/b.bin (FAT chain in falling cluster order). `exfat-cat` reads a file back.
struct ExFix {
    BlockDevice& d;
    std::vector<std::uint8_t> boot = std::vector<std::uint8_t>(512);
    std::uint64_t bps = 0, cb = 0, fat = 0, heap = 0;
    std::uint32_t count = 0, root = 0;
    static std::uint32_t rd32(const std::uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<std::uint32_t>(p[3]) << 24; }
    static std::uint64_t rd64(const std::uint8_t* p) { return rd32(p) | std::uint64_t{rd32(p + 4)} << 32; }
    static void wr32(std::uint8_t* p, std::uint64_t v) {
        for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
    static void wr64(std::uint8_t* p, std::uint64_t v) {
        for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
    bool load() {
        if (!d.read(0, boot.data(), 512)) return false;
        bps = 1ull << boot[108];
        cb = bps << boot[109];
        fat = rd32(&boot[80]) * bps;
        heap = rd32(&boot[88]) * bps;
        count = rd32(&boot[92]);
        root = rd32(&boot[96]);
        return true;
    }
    std::uint64_t at(std::uint32_t c) const { return heap + (c - 2) * cb; }
    std::uint32_t next(std::uint32_t c) const {
        std::uint8_t b[4];
        d.read(fat + 4ull * c, b, 4);
        return rd32(b);
    }
    void setFat(std::uint32_t c, std::uint32_t v) {
        std::uint8_t b[4];
        wr32(b, v);
        d.write(fat + 4ull * c, b, 4);
    }
    // Writes a file set for `name` at directory offset `where`; returns the set size.
    std::size_t entrySet(std::vector<std::uint8_t>& dir, std::size_t o, const std::string& name, bool isDir, std::uint32_t first,
                         std::uint64_t bytes, bool noChain) {
        const std::size_t count2 = 1 + (name.size() + 14) / 15;
        std::uint8_t* e = &dir[o];
        e[0] = 0x85;
        e[1] = static_cast<std::uint8_t>(count2);
        e[4] = isDir ? 0x10 : 0x20;
        e[32] = 0xC0;
        e[33] = static_cast<std::uint8_t>(noChain ? 3 : 1);
        e[35] = static_cast<std::uint8_t>(name.size());
        std::uint16_t hash = 0;
        for (char c : name) {
            const char u = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            hash = static_cast<std::uint16_t>(((hash << 15) | (hash >> 1)) + static_cast<std::uint8_t>(u));
            hash = static_cast<std::uint16_t>(((hash << 15) | (hash >> 1)));
        }
        e[36] = static_cast<std::uint8_t>(hash);
        e[37] = static_cast<std::uint8_t>(hash >> 8);
        wr64(&e[40], bytes);
        wr32(&e[52], first);
        wr64(&e[56], bytes);
        for (std::size_t i = 0; i < name.size(); ++i) {
            std::uint8_t* n = &e[64 + i / 15 * 32];
            n[0] = 0xC1;
            n[2 + i % 15 * 2] = static_cast<std::uint8_t>(name[i]);
        }
        std::uint16_t sum = 0;
        for (std::size_t i = 0; i < (count2 + 1) * 32; ++i) {
            if (i == 2 || i == 3) continue;
            sum = static_cast<std::uint16_t>(((sum << 15) | (sum >> 1)) + e[i]);
        }
        e[2] = static_cast<std::uint8_t>(sum);
        e[3] = static_cast<std::uint8_t>(sum >> 8);
        return (count2 + 1) * 32;
    }
};

int exfatInject(const std::string& file, const std::string& payload) {
    ImageFileDevice dev;
    std::string err;
    if (!dev.open(file, 512, err)) return 2;
    ExFix x{dev};
    if (!x.load()) return 2;
    std::ifstream in(payload, std::ios::binary);
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::uint64_t n = (data.size() + x.cb - 1) / x.cb;
    if (2 * n + 8 >= x.count) return 2;
    std::vector<std::uint8_t> rootDir(static_cast<std::size_t>(x.cb));
    if (!dev.read(x.at(x.root), rootDir.data(), rootDir.size())) return 2;
    std::size_t o = 0;
    std::uint32_t bitmapFirst = 0;
    std::uint64_t bitmapBytes = 0;
    for (; o + 32 <= rootDir.size() && rootDir[o]; o += 32)
        if (rootDir[o] == 0x81) {
            bitmapFirst = ExFix::rd32(&rootDir[o + 20]);
            bitmapBytes = ExFix::rd64(&rootDir[o + 24]);
        }
    std::vector<std::uint8_t> bits(static_cast<std::size_t>(bitmapBytes));
    if (!bitmapFirst || !dev.read(x.at(bitmapFirst), bits.data(), bits.size())) return 2;
    auto take = [&](std::uint32_t c) { bits[(c - 2) / 8] = static_cast<std::uint8_t>(bits[(c - 2) / 8] | (1u << ((c - 2) % 8))); };
    const std::uint32_t last = x.count + 1;  // highest cluster number
    // /a.bin: contiguous at the end of the heap.
    const std::uint32_t aFirst = static_cast<std::uint32_t>(last - n + 1);
    for (std::uint32_t c = 0; c < n; ++c) take(aFirst + c);
    dev.write(x.at(aFirst), data.data(), data.size());
    // /dir at one cluster in front of it, /dir/b.bin below in falling order.
    const std::uint32_t dirCluster = aFirst - 1;
    take(dirCluster);
    x.setFat(dirCluster, 0xFFFFFFFFu);
    std::vector<std::uint32_t> chain;
    for (std::uint64_t i = 0; i < n; ++i) chain.push_back(static_cast<std::uint32_t>(dirCluster - 1 - i));
    for (std::size_t i = 0; i < chain.size(); ++i) {
        take(chain[i]);
        x.setFat(chain[i], i + 1 < chain.size() ? chain[i + 1] : 0xFFFFFFFFu);
        const std::size_t len = static_cast<std::size_t>(std::min<std::uint64_t>(x.cb, data.size() - i * x.cb));
        dev.write(x.at(chain[i]), &data[i * x.cb], len);
    }
    std::vector<std::uint8_t> sub(static_cast<std::size_t>(x.cb), 0);
    x.entrySet(sub, 0, "b.bin", false, chain.front(), data.size(), false);
    dev.write(x.at(dirCluster), sub.data(), sub.size());
    o += x.entrySet(rootDir, o, "a.bin", false, aFirst, data.size(), true);
    x.entrySet(rootDir, o, "dir", true, dirCluster, x.cb, false);
    dev.write(x.at(x.root), rootDir.data(), rootDir.size());
    dev.write(x.at(bitmapFirst), bits.data(), bits.size());
    // Percent in use in the boot region is informational; fsck.exfat does not require it.
    return dev.flush() ? 0 : 1;
}

int exfatCat(const std::string& file, const std::string& path) {
    FileBlockReader r;
    if (!r.open(file, 512)) return 2;
    // Read through a device-shaped view: ExFix only reads here.
    std::vector<std::uint8_t> boot(512);
    if (!r.read(0, boot.data(), 512)) return 2;
    const std::uint64_t bps = 1ull << boot[108], cb = bps << boot[109];
    const std::uint64_t fat = ExFix::rd32(&boot[80]) * bps, heap = ExFix::rd32(&boot[88]) * bps;
    auto at = [&](std::uint32_t c) { return heap + (c - 2) * cb; };
    auto chainOf = [&](std::uint32_t first, std::uint64_t bytes, bool noChain) {
        std::vector<std::uint32_t> out;
        if (noChain) {
            for (std::uint64_t i = 0; i * cb < bytes; ++i) out.push_back(static_cast<std::uint32_t>(first + i));
            return out;
        }
        for (std::uint32_t c = first; c >= 2 && c < 0xFFFFFFF7u && out.size() < 100000000u;) {
            out.push_back(c);
            std::uint8_t b[4];
            if (!r.read(fat + 4ull * c, b, 4)) break;
            c = ExFix::rd32(b);
        }
        return out;
    };
    std::vector<std::uint32_t> dirChain = chainOf(ExFix::rd32(&boot[96]), 0, false);
    std::string rest = path;
    for (;;) {
        const std::size_t slash = rest.find('/');
        const std::string name = rest.substr(0, slash);
        std::vector<std::uint8_t> dir(dirChain.size() * static_cast<std::size_t>(cb));
        for (std::size_t i = 0; i < dirChain.size(); ++i)
            if (!r.read(at(dirChain[i]), &dir[i * static_cast<std::size_t>(cb)], static_cast<std::size_t>(cb))) return 2;
        bool found = false;
        for (std::size_t o = 0; o + 32 <= dir.size() && dir[o]; o += 32) {
            if (dir[o] != 0x85) continue;
            const std::size_t count = dir[o + 1];
            std::string got;
            for (std::size_t i = 0; i < dir[o + 32 + 3]; ++i) got += static_cast<char>(dir[o + 64 + i / 15 * 32 + 2 + i % 15 * 2]);
            if (got == name) {
                const bool noChain = (dir[o + 33] & 2) != 0;
                const std::uint32_t first = ExFix::rd32(&dir[o + 52]);
                const std::uint64_t bytes = ExFix::rd64(&dir[o + 56]);
                const std::vector<std::uint32_t> chain = chainOf(first, bytes, noChain);
                if (slash != std::string::npos) {
                    dirChain = chain;
                    rest = rest.substr(slash + 1);
                    found = true;
                    break;
                }
                std::vector<std::uint8_t> buf(static_cast<std::size_t>(cb));
                std::uint64_t left = bytes;
                for (std::uint32_t c : chain) {
                    if (!r.read(at(c), buf.data(), buf.size())) return 2;
                    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(left, cb));
                    std::fwrite(buf.data(), 1, n, stdout);
                    left -= n;
                }
                return left ? 1 : 0;
            }
            o += count * 32;
        }
        if (!found) return 1;
    }
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
    if (argc >= 4 && std::strcmp(argv[1], "exfat-inject") == 0) return exfatInject(argv[2], argv[3]);
    if (argc >= 4 && std::strcmp(argv[1], "exfat-cat") == 0) return exfatCat(argv[2], argv[3]);
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
                        : scenario.rfind("create:", 0) == 0 ? createFs(q, fsByName(scenario.c_str() + 7))
                        : scenario == "grow-last"  ? lastPartition(q, true)
                        : scenario == "delete-last" ? lastPartition(q, false)
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
