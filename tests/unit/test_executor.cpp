// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Plan execution on image files (WP-030, WP-032, WP-041, WP-042): every test
// executes a plan on an in-memory or temporary image and parses the result.
#include <cstdio>
#include <cstring>

#include "image_builder.hpp"
#include "test_framework.hpp"
#include "wparted/core/executor.hpp"

using namespace wparted;
using wptest::Image;

namespace {

constexpr std::uint64_t MiB = 1024 * 1024;
constexpr const char* kLinux = "0FC63DAF-8483-4772-8E79-3D69D8477DE4";
constexpr const char* kData = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7";

DiskLayout parse(const Image& im) {
    MemoryBlockReader r(im.data, im.ss);
    return parseDiskLayout(r, true);
}

OperationQueue queueFor(const Image& im) {
    const DiskLayout l = parse(im);
    return OperationQueue(l, identityOf(l, "", "image"));
}

Operation op(OpType t, std::uint32_t target = 0) {
    Operation o;
    o.type = t;
    o.target = target;
    return o;
}

const PartitionEntry* byNumber(const DiskLayout& l, int n) {
    for (const auto& p : l.partitions)
        if (p.number == n) return &p;
    return nullptr;
}

// Recognisable content: every sector starts with its index inside the partition.
void fillPattern(Image& im, std::uint64_t firstLba, std::uint64_t sectors, std::uint32_t skip) {
    for (std::uint64_t s = skip; s < sectors; ++s) im.put32((firstLba + s) * im.ss, static_cast<std::uint32_t>(0xA5000000u | s));
}
bool hasPattern(const Image& im, std::uint64_t firstLba, std::uint64_t sectors, std::uint32_t skip) {
    for (std::uint64_t s = skip; s < sectors; ++s) {
        const std::uint8_t* p = im.data.data() + (firstLba + s) * im.ss;
        if (le32(p) != (0xA5000000u | s)) return false;
    }
    return true;
}

ExecuteResult run(Image& im, const OperationQueue& q) {
    MemoryBlockDevice dev(im.data, im.ss);
    ExecuteResult r = executePlanOnImage(dev, q, "test image");
    if (!r.ok) std::printf("    %s\n", r.error.c_str());
    return r;
}

}  // namespace

TEST("executor: move forward with overlap keeps the data, then delete/create/name/flags") {
    Image im(64 * MiB, 512);
    im.gpt({{kData, 2048, 4095, "first", 0}, {kLinux, 8192, 40959, "data", 0}});
    wptest::writeExt4(im, 8192ull * 512, "moveme");
    fillPattern(im, 8192, 32768, 8);  // after the superblock area
    OperationQueue q = queueFor(im);
    std::string err;
    Operation mv = op(OpType::ResizeMove, 2);
    mv.firstLba = 16384;             // +4 MiB: destination overlaps the source
    mv.sectorCount = 32768;
    REQUIRE(q.add(mv, err));
    REQUIRE(q.add(op(OpType::DeletePartition, 1), err));
    Operation c = op(OpType::CreatePartition);
    c.firstLba = 2048;
    c.sectorCount = 8192;
    c.fs = FsType::Unknown;
    REQUIRE(q.add(c, err));
    Operation nm = op(OpType::SetName, q.operations().back().result);
    nm.name = "new";
    REQUIRE(q.add(nm, err));
    Operation fl = op(OpType::SetFlags, 2);
    fl.flagsOn = {"linux-home"};
    REQUIRE(q.add(fl, err));
    const ExecuteResult r = run(im, q);
    REQUIRE(r.ok);
    CHECK_EQ(r.completed, std::size_t{5});
    const DiskLayout back = parse(im);
    CHECK(!back.hasErrors());
    CHECK(back.gptPrimaryValid && back.gptBackupValid);
    const PartitionEntry* moved = byNumber(back, 2);
    REQUIRE(moved != nullptr);
    CHECK_EQ(moved->firstLba, 16384u);
    CHECK(moved->fs.type == FsType::Ext4);
    CHECK_EQ(moved->fs.label, std::string("moveme"));
    CHECK(hasPattern(im, 16384, 32768, 8));
    CHECK(std::find(partitionFlags(back, *moved).begin(), partitionFlags(back, *moved).end(), "linux-home") !=
          partitionFlags(back, *moved).end());
    const PartitionEntry* created = byNumber(back, 1);
    REQUIRE(created != nullptr);
    CHECK_EQ(created->name, std::string("new"));
    CHECK(created->fs.type == FsType::Unknown);  // old signatures cleared
    for (const auto& s : r.log.children) CHECK(s.status == LogStep::Status::Success);
}

TEST("executor: move backwards with overlap keeps the data") {
    Image im(64 * MiB, 512);
    im.gpt({{kLinux, 16384, 24575, "data", 0}});
    fillPattern(im, 16384, 8192, 0);
    OperationQueue q = queueFor(im);
    std::string err;
    Operation mv = op(OpType::ResizeMove, 1);
    mv.firstLba = 12288;  // -2 MiB
    mv.sectorCount = 8192;
    REQUIRE(q.add(mv, err));
    REQUIRE(run(im, q).ok);
    CHECK(hasPattern(im, 12288, 8192, 0));
    CHECK_EQ(byNumber(parse(im), 1)->firstLba, 12288u);
}

TEST("executor: copy to free space gives an identical file system and a new GUID") {
    Image im(64 * MiB, 512);
    im.gpt({{kLinux, 2048, 34815, "src", 0}});
    wptest::writeExt4(im, 2048ull * 512, "copied");
    fillPattern(im, 2048, 32768, 8);
    OperationQueue q = queueFor(im);
    std::string err;
    Operation cp = op(OpType::Copy, 1);
    cp.firstLba = 40960;
    cp.sectorCount = 32768;
    REQUIRE(q.add(cp, err));
    REQUIRE(run(im, q).ok);
    const DiskLayout back = parse(im);
    REQUIRE(back.partitions.size() == 2);
    const PartitionEntry* dst = byNumber(back, 2);
    REQUIRE(dst != nullptr);
    CHECK(dst->fs.type == FsType::Ext4);
    CHECK_EQ(dst->fs.label, std::string("copied"));
    CHECK(hasPattern(im, 40960, 32768, 8));
    CHECK(dst->uniqueGuid != byNumber(back, 1)->uniqueGuid);
}

TEST("executor: new msdos table with primary, extended and logical partitions") {
    Image im(64 * MiB, 512);
    OperationQueue q = queueFor(im);
    std::string err;
    Operation t = op(OpType::CreateTable);
    t.table = TableType::Mbr;
    REQUIRE(q.add(t, err));
    Operation p1 = op(OpType::CreatePartition);
    p1.firstLba = 2048;
    p1.sectorCount = 20480;
    REQUIRE(q.add(p1, err));
    Operation ex = op(OpType::CreatePartition);
    ex.kind = PartitionKind::Extended;
    ex.firstLba = 22528;
    ex.sectorCount = 40960;
    REQUIRE(q.add(ex, err));
    Operation lg = op(OpType::CreatePartition);
    lg.kind = PartitionKind::Logical;
    lg.firstLba = 24576;
    lg.sectorCount = 16384;
    REQUIRE(q.add(lg, err));
    REQUIRE(run(im, q).ok);
    const DiskLayout back = parse(im);
    CHECK(back.type == TableType::Mbr);
    CHECK(!back.hasErrors());
    REQUIRE(back.partitions.size() == 3);
    CHECK(byNumber(back, 5) != nullptr && byNumber(back, 5)->kind == PartitionKind::Logical);
    CHECK(im.b(510) == 0x55 && im.b(511) == 0xAA);
}

TEST("executor: refuses invalid plans and changed images without writing") {
    Image im(64 * MiB, 512);
    im.gpt({{kLinux, 2048, 10239, "a", 0}});
    wptest::writeExt4(im, 2048ull * 512, "keep");
    std::string err;
    {
        OperationQueue q = queueFor(im);
        Operation f = op(OpType::Format, 1);
        f.fs = FsType::Ntfs;  // creating file systems is not possible on images yet
        REQUIRE(q.add(f, err));
        const auto before = im.data;
        const ExecuteResult r = run(im, q);
        CHECK(!r.ok);
        CHECK(!r.validation.ok());
        CHECK(im.data == before);
        for (const auto& s : r.log.children) CHECK(s.status == LogStep::Status::Skipped);
    }
    {
        OperationQueue q = queueFor(im);
        REQUIRE(q.add(op(OpType::DeletePartition, 1), err));
        Image other(64 * MiB, 512);  // a different image: the plan must not be applied
        other.gpt({{kLinux, 2048, 20479, "b", 0}});
        const auto before = other.data;
        const ExecuteResult r = run(other, q);
        CHECK(!r.ok);
        CHECK(r.error.find("changed since planning") != std::string::npos);
        CHECK(other.data == before);
    }
    {
        OperationQueue q = queueFor(im);
        Operation grow = op(OpType::ResizeMove, 1);
        grow.firstLba = 2048;
        grow.sectorCount = 16384;  // growing ext4 needs the file system tools
        REQUIRE(q.add(grow, err));
        const auto before = im.data;
        CHECK(!run(im, q).ok);
        CHECK(im.data == before);
    }
}

TEST("executor: system partitions are warnings on images, extended moves are refused") {
    Image im(64 * MiB, 512);
    im.gpt({{"C12A7328-F81F-11D2-BA4B-00A0C93EC93B", 2048, 4095, "EFI", 0}});
    OperationQueue q = queueFor(im);
    std::string err;
    REQUIRE(q.add(op(OpType::DeletePartition, 1), err));
    const ExecuteResult r = run(im, q);
    CHECK(r.ok);
    bool warned = false;
    for (const auto& d : r.validation.diagnostics)
        if (d.code == "op.system_critical") warned = d.severity == Severity::Warning;
    CHECK(warned);

    Image mbr(64 * MiB, 512);
    OperationQueue m = queueFor(mbr);
    Operation t = op(OpType::CreateTable);
    t.table = TableType::Mbr;
    REQUIRE(m.add(t, err));
    Operation ex = op(OpType::CreatePartition);
    ex.kind = PartitionKind::Extended;
    ex.firstLba = 2048;
    ex.sectorCount = 20480;
    REQUIRE(m.add(ex, err));
    REQUIRE(run(mbr, m).ok);
    OperationQueue m2 = queueFor(mbr);
    Operation mv = op(OpType::ResizeMove, 1);
    mv.firstLba = 4096;
    mv.sectorCount = 20480;
    REQUIRE(m2.add(mv, err));
    const ExecuteResult r2 = run(mbr, m2);
    CHECK(!r2.ok);
    CHECK(r2.error.find("extended") != std::string::npos);
    CHECK(!r2.validation.ok());  // refused by validation, before anything is written
}

TEST("executor: device paths are refused, image files are modified in place only") {
    CHECK(isDevicePath("\\\\.\\PhysicalDrive0"));
    CHECK(isDevicePath("//./physicaldrive1"));
    CHECK(isDevicePath("\\\\?\\Volume{01234567-89ab-cdef-0123-456789abcdef}\\"));
    CHECK(isDevicePath("/dev/sda"));
    CHECK(isDevicePath("C:"));
    CHECK(isDevicePath("\\\\?\\GLOBALROOT\\Device\\Harddisk0\\Partition1"));
    CHECK(!isDevicePath("C:\\images\\disk.img"));
    CHECK(!isDevicePath("disk.img"));
    CHECK(!isDevicePath("/home/user/disk.img"));

    std::string err;
    {
        ImageFileDevice refused;
        CHECK(!refused.open("\\\\.\\PhysicalDrive0", 512, err));
        CHECK(!refused.open("does-not-exist-wparted.img", 512, err));  // never created
    }

    const char* path = "wparted_executor_test.img";
    Image im(16 * MiB, 512);
    im.gpt({{kLinux, 2048, 8191, "x", 0}});
    std::FILE* f = nullptr;
#ifdef _WIN32
    if (fopen_s(&f, path, "wb") != 0) f = nullptr;
#else
    f = std::fopen(path, "wb");
#endif
    REQUIRE(f != nullptr);
    std::fwrite(im.data.data(), 1, im.data.size(), f);
    std::fclose(f);
    {
        ImageFileDevice dev;
        REQUIRE(dev.open(path, 512, err));
        CHECK_EQ(dev.size(), im.data.size());
        std::uint8_t b = 0;
        CHECK(!dev.write(dev.size(), &b, 1));  // cannot grow the file
        OperationQueue q = queueFor(im);
        REQUIRE(q.add(op(OpType::DeletePartition, 1), err));
        CHECK(executePlanOnImage(dev, q, path).ok);
        CHECK(parseDiskLayout(dev, false).partitions.empty());
    }  // closed before the file is removed
    std::remove(path);
}
