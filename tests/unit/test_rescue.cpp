// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#include <atomic>
#include <cstring>

#include "image_builder.hpp"
#include "test_framework.hpp"
#include "wparted/core/operation_log.hpp"
#include "wparted/core/rescue.hpp"
#include "wparted/core/validation.hpp"

using namespace wparted;
using wptest::Image;

namespace {

constexpr std::uint64_t MiB = 1024 * 1024;
constexpr const char* kLinux = "0FC63DAF-8483-4772-8E79-3D69D8477DE4";

void ntfsWithSize(Image& im, std::uint64_t base, std::uint64_t sectors) {
    wptest::writeNtfs(im, base);
    im.put16(base + 0x0B, 512);
    im.put64(base + 0x28, sectors);
}

DiskLayout parse(const Image& im) {
    MemoryBlockReader r(im.data, im.ss);
    return parseDiskLayout(r, true);
}

}  // namespace

TEST("rescue: finds an NTFS file system whose GPT entry was deleted") {
    Image im(64 * MiB, 512);
    im.gpt({{kLinux, 2048, 4095, "kept", 0}});
    ntfsWithSize(im, 8192ull * 512, 20000);  // lost partition at 4 MiB
    DiskLayout l = parse(im);
    MemoryBlockReader r(im.data, 512);
    auto found = rescueScan(r, l);
    REQUIRE(found.size() == 1);
    CHECK_EQ(found[0].firstLba, 8192u);
    CHECK(found[0].fs.type == FsType::Ntfs);
    CHECK_EQ(found[0].sectorCount, 20001u);  // incl. backup boot sector
}

TEST("rescue: finds ext4 at a legacy cylinder boundary on an empty MBR") {
    Image im(64 * MiB, 512);
    im.b(510) = 0x55;
    im.b(511) = 0xAA;
    const std::uint64_t lba = 255ull * 63;  // first cylinder boundary
    wptest::writeExt4(im, lba * 512, "lost");
    im.put32(lba * 512 + 1024 + 4, 4096);   // blocks_count
    im.put32(lba * 512 + 1024 + 24, 0);     // 1 KiB blocks
    DiskLayout l = parse(im);
    REQUIRE(l.type == TableType::Mbr);
    MemoryBlockReader r(im.data, 512);
    auto found = rescueScan(r, l);
    REQUIRE(found.size() == 1);
    CHECK_EQ(found[0].firstLba, lba);
    CHECK(found[0].fs.type == FsType::Ext4);
    CHECK_EQ(found[0].fs.label, std::string("lost"));
    CHECK_EQ(found[0].sectorCount, 8192u);
}

TEST("rescue: blank device without table is scanned completely, partitions are skipped") {
    Image im(64 * MiB, 512);
    wptest::writeFat32(im, 4096ull * 512, "OLDSTICK   ");
    DiskLayout l = parse(im);
    REQUIRE(l.type == TableType::None);
    MemoryBlockReader r(im.data, 512);
    auto found = rescueScan(r, l);
    REQUIRE(found.size() == 1);
    CHECK(found[0].fs.type == FsType::Fat32);
    CHECK_EQ(found[0].fs.label, std::string("OLDSTICK"));

    Image used(64 * MiB, 512);  // the same file system inside an existing partition is not "lost"
    used.gpt({{kLinux, 4096, 60000, "p", 0}});
    wptest::writeFat32(used, 4096ull * 512, "INUSE      ");
    MemoryBlockReader r2(used.data, 512);
    CHECK(rescueScan(r2, parse(used)).empty());
}

TEST("rescue: cancellation and progress") {
    Image im(32 * MiB, 512);
    DiskLayout l = parse(im);
    MemoryBlockReader r(im.data, 512);
    std::atomic<bool> cancel{true};
    RescueOptions o;
    o.cancel = &cancel;
    CHECK(rescueScan(r, l, o).empty());
    std::uint64_t last = 0, total = 0;
    RescueOptions p;
    p.progress = [&](std::uint64_t d, std::uint64_t t) {
        last = d;
        total = t;
    };
    rescueScan(r, l, p);
    CHECK(total > 0);
    CHECK_EQ(last, total);
}

TEST("rescue: file system sizes from headers") {
    Image im(8 * MiB, 512);
    wptest::writeFat32(im, 0, "X          ");
    MemoryBlockReader r(im.data, 512);
    CHECK_EQ(filesystemSizeBytes(r, FsType::Fat32), 1048576ull * 512);
    CHECK_EQ(filesystemSizeBytes(r, FsType::Apfs), 0u);
}

// --- operation log ---------------------------------------------------------------

TEST("operation log: plan log, JSON and escaped HTML") {
    Image im(64 * MiB, 512);
    im.gpt({{kLinux, 2048, 20479, "<root>", 0}});
    DiskLayout l = parse(im);
    OperationQueue q(l, identityOf(l, "S", "M"));
    Operation del;
    del.type = OpType::DeletePartition;
    del.target = 1;
    std::string err;
    REQUIRE(q.add(del, err));
    ValidationContext ctx;
    ctx.mode = ValidationMode::Execution;
    ctx.haveCurrentIdentity = true;
    ctx.currentIdentity = q.identity();
    const auto vr = validatePlan(q, ctx);
    const LogStep log = planLog(q, vr, "Disk <1> & \"x\"");
    CHECK(log.status == LogStep::Status::Error);  // execution is unavailable in this build
    REQUIRE(log.children.size() == 1);
    CHECK(log.children[0].status == LogStep::Status::Skipped);
    const std::string json = logToJson(log);
    CHECK(json.find("\"status\":\"error\"") != std::string::npos);
    const std::string html = logToHtml(log, "Plan <b>");
    CHECK(html.find("<b>") == std::string::npos);
    CHECK(html.find("Plan &lt;b&gt;") != std::string::npos);
    CHECK(html.find("Disk &lt;1&gt; &amp; &quot;x&quot;") != std::string::npos);
    CHECK_EQ(htmlEscape("a'b"), std::string("a&#39;b"));
}

// --- dynamic disks / Storage Spaces (WP-072) ---------------------------------------

TEST("validation: disks managed by Windows (LDM, Storage Spaces) are refused") {
    Image im(64 * MiB, 512);
    im.gpt({{"5808C8AA-7E8F-42E0-85D2-E1E90434CFB3", 2048, 4095, "LDM metadata", 0},
            {"AF9B60A0-1431-4F62-BC68-3311714A69AD", 4096, 60000, "LDM data", 0}});
    DiskLayout l = parse(im);
    CHECK(isSystemCriticalType(l, l.partitions[0]));
    CHECK(isSystemCriticalType(l, l.partitions[1]));
    OperationQueue q(l, identityOf(l, "S", "M"));
    Operation del;
    del.type = OpType::DeletePartition;
    del.target = 2;
    std::string err;
    REQUIRE(q.add(del, err));
    ValidationContext ctx;
    ctx.managedByWindows = true;
    const auto vr = validatePlan(q, ctx);
    bool managed = false, critical = false;
    for (const auto& d : vr.diagnostics) {
        if (d.code == "device.managed_by_windows" && d.severity == Severity::Error) managed = true;
        if (d.code == "op.system_critical" && d.severity == Severity::Error) critical = true;
    }
    CHECK(managed);
    CHECK(critical);
}

TEST("validation: diagnostics carry operation number and arguments for translations") {
    Image im(64 * MiB, 512);
    im.gpt({{kLinux, 2048, 4095, "a", 0}, {kLinux, 4096, 8191, "b", 0}});
    DiskLayout l = parse(im);
    OperationQueue q(l, identityOf(l, "S", "M"));
    Operation del;
    del.type = OpType::DeletePartition;
    del.target = 2;
    Operation mk;
    mk.type = OpType::CreatePartition;
    mk.firstLba = 4096;
    mk.sectorCount = 4096;
    mk.fs = FsType::Ntfs;
    std::string err;
    REQUIRE(q.add(del, err));
    REQUIRE(q.add(mk, err));
    ValidationContext ctx;
    ctx.state[1].mounted = true;
    Operation fmt;
    fmt.type = OpType::Format;
    fmt.target = 1;
    fmt.fs = FsType::Ext4;
    REQUIRE(q.add(fmt, err));
    bool capability = false, busy = false;
    for (const auto& d : validatePlan(q, ctx).diagnostics) {
        if (d.code == "op.capability" && d.operation == 2) {
            REQUIRE(d.args.size() == 3);
            CHECK_EQ(d.args[0], std::string("create"));
            CHECK_EQ(d.args[1], std::string("ntfs"));
            CHECK_EQ(d.args[2], std::string(supportName(capabilitiesFor(FsType::Ntfs).create)));
            capability = true;
        }
        if (d.code == "op.busy") {
            CHECK_EQ(d.operation, 3);
            REQUIRE(d.args.size() == 1);
            CHECK_EQ(d.args[0], std::string("1"));
            busy = true;
        }
    }
    CHECK(capability);
    CHECK(busy);
}
