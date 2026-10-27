// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Tests for the plan model (operations, queue, plan JSON), the validation
// engine, the JSON parser, foreign table detection and - as the image test
// harness of WP-031 - the round trip simulate -> serialise -> apply to an
// in-memory image -> parse back.
#include <algorithm>
#include <cstring>
#include <random>

#include "image_builder.hpp"
#include "test_framework.hpp"
#include "wparted/core/json_value.hpp"
#include "wparted/core/layout_writer.hpp"
#include "wparted/core/operations.hpp"
#include "wparted/core/validation.hpp"

using namespace wparted;
using wptest::Image;

namespace {

constexpr std::uint64_t MiB = 1024 * 1024;
constexpr const char* kBasic = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7";
constexpr const char* kLinux = "0FC63DAF-8483-4772-8E79-3D69D8477DE4";
constexpr const char* kEsp = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B";

DiskLayout parseImage(const Image& im) {
    MemoryBlockReader r(im.data, im.ss);
    return parseDiskLayout(r, true);
}

OperationQueue queueFor(const Image& im) {
    DiskLayout l = parseImage(im);
    return OperationQueue(l, identityOf(l, "TEST-SERIAL", "Test disk"));
}

bool hasCode(const ValidationResult& r, const std::string& code, Severity s) {
    for (const auto& d : r.diagnostics)
        if (d.code == code && d.severity == s) return true;
    return false;
}

Operation create(std::uint64_t first, std::uint64_t count, FsType fs = FsType::Unknown,
                 PartitionKind kind = PartitionKind::Primary) {
    Operation op;
    op.type = OpType::CreatePartition;
    op.firstLba = first;
    op.sectorCount = count;
    op.fs = fs;
    op.kind = kind;
    return op;
}

Operation onTarget(OpType t, std::uint32_t target) {
    Operation op;
    op.type = t;
    op.target = target;
    return op;
}

// Image test harness: writes the planned layout into the image and parses it.
DiskLayout applyToImage(Image& im, DiskLayout planned) {
    assignNewIdentifiers(planned);
    SerializeOptions opt;
    opt.originalSector0.assign(im.data.begin(), im.data.begin() + im.ss);
    std::vector<SectorWrite> writes;
    std::string err;
    if (!serializeLayout(planned, opt, writes, err)) {
        std::printf("    serialize failed: %s\n", err.c_str());
        return DiskLayout{};
    }
    for (const auto& w : writes) std::memcpy(im.at(w.lba * im.ss), w.data.data(), w.data.size());
    return parseImage(im);
}

// Compares the on-disk result with the simulated plan (geometry and types).
bool sameGeometry(const DiskLayout& a, const DiskLayout& b) {
    if (a.type != b.type || a.partitions.size() != b.partitions.size()) {
        std::printf("    table %s/%s, %zu/%zu partitions\n", tableTypeName(a.type), tableTypeName(b.type),
                    a.partitions.size(), b.partitions.size());
        return false;
    }
    auto key = [](const PartitionEntry& p) { return std::make_pair(p.firstLba, p.number); };
    std::vector<PartitionEntry> x = a.partitions, y = b.partitions;
    std::sort(x.begin(), x.end(), [&](auto& p, auto& q) { return key(p) < key(q); });
    std::sort(y.begin(), y.end(), [&](auto& p, auto& q) { return key(p) < key(q); });
    for (std::size_t i = 0; i < x.size(); ++i) {
        const auto &p = x[i], &q = y[i];
        if (p.number != q.number || p.kind != q.kind || p.firstLba != q.firstLba || p.sectorCount != q.sectorCount ||
            p.mbrType != q.mbrType || p.mbrBootable != q.mbrBootable || p.typeGuid != q.typeGuid || p.name != q.name ||
            p.gptAttributes != q.gptAttributes) {
            std::printf("    partition %d differs (first %llu/%llu, count %llu/%llu)\n", p.number,
                        static_cast<unsigned long long>(p.firstLba), static_cast<unsigned long long>(q.firstLba),
                        static_cast<unsigned long long>(p.sectorCount), static_cast<unsigned long long>(q.sectorCount));
            return false;
        }
    }
    return true;
}

}  // namespace

// --- JSON parser ---------------------------------------------------------------

TEST("json parser reads objects, arrays, escapes and exact integers") {
    JsonValue v;
    std::string err;
    // JSON \u escapes are spelled with a double backslash: MSVC would
    // otherwise treat them as C++ universal character names.
    REQUIRE(parseJson("{\"a\":[1,2,{\"b\":true}],\"s\":\"x\\\"\\u00e4\\ud83d\\ude00\",\"n\":18446744073709551615,"
                      "\"f\":-1.5,\"z\":null}",
                      v, err));
    CHECK(v.isObject());
    CHECK_EQ(v.find("a")->array.size(), std::size_t{3});
    CHECK(v.find("a")->array[2].getBool("b"));
    CHECK_EQ(v.getString("s"), std::string("x\"\xC3\xA4\xF0\x9F\x98\x80"));
    CHECK_EQ(v.getUint("n"), 18446744073709551615ull);
    bool ok = true;
    CHECK_EQ(v.getUint("f", 7, &ok), 7u);
    CHECK(!ok);
}

TEST("json parser rejects malformed input and enforces limits") {
    JsonValue v;
    std::string err;
    for (const char* bad : {"", "{", "[1,]", "{\"a\":1,}", "{\"a\" 1}", "01", "\"\\x\"", "\"a", "[1] x",
                            "{\"a\":1,\"a\":2}", "\"\\ud800\"", "tru", "\"\x01\""})
        CHECK(!parseJson(bad, v, err));
    std::string deep(100, '[');
    deep += std::string(100, ']');
    CHECK(!parseJson(deep, v, err));
    JsonParseLimits small;
    small.maxBytes = 4;
    CHECK(!parseJson("[1,2,3]", v, err, small));
}

TEST("json parser survives random input") {
    std::mt19937 rng(4711);
    const char alphabet[] = "{}[]\",:0123456789-.eEtrufalsn\\u \t";
    JsonValue v;
    std::string err;
    for (int i = 0; i < 3000; ++i) {
        std::string s;
        int n = static_cast<int>(rng() % 40);
        for (int k = 0; k < n; ++k) s += alphabet[rng() % (sizeof alphabet - 1)];
        parseJson(s, v, err);
    }
    CHECK(true);
}

// --- foreign tables -------------------------------------------------------------

TEST("foreign partition tables are recognised instead of shown as empty") {
    struct Case { const char* name; void (*write)(Image&); };
    const Case cases[] = {
        {"mac", [](Image& i) { i.putStr(0, "ER", 2); i.b(2) = 0x02; i.b(3) = 0x00; i.putStr(512, "PM", 2); }},
        {"dvh", [](Image& i) { i.b(0) = 0x0B; i.b(1) = 0xE5; i.b(2) = 0xA9; i.b(3) = 0x41; }},
        {"aix", [](Image& i) { i.b(0) = 0xC9; i.b(1) = 0xC2; i.b(2) = 0xD4; i.b(3) = 0xC1; }},
        {"sun", [](Image& i) { i.b(508) = 0xDA; i.b(509) = 0xBE; }},
        {"bsd", [](Image& i) { i.put32(512, 0x82564557u); }},
        {"amiga", [](Image& i) { i.putStr(1024, "RDSK", 4); }},
    };
    for (const auto& c : cases) {
        Image im(4 * MiB, 512);
        c.write(im);
        DiskLayout l = parseImage(im);
        if (l.foreignTable != c.name) std::printf("    case %s detected as '%s'\n", c.name, l.foreignTable.c_str());
        CHECK(l.type == TableType::Unknown);
        CHECK_EQ(l.foreignTable, std::string(c.name));
    }
}

// --- operations and queue --------------------------------------------------------

TEST("queue: create, format merge, undo restores the exact previous layout") {
    Image im(64 * MiB, 512);
    im.gpt({{kBasic, 2048, 20479, "data", 0}});
    OperationQueue q = queueFor(im);
    std::string err;
    REQUIRE(q.add(create(20480, 40960), err));
    const std::uint32_t id = q.operations().back().result;
    CHECK(id == 2);
    const DiskLayout afterCreate = q.current();
    Operation fmt = onTarget(OpType::Format, id);
    fmt.fs = FsType::Ext4;
    fmt.label = "home";
    REQUIRE(q.add(fmt, err));
    CHECK_EQ(q.operations().size(), std::size_t{1});  // merged into the create operation
    CHECK(findPlanned(q.current(), id)->fs.type == FsType::Ext4);
    CHECK(findPlanned(q.current(), id)->typeGuid == Guid{} ? false : true);
    REQUIRE(q.undoLast());
    CHECK(findPlanned(q.current(), id)->fs.type == FsType::Unknown);
    CHECK(sameGeometry(q.current(), afterCreate));
    q.clear();
    CHECK(q.empty());
    CHECK(sameGeometry(q.current(), q.original()));
}

TEST("queue: deleting a planned partition cancels its creation") {
    Image im(64 * MiB, 512);
    im.gpt({});
    OperationQueue q = queueFor(im);
    std::string err;
    REQUIRE(q.add(create(2048, 4096, FsType::Ntfs), err));
    const std::uint32_t a = q.operations().back().result;
    REQUIRE(q.add(create(8192, 4096), err));
    const std::uint32_t b = q.operations().back().result;
    Operation lbl = onTarget(OpType::SetName, a);
    lbl.name = "x";
    REQUIRE(q.add(lbl, err));
    REQUIRE(q.add(onTarget(OpType::DeletePartition, a), err));
    REQUIRE(q.operations().size() == 1);
    CHECK_EQ(q.operations()[0].result, b);  // the surviving partition keeps its id
    CHECK(findPlanned(q.current(), b) != nullptr);
    CHECK(findPlanned(q.current(), a) == nullptr);
}

TEST("queue: consecutive resizes merge, failed operations leave the plan unchanged") {
    Image im(64 * MiB, 512);
    im.gpt({{kLinux, 2048, 20479, "root", 0}});
    OperationQueue q = queueFor(im);
    std::string err;
    Operation r1 = onTarget(OpType::ResizeMove, 1);
    r1.firstLba = 2048;
    r1.sectorCount = 30000;
    REQUIRE(q.add(r1, err));
    Operation r2 = r1;
    r2.sectorCount = 40000;
    REQUIRE(q.add(r2, err));
    CHECK_EQ(q.operations().size(), std::size_t{1});
    CHECK_EQ(findPlanned(q.current(), 1)->sectorCount, 40000u);
    CHECK(q.operations()[0].hasOriginal);
    CHECK_EQ(q.operations()[0].original.sectorCount, 20479u - 2048 + 1);
    Operation bad = onTarget(OpType::SetName, 99);
    CHECK(!q.add(bad, err));
    CHECK_EQ(q.operations().size(), std::size_t{1});
    Operation mbrOnly = create(50000, 100, FsType::Unknown, PartitionKind::Logical);
    CHECK(!q.add(mbrOnly, err));  // GPT has no logical partitions
}

TEST("operations: structural errors are reported") {
    Image im(64 * MiB, 512);
    im.mbrEntry(0, 0, 0x0F, 2048, 60000);
    im.mbrEntry(2048, 0, 0x83, 2048, 1000);
    OperationQueue q = queueFor(im);
    std::string err;
    CHECK(!q.add(onTarget(OpType::DeletePartition, 1), err));  // extended still has a logical
    Operation name = onTarget(OpType::SetName, 2);
    name.name = "nope";
    CHECK(!q.add(name, err));  // names exist only on GPT
    Operation longName;
    Image g(64 * MiB, 512);
    g.gpt({{kLinux, 2048, 4095, "a", 0}});
    OperationQueue gq = queueFor(g);
    longName = onTarget(OpType::SetName, 1);
    longName.name = std::string(37, 'x');
    CHECK(!gq.add(longName, err));
    CHECK(!gq.add(onTarget(OpType::Check, 99), err));
    Operation tbl;
    tbl.type = OpType::CreateTable;
    tbl.table = TableType::None;
    CHECK(!gq.add(tbl, err));
}

TEST("flags: GPT type and attribute flags, MBR boot/hidden/lba round trip") {
    PartitionEntry p;
    p.fs.type = FsType::Ntfs;
    applyDefaultType(TableType::Gpt, p, FsType::Ntfs);
    std::string err;
    REQUIRE(setPartitionFlag(TableType::Gpt, p, "esp", true, err));
    Guid esp;
    Guid::parse(kEsp, esp);
    CHECK(p.typeGuid == esp);
    REQUIRE(setPartitionFlag(TableType::Gpt, p, "esp", false, err));
    Guid basic;
    Guid::parse(kBasic, basic);
    CHECK(p.typeGuid == basic);
    REQUIRE(setPartitionFlag(TableType::Gpt, p, "hidden", true, err));
    REQUIRE(setPartitionFlag(TableType::Gpt, p, "no_automount", true, err));
    CHECK_EQ(p.gptAttributes, (1ull << 62) | (1ull << 63));
    REQUIRE(setPartitionFlag(TableType::Gpt, p, "hidden", false, err));
    CHECK_EQ(p.gptAttributes, 1ull << 63);
    CHECK(!setPartitionFlag(TableType::Gpt, p, "lba", true, err));

    PartitionEntry m;
    m.fs.type = FsType::Fat32;
    applyDefaultType(TableType::Mbr, m, FsType::Fat32);
    CHECK_EQ(m.mbrType, 0x0C);
    REQUIRE(setPartitionFlag(TableType::Mbr, m, "hidden", true, err));
    CHECK_EQ(m.mbrType, 0x1C);
    REQUIRE(setPartitionFlag(TableType::Mbr, m, "hidden", false, err));
    REQUIRE(setPartitionFlag(TableType::Mbr, m, "lba", false, err));
    CHECK_EQ(m.mbrType, 0x0B);
    REQUIRE(setPartitionFlag(TableType::Mbr, m, "boot", true, err));
    CHECK(m.mbrBootable);
    CHECK(!setPartitionFlag(TableType::Mbr, m, "msftres", true, err));
}

TEST("plan JSON round trip and stale-plan detection") {
    Image im(64 * MiB, 512);
    im.gpt({{kLinux, 2048, 20479, "root", 0}});
    OperationQueue q = queueFor(im);
    std::string err;
    Operation r = onTarget(OpType::ResizeMove, 1);
    r.firstLba = 2048;
    r.sectorCount = 30000;
    REQUIRE(q.add(r, err));
    Operation c = create(40960, 8192, FsType::Ext4);
    c.label = "data";
    c.name = "Data \"1\"";
    REQUIRE(q.add(c, err));
    Operation f = onTarget(OpType::SetFlags, 1);
    f.flagsOn = {"legacy_boot"};
    REQUIRE(q.add(f, err));
    const std::string json = q.toJson();

    OperationQueue back;
    REQUIRE(OperationQueue::fromJson(json, q.original(), back, err));
    CHECK_EQ(back.operations().size(), q.operations().size());
    CHECK(sameGeometry(back.current(), q.current()));
    CHECK_EQ(back.identity().serial, std::string("TEST-SERIAL"));
    CHECK_EQ(back.toJson(), json);

    // The disk changed after planning: partition 1 was resized elsewhere.
    Image changed(64 * MiB, 512);
    changed.gpt({{kLinux, 2048, 25000, "root", 0}});
    DiskLayout other = parseImage(changed);
    OperationQueue stale;
    CHECK(!OperationQueue::fromJson(json, other, stale, err));
    CHECK(err.find("no longer matches") != std::string::npos);

    CHECK(!OperationQueue::fromJson("{\"schema\":\"other\"}", q.original(), stale, err));
    CHECK(!OperationQueue::fromJson("not json", q.original(), stale, err));
}

TEST("plan JSON import survives mutated documents") {
    Image im(64 * MiB, 512);
    im.gpt({{kLinux, 2048, 20479, "root", 0}});
    OperationQueue q = queueFor(im);
    std::string err;
    REQUIRE(q.add(create(40960, 8192, FsType::Ext4), err));
    const std::string json = q.toJson();
    std::mt19937 rng(99);
    for (int i = 0; i < 1500; ++i) {
        std::string s = json;
        for (int m = 0; m < 3; ++m) s[rng() % s.size()] = static_cast<char>(32 + rng() % 95);
        OperationQueue out;
        OperationQueue::fromJson(s, q.original(), out, err);
    }
    CHECK(true);
}

TEST("descriptions and risk levels") {
    Image im(64 * MiB, 512);
    im.gpt({{kLinux, 2048, 20479, "root", 0}});
    OperationQueue q = queueFor(im);
    std::string err;
    REQUIRE(q.add(onTarget(OpType::DeletePartition, 1), err));
    auto d = q.descriptions();
    REQUIRE(d.size() == 1);
    CHECK(d[0].find("Delete partition #1") == 0);
    CHECK(riskOf(q.operations()[0]) == Risk::High);
    Operation t;
    t.type = OpType::CreateTable;
    CHECK(riskOf(t) == Risk::Critical);
    CHECK(std::string(opTypeName(OpType::ResizeMove)) == "resize-move");
    OpType parsed;
    CHECK(opTypeFromName("set-flags", parsed) && parsed == OpType::SetFlags);
    CHECK(!opTypeFromName("rm -rf", parsed));
}

// --- validation engine -------------------------------------------------------------

TEST("validation: an empty plan has no diagnostics") {
    Image im(64 * MiB, 512);
    im.gpt({{kLinux, 2048, 20479, "root", 0}});
    OperationQueue q = queueFor(im);
    ValidationContext ctx;
    CHECK(validatePlan(q, ctx).diagnostics.empty());
}

TEST("validation: geometry rules (bounds, overlap, alignment, minimum size)") {
    Image im(64 * MiB, 512);
    im.gpt({{kLinux, 2048, 20479, "root", 0}});
    std::string err;
    ValidationContext ctx;
    {
        OperationQueue q = queueFor(im);
        REQUIRE(q.add(create(20480, 4096), err));  // directly after #1, aligned
        CHECK(validatePlan(q, ctx).ok());
    }
    {
        OperationQueue q = queueFor(im);
        REQUIRE(q.add(create(10000, 20000), err));  // overlaps #1
        CHECK(hasCode(validatePlan(q, ctx), "layout.overlap", Severity::Error));
    }
    {
        OperationQueue q = queueFor(im);
        REQUIRE(q.add(create(20480, 999999999), err));
        CHECK(hasCode(validatePlan(q, ctx), "layout.out_of_bounds", Severity::Error));
    }
    {
        OperationQueue q = queueFor(im);
        REQUIRE(q.add(create(20481, 4096), err));
        auto r = validatePlan(q, ctx);
        CHECK(hasCode(r, "layout.unaligned", Severity::Warning));
        CHECK(r.ok());  // warning only
        ValidationContext phys;
        phys.physicalSectorSize = 4096;
        CHECK(hasCode(validatePlan(q, phys), "layout.physical_unaligned", Severity::Warning));
    }
    {
        OperationQueue q = queueFor(im);
        REQUIRE(q.add(create(20480, 2048, FsType::Fat32), err));  // 1 MiB FAT32
        CHECK(hasCode(validatePlan(q, ctx), "op.too_small", Severity::Error));
    }
}

TEST("validation: MBR limits (primaries, extended, logical placement, bootable)") {
    Image im(64 * MiB, 512);
    im.mbrEntry(0, 0, 0x83, 2048, 4096);
    std::string err;
    ValidationContext ctx;
    {
        OperationQueue q = queueFor(im);
        REQUIRE(q.add(create(8192, 20000, FsType::Unknown, PartitionKind::Extended), err));
        REQUIRE(q.add(create(8192 + 2048, 4096, FsType::Unknown, PartitionKind::Logical), err));
        REQUIRE(q.add(create(8192 + 8192, 4096, FsType::Unknown, PartitionKind::Logical), err));
        CHECK(validatePlan(q, ctx).ok());
    }
    {
        OperationQueue q = queueFor(im);
        REQUIRE(q.add(create(8192, 20000, FsType::Unknown, PartitionKind::Extended), err));
        REQUIRE(q.add(create(8192, 4096, FsType::Unknown, PartitionKind::Logical), err));  // no room for its EBR
        CHECK(hasCode(validatePlan(q, ctx), "layout.no_room_for_ebr", Severity::Error));
    }
    {
        OperationQueue q = queueFor(im);
        REQUIRE(q.add(create(8192, 2048, FsType::Unknown, PartitionKind::Extended), err));
        REQUIRE(q.add(create(20480, 2048, FsType::Unknown, PartitionKind::Logical), err));
        CHECK(hasCode(validatePlan(q, ctx), "layout.logical_outside_extended", Severity::Error));
    }
    {
        OperationQueue q = queueFor(im);
        REQUIRE(q.add(create(8192, 2048), err));
        REQUIRE(q.add(create(12288, 2048), err));
        REQUIRE(q.add(create(16384, 2048), err));
        CHECK(!q.add(create(20480, 2048), err));  // all four primary entries in use
        Operation b1 = onTarget(OpType::SetFlags, 1);
        b1.flagsOn = {"boot"};
        REQUIRE(q.add(b1, err));
        Operation b2 = onTarget(OpType::SetFlags, q.operations()[0].result);
        b2.flagsOn = {"boot"};
        REQUIRE(q.add(b2, err));
        CHECK(hasCode(validatePlan(q, ctx), "mbr.multiple_bootable", Severity::Warning));
    }
}

TEST("validation: protection of system, mounted and encrypted partitions") {
    Image im(256 * MiB, 512);
    im.gpt({{kEsp, 2048, 206847, "EFI", 0}, {kBasic, 206848, 400000, "C", 0}, {kBasic, 400001, 450000, "D", 0}});
    std::memcpy(im.at(400001ull * 512 + 3), "-FVE-FS-", 8);
    std::string err;
    {
        OperationQueue q = queueFor(im);
        REQUIRE(q.add(onTarget(OpType::DeletePartition, 1), err));
        CHECK(hasCode(validatePlan(q, ValidationContext{}), "op.system_critical", Severity::Error));
    }
    {
        OperationQueue q = queueFor(im);
        REQUIRE(q.add(onTarget(OpType::DeletePartition, 2), err));
        ValidationContext ctx;
        ctx.state[2].containsWindows = true;
        CHECK(hasCode(validatePlan(q, ctx), "op.system_critical", Severity::Error));
        ValidationContext mounted;
        mounted.state[2].mounted = true;
        auto r = validatePlan(q, mounted);
        CHECK(hasCode(r, "op.busy", Severity::Warning));  // planning mode
        mounted.mode = ValidationMode::Execution;
        CHECK(hasCode(validatePlan(q, mounted), "op.busy", Severity::Error));
    }
    {
        OperationQueue q = queueFor(im);
        Operation rs = onTarget(OpType::ResizeMove, 3);
        rs.firstLba = 400001;
        rs.sectorCount = 40000;
        REQUIRE(q.add(rs, err));
        CHECK(hasCode(validatePlan(q, ValidationContext{}), "op.encrypted", Severity::Error));
    }
    {
        OperationQueue q = queueFor(im);
        Operation tbl;
        tbl.type = OpType::CreateTable;
        tbl.table = TableType::Mbr;
        REQUIRE(q.add(tbl, err));
        CHECK(hasCode(validatePlan(q, ValidationContext{}), "op.system_critical", Severity::Error));  // wipes the ESP
    }
}

TEST("validation: capability gaps are warnings when planning and errors for execution") {
    Image im(64 * MiB, 512);
    im.gpt({{kBasic, 2048, 40959, "data", 0}});
    wptest::writeNtfs(im, 2048ull * 512);
    std::string err;
    OperationQueue q = queueFor(im);
    Operation rs = onTarget(OpType::ResizeMove, 1);
    rs.firstLba = 2048;
    rs.sectorCount = 30000;
    REQUIRE(q.add(rs, err));
    ValidationContext plan;
    auto pr = validatePlan(q, plan);
    CHECK(hasCode(pr, "op.capability", Severity::Warning));
    CHECK(pr.ok());
    ValidationContext exec;
    exec.mode = ValidationMode::Execution;
    exec.haveCurrentIdentity = true;
    exec.currentIdentity = q.identity();
    auto er = validatePlan(q, exec);
    CHECK(hasCode(er, "op.capability", Severity::Error));
    CHECK(hasCode(er, "execution.unavailable", Severity::Error));
    CHECK(!er.ok());
}

TEST("validation: identity mismatch, used space, write protection, source errors") {
    Image im(64 * MiB, 512);
    im.gpt({{kLinux, 2048, 40959, "data", 0}});
    std::string err;
    OperationQueue q = queueFor(im);
    Operation rs = onTarget(OpType::ResizeMove, 1);
    rs.firstLba = 2048;
    rs.sectorCount = 4096;  // 2 MiB
    REQUIRE(q.add(rs, err));
    ValidationContext ctx;
    ctx.state[1].usedBytes = 10 * MiB;
    CHECK(hasCode(validatePlan(q, ctx), "op.shrink_below_used", Severity::Error));
    ValidationContext wp;
    wp.deviceWriteProtected = true;
    CHECK(hasCode(validatePlan(q, wp), "device.write_protected", Severity::Error));
    ValidationContext exec;
    exec.mode = ValidationMode::Execution;
    exec.haveCurrentIdentity = true;
    exec.currentIdentity = q.identity();
    exec.currentIdentity.serial = "OTHER";
    CHECK(hasCode(validatePlan(q, exec), "plan.identity_mismatch", Severity::Error));
    exec.haveCurrentIdentity = false;
    CHECK(hasCode(validatePlan(q, exec), "plan.identity_unknown", Severity::Error));

    Image broken(64 * MiB, 512);
    broken.gpt({{kLinux, 2048, 10000, "a", 0}, {kLinux, 9000, 20000, "b", 0}});  // overlapping on disk
    OperationQueue bq = queueFor(broken);
    Operation lbl = onTarget(OpType::SetName, 1);
    lbl.name = "x";
    REQUIRE(bq.add(lbl, err));
    CHECK(hasCode(validatePlan(bq, ValidationContext{}), "plan.source_errors", Severity::Error));
}

TEST("validation: random plans never validate with overlapping partitions") {
    std::mt19937 rng(2026);
    for (int iter = 0; iter < 200; ++iter) {
        Image im(32 * MiB, 512);
        im.gpt({});
        OperationQueue q = queueFor(im);
        std::string err;
        for (int k = 0; k < 6; ++k) {
            std::uint64_t first = 34 + rng() % 60000;
            std::uint64_t count = 1 + rng() % 20000;
            q.add(create(first, count), err);
        }
        ValidationContext ctx;
        ctx.alignmentBytes = 0;
        if (!validatePlan(q, ctx).ok()) continue;
        auto parts = q.current().partitions;
        std::sort(parts.begin(), parts.end(), [](auto& a, auto& b) { return a.firstLba < b.firstLba; });
        for (std::size_t i = 1; i < parts.size(); ++i) CHECK(parts[i].firstLba > parts[i - 1].lastLba());
        for (const auto& p : parts) CHECK(p.lastLba() <= q.current().lastUsableLba);
    }
}

// --- image test harness: simulate -> serialise -> apply -> parse -------------------

TEST("harness: GPT plan written to an image parses back identically") {
    Image im(128 * MiB, 512);
    im.gpt({{kEsp, 2048, 206847 / 4, "EFI", 0}, {kLinux, 65536, 131071, "old", 0}});
    std::string err;
    OperationQueue q = queueFor(im);
    REQUIRE(q.add(onTarget(OpType::DeletePartition, 2), err));
    Operation c = create(65536, 100000, FsType::Ext4);
    c.name = "Linux \xC3\xA4";
    REQUIRE(q.add(c, err));
    Operation f = onTarget(OpType::SetFlags, q.operations().back().result);
    f.flagsOn = {"legacy_boot", "linux-home"};
    REQUIRE(q.add(f, err));
    CHECK(validatePlan(q, ValidationContext{}).ok());
    DiskLayout back = applyToImage(im, q.current());
    CHECK(back.type == TableType::Gpt);
    CHECK(back.gptPrimaryValid && back.gptBackupValid);
    CHECK(!back.hasErrors());
    CHECK(sameGeometry(back, [&] {
        DiskLayout x = q.current();
        assignNewIdentifiers(x);
        return x;
    }()));
    CHECK(back.diskGuid == q.original().diskGuid);  // existing disk GUID kept
}

TEST("harness: MBR with extended and logical partitions round trip, boot code kept") {
    Image im(128 * MiB, 512);
    im.b(0) = 0xFA;  // boot code marker
    im.b(100) = 0x42;
    im.mbrEntry(0, 0, 0x07, 2048, 20480, true);
    im.put32(440, 0xCAFEBABE);
    std::string err;
    OperationQueue q = queueFor(im);
    REQUIRE(q.add(create(24576, 200000, FsType::Unknown, PartitionKind::Extended), err));
    REQUIRE(q.add(create(24576 + 2048, 40960, FsType::Ext4, PartitionKind::Logical), err));
    REQUIRE(q.add(create(24576 + 2048 + 40960 + 2048, 40960, FsType::LinuxSwap, PartitionKind::Logical), err));
    REQUIRE(validatePlan(q, ValidationContext{}).ok());
    DiskLayout planned = q.current();
    DiskLayout back = applyToImage(im, planned);
    CHECK(back.type == TableType::Mbr);
    CHECK(!back.hasErrors());
    CHECK(sameGeometry(back, planned));
    CHECK_EQ(back.mbrSignature, 0xCAFEBABEu);
    CHECK_EQ(im.b(0), 0xFA);
    CHECK_EQ(im.b(100), 0x42);
}

TEST("harness: converting GPT to msdos removes the stale GPT, and back") {
    Image im(64 * MiB, 512);
    im.gpt({{kLinux, 2048, 4095, "a", 0}});
    std::string err;
    {
        OperationQueue q = queueFor(im);
        Operation t;
        t.type = OpType::CreateTable;
        t.table = TableType::Mbr;
        REQUIRE(q.add(t, err));
        REQUIRE(q.add(create(2048, 8192, FsType::Fat32), err));
        DiskLayout back = applyToImage(im, q.current());
        CHECK(back.type == TableType::Mbr);
        CHECK_EQ(back.partitions.size(), std::size_t{1});
        CHECK(std::memcmp(im.at(512), "EFI PART", 8) != 0);
    }
    {
        OperationQueue q = queueFor(im);
        Operation t;
        t.type = OpType::CreateTable;
        t.table = TableType::Gpt;
        REQUIRE(q.add(t, err));
        DiskLayout back = applyToImage(im, q.current());
        CHECK(back.type == TableType::Gpt);
        CHECK(back.partitions.empty());
        CHECK(!back.diskGuid.isZero());
    }
}

TEST("harness: 4096-byte sectors") {
    Image im(64 * MiB, 4096);
    im.gpt({});
    std::string err;
    OperationQueue q = queueFor(im);
    REQUIRE(q.add(create(256, 4096, FsType::Ntfs), err));
    REQUIRE(validatePlan(q, ValidationContext{}).ok());
    DiskLayout back = applyToImage(im, q.current());
    CHECK(back.type == TableType::Gpt);
    CHECK(!back.hasErrors());
    REQUIRE(back.partitions.size() == 1);
    CHECK_EQ(back.partitions[0].firstLba, 256u);
}

TEST("serializer refuses unassigned identifiers and impossible layouts") {
    DiskLayout l;
    l.type = TableType::Gpt;
    l.sectorSize = 512;
    l.diskBytes = 64 * MiB;
    l.firstUsableLba = 34;
    l.lastUsableLba = l.totalSectors() - 34;
    std::vector<SectorWrite> w;
    std::string err;
    CHECK(!serializeLayout(l, SerializeOptions{}, w, err));  // disk GUID missing
    l.diskGuid = randomGuid();
    l.firstUsableLba = 1;
    CHECK(!serializeLayout(l, SerializeOptions{}, w, err));  // overlaps GPT structures
    DiskLayout m;
    m.type = TableType::Mbr;
    m.sectorSize = 512;
    m.diskBytes = 64 * MiB;
    PartitionEntry p;
    p.kind = PartitionKind::Logical;
    p.number = 5;
    p.firstLba = 4096;
    p.sectorCount = 100;
    m.partitions.push_back(p);
    CHECK(!serializeLayout(m, SerializeOptions{}, w, err));  // logical without extended
    Guid g = randomGuid();
    CHECK((g.bytes[7] & 0xF0) == 0x40);
    CHECK((g.bytes[8] & 0xC0) == 0x80);
}

TEST("utf8 to utf16 encoding") {
    std::uint8_t buf[8];
    CHECK(utf8ToUtf16le("ab", buf, 4));
    CHECK(buf[0] == 'a' && buf[2] == 'b' && buf[4] == 0);
    CHECK(utf8ToUtf16le("\xF0\x9F\x98\x80", buf, 2));
    CHECK(buf[0] == 0x3D && buf[1] == 0xD8);
    CHECK(!utf8ToUtf16le("\xF0\x9F\x98\x80", buf, 1));
    CHECK(!utf8ToUtf16le("\xC3", buf, 4));
    CHECK(!utf8ToUtf16le("abcde", buf, 4));
}
