// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Plan execution on image files (WP-030, WP-032, WP-041, WP-042): every test
// executes a plan on an in-memory or temporary image and parses the result.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>

#include "image_builder.hpp"
#include "test_framework.hpp"
#include "wparted/core/executor.hpp"
#include "wparted/core/fs_tools.hpp"

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

TEST("executor: file-system steps (create, label, UUID, check, grow, shrink+move, format)") {
    Image im(256 * MiB, 512);
    im.gpt({{kData, 2048, 4095, "small", 0}});
    std::string err;
    {
        OperationQueue q = queueFor(im);
        Operation c = op(OpType::CreatePartition);
        c.firstLba = 8192;
        c.sectorCount = 40 * 2048;  // 40 MiB
        c.fs = FsType::Fat32;
        c.label = "Daten";
        REQUIRE(q.add(c, err));
        REQUIRE(run(im, q).ok);
    }
    // Copy, not a pointer: parse() returns a temporary layout.
    auto part = [&](int n) {
        const DiskLayout l = parse(im);
        const PartitionEntry* p = byNumber(l, n);
        return p ? std::optional<PartitionEntry>(*p) : std::nullopt;
    };
    REQUIRE(part(2).has_value());
    CHECK(part(2)->fs.type == FsType::Fat32);
    CHECK_EQ(part(2)->fs.label, std::string("DATEN"));
    const std::string oldUuid = part(2)->fs.uuid;
    {
        OperationQueue q = queueFor(im);
        Operation l = op(OpType::SetLabel, 2);
        l.label = "Archiv";
        REQUIRE(q.add(l, err));
        REQUIRE(q.add(op(OpType::NewUuid, 2), err));
        REQUIRE(q.add(op(OpType::Check, 2), err));
        Operation g = op(OpType::ResizeMove, 2);
        g.firstLba = 8192;
        g.sectorCount = 100 * 2048;  // grow to 100 MiB
        REQUIRE(q.add(g, err));
        const ExecuteResult r = run(im, q);
        REQUIRE(r.ok);
        CHECK_EQ(r.completed, std::size_t{4});
    }
    CHECK_EQ(part(2)->fs.label, std::string("ARCHIV"));
    CHECK(part(2)->fs.uuid != oldUuid);
    CHECK_EQ(part(2)->sectorCount, 100u * 2048);
    {
        OperationQueue q = queueFor(im);
        Operation s = op(OpType::ResizeMove, 2);
        s.firstLba = 16384;          // move right and shrink to 60 MiB
        s.sectorCount = 60 * 2048;
        REQUIRE(q.add(s, err));
        REQUIRE(run(im, q).ok);
    }
    REQUIRE(part(2).has_value());
    CHECK_EQ(part(2)->firstLba, 16384u);
    CHECK(part(2)->fs.type == FsType::Fat32);
    CHECK_EQ(part(2)->fs.label, std::string("ARCHIV"));
    {
        MemoryBlockDevice dev(im.data, 512);
        RegionDevice p2(dev, 16384ull * 512, 60ull * MiB);
        fstools::CheckReport rep;
        REQUIRE(fstools::check(p2, FsType::Fat32, rep, err));
        CHECK(rep.ok);
        CHECK(rep.totalBytes > 55 * MiB && rep.totalBytes < 60 * MiB);  // 60 MiB minus FAT and reserved area
    }
    {
        OperationQueue q = queueFor(im);
        Operation f = op(OpType::Format, 2);
        f.fs = FsType::Fat16;
        f.label = "NEU";
        REQUIRE(q.add(f, err));
        REQUIRE(run(im, q).ok);
    }
    CHECK(part(2)->fs.type == FsType::Fat16);
    CHECK_EQ(part(2)->fs.label, std::string("NEU"));
    {  // invalid labels and shrinking below the data are refused before writing
        OperationQueue q = queueFor(im);
        Operation l = op(OpType::SetLabel, 2);
        l.label = "way too long label";
        REQUIRE(q.add(l, err));
        const auto before = im.data;
        CHECK(!run(im, q).ok);
        CHECK(im.data == before);
    }
}

TEST("executor: a plan for a blank image survives the plan file round trip") {
    Image im(64 * MiB, 512);  // no partition table at all
    OperationQueue q = queueFor(im);
    std::string err;
    Operation t = op(OpType::CreateTable);
    t.table = TableType::Gpt;
    REQUIRE(q.add(t, err));
    Operation c = op(OpType::CreatePartition);
    c.firstLba = 2048;
    c.sectorCount = 20480;
    REQUIRE(q.add(c, err));
    OperationQueue loaded;
    REQUIRE(OperationQueue::fromJson(q.toJson(), parse(im), loaded, err));
    CHECK(loaded.identity().table == TableType::None);
    REQUIRE(run(im, loaded).ok);
    CHECK(parse(im).type == TableType::Gpt);
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

TEST("executor: system partitions are warnings on images") {
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
}

TEST("executor: moving an extended partition rewrites the EBR chain, logical data stays") {
    Image mbr(64 * MiB, 512);
    OperationQueue m = queueFor(mbr);
    std::string err;
    Operation t = op(OpType::CreateTable);
    t.table = TableType::Mbr;
    REQUIRE(m.add(t, err));
    Operation ex = op(OpType::CreatePartition);
    ex.kind = PartitionKind::Extended;
    ex.firstLba = 4096;
    ex.sectorCount = 61440;
    REQUIRE(m.add(ex, err));
    for (std::uint64_t start : {8192u, 30720u}) {
        Operation lg = op(OpType::CreatePartition);
        lg.kind = PartitionKind::Logical;
        lg.firstLba = start;
        lg.sectorCount = 16384;
        REQUIRE(m.add(lg, err));
    }
    REQUIRE(run(mbr, m).ok);
    fillPattern(mbr, 8192, 16384, 0);
    fillPattern(mbr, 30720, 16384, 0);
    auto extOf = [](const OperationQueue& q) {
        for (const auto& p : q.original().partitions)
            if (p.kind == PartitionKind::Extended) return p.planId;
        return 0u;
    };

    // Start 1 MiB later (the old first EBR is cleared), then 2 MiB earlier.
    for (std::uint64_t start : {6144u, 2048u}) {
        OperationQueue q = queueFor(mbr);
        REQUIRE(extOf(q) != 0);
        Operation mv = op(OpType::ResizeMove, extOf(q));
        mv.firstLba = start;
        mv.sectorCount = 65536 - start;
        REQUIRE(q.add(mv, err));
        const ExecuteResult r = run(mbr, q);
        REQUIRE(r.ok);
        const DiskLayout back = parse(mbr);
        CHECK(!back.hasErrors());
        REQUIRE(back.partitions.size() == 3);
        const PartitionEntry* e = nullptr;
        for (const auto& p : back.partitions)
            if (p.kind == PartitionKind::Extended) e = &p;
        REQUIRE(e != nullptr);
        CHECK_EQ(e->firstLba, start);
        CHECK(byNumber(back, 5) && byNumber(back, 5)->firstLba == 8192u);
        CHECK(byNumber(back, 6) && byNumber(back, 6)->firstLba == 30720u);
        CHECK(hasPattern(mbr, 8192, 16384, 0));
        CHECK(hasPattern(mbr, 30720, 16384, 0));
    }
    CHECK(mbr.b(4096 * 512 + 510) == 0 && mbr.b(6144 * 512 + 510) == 0);  // stale EBRs cleared

    // A logical partition must stay inside, with room for its EBR.
    OperationQueue bad = queueFor(mbr);
    Operation mv = op(OpType::ResizeMove, extOf(bad));
    mv.firstLba = 8192;
    mv.sectorCount = 57344;
    REQUIRE(bad.add(mv, err));
    CHECK(!run(mbr, bad).ok);
    CHECK(hasPattern(mbr, 8192, 16384, 0));
}

namespace {

// A temporary image file on disk (the journal lives next to it).
struct TempImage {
    std::string path;
    explicit TempImage(const Image& im, const char* name) : path(name) {
        std::string err;
        discardJournal(journalPathFor(path), err);
        std::FILE* f = nullptr;
#ifdef _WIN32
        if (fopen_s(&f, path.c_str(), "wb") != 0) f = nullptr;
#else
        f = std::fopen(path.c_str(), "wb");
#endif
        if (f) {
            std::fwrite(im.data.data(), 1, im.data.size(), f);
            std::fclose(f);
        }
    }
    ~TempImage() {
        std::string err;
        discardJournal(journalPathFor(path), err);
        std::remove(path.c_str());
    }
    Image load(std::uint32_t ss = 512) const {
        Image im(0, ss);
        std::FILE* f = nullptr;
#ifdef _WIN32
        if (fopen_s(&f, path.c_str(), "rb") != 0) f = nullptr;
#else
        f = std::fopen(path.c_str(), "rb");
#endif
        if (!f) return im;
        std::uint8_t buf[65536];
        for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) im.data.insert(im.data.end(), buf, buf + n);
        std::fclose(f);
        return im;
    }
};

}  // namespace

TEST("executor: an interrupted overlapping move is journalled and resumed without data loss") {
    for (bool backwards : {false, true}) {
        Image im(64 * MiB, 512);
        im.gpt({{kData, 2048, 4095, "first", 0}, {kLinux, 16384, 49151, "data", 0}});
        fillPattern(im, 16384, 32768, 0);
        TempImage file(im, backwards ? "wparted_journal_back.img" : "wparted_journal_fwd.img");
        OperationQueue q = queueFor(im);
        std::string err;
        REQUIRE(q.add(op(OpType::DeletePartition, 1), err));
        Operation mv = op(OpType::ResizeMove, 2);
        mv.firstLba = backwards ? 12288 : 20480;  // 2 MiB shift of a 16 MiB partition
        mv.sectorCount = 32768;
        REQUIRE(q.add(mv, err));
        Operation fl = op(OpType::SetFlags, 2);
        fl.flagsOn = {"linux-home"};
        REQUIRE(q.add(fl, err));
        const std::string journal = journalPathFor(file.path);
        {
            ImageFileDevice dev;
            REQUIRE(dev.open(file.path, 512, err));
            ExecuteOptions o;
            o.journalPath = journal;
            o.chunkBytes = 4 * MiB;       // limited to the 2 MiB shift distance
            o.abortAfterChunks = 3;       // "crash" after the third chunk was written
            const ExecuteResult r = executePlanOnImage(dev, q, file.path, o);
            CHECK(!r.ok);
            CHECK_EQ(r.completed, std::size_t{1});
            CHECK(r.error.find("interrupted") != std::string::npos);
        }
        JournalState s;
        REQUIRE(readJournal(journal, s, err));
        CHECK_EQ(s.completed, std::size_t{1});
        CHECK_EQ(s.phase, std::string("copy"));
        CHECK_EQ(s.operation, std::size_t{1});
        CHECK_EQ(s.copyDone, 4 * MiB);  // two chunks on disk; the third was not recorded
        CHECK_EQ(s.operationCount, std::size_t{3});
        CHECK(s.resumable());
        {
            ImageFileDevice dev;
            REQUIRE(dev.open(file.path, 512, err));
            ExecuteOptions o;
            o.journalPath = journal;
            CHECK(!executePlanOnImage(dev, q, file.path, o).ok);  // the journal blocks a new run
            const ExecuteResult r = resumePlanOnImage(dev, file.path, o);
            if (!r.ok) std::printf("    %s\n", r.error.c_str());
            REQUIRE(r.ok);
            CHECK_EQ(r.completed, std::size_t{3});
        }
        CHECK(!journalExists(journal));
        const Image after = file.load();
        CHECK(hasPattern(after, mv.firstLba, 32768, 0));
        const DiskLayout back = parse(after);
        REQUIRE(back.partitions.size() == 1);
        CHECK_EQ(back.partitions[0].firstLba, mv.firstLba);
    }
}

TEST("executor: cancelling between operations leaves a resumable journal; damage is detected") {
    Image im(64 * MiB, 512);
    im.gpt({{kData, 2048, 4095, "a", 0}, {kData, 8192, 10239, "b", 0}});
    TempImage file(im, "wparted_journal_cancel.img");
    OperationQueue q = queueFor(im);
    std::string err;
    REQUIRE(q.add(op(OpType::DeletePartition, 1), err));
    REQUIRE(q.add(op(OpType::DeletePartition, 2), err));
    const std::string journal = journalPathFor(file.path);
    std::atomic<bool> cancel{false};
    {
        ImageFileDevice dev;
        REQUIRE(dev.open(file.path, 512, err));
        ExecuteOptions o;
        o.journalPath = journal;
        o.cancel = &cancel;
        o.progress = [&](std::size_t, std::uint64_t, std::uint64_t) {};
        // Cancel as soon as the first operation is done: watch the journal.
        struct Watch final : public BlockDevice {
            BlockDevice& d;
            std::atomic<bool>& c;
            int tableWrites = 0;
            Watch(BlockDevice& dd, std::atomic<bool>& cc) : d(dd), c(cc) {}
            std::uint64_t size() const override { return d.size(); }
            std::uint32_t sectorSize() const override { return d.sectorSize(); }
            bool read(std::uint64_t off, void* b, std::size_t n) override { return d.read(off, b, n); }
            bool write(std::uint64_t off, const void* b, std::size_t n) override {
                if (off == 512) c = true;  // GPT header: the first table write
                return d.write(off, b, n);
            }
            bool flush() override { return d.flush(); }
        } watch(dev, cancel);
        const ExecuteResult r = executePlanOnImage(watch, q, file.path, o);
        CHECK(!r.ok);
        CHECK_EQ(r.completed, std::size_t{1});
    }
    JournalState s;
    REQUIRE(readJournal(journal, s, err));
    CHECK_EQ(s.phase, std::string("idle"));
    CHECK_EQ(s.completed, std::size_t{1});

    // A changed header is refused as damaged; restored, the journal resumes.
    auto poke = [&](char c) {
        std::FILE* f = nullptr;
#ifdef _WIN32
        if (fopen_s(&f, journal.c_str(), "r+b") != 0) f = nullptr;
#else
        f = std::fopen(journal.c_str(), "r+b");
#endif
        if (!f) return false;
        std::fseek(f, 28, SEEK_SET);  // first digit of "completed=0000000001"
        std::fputc(c, f);
        std::fclose(f);
        return true;
    };
    REQUIRE(poke('5'));
    JournalState damaged;
    CHECK(!readJournal(journal, damaged, err));
    REQUIRE(poke('0'));
    {
        ImageFileDevice dev;
        REQUIRE(dev.open(file.path, 512, err));
        ExecuteOptions o;
        o.journalPath = journal;
        const ExecuteResult r = resumePlanOnImage(dev, file.path, o);
        REQUIRE(r.ok);
        CHECK_EQ(r.completed, std::size_t{2});
    }
    CHECK(!journalExists(journal));
    CHECK(parse(file.load()).partitions.empty());
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
