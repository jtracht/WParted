// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>

#include "image_builder.hpp"
#include "test_framework.hpp"
#include "wparted/core/block_reader.hpp"
#include "wparted/core/filesystem.hpp"
#include "wparted/core/fs_tools.hpp"
#include "wparted/core/json.hpp"
#include "wparted/core/layout_view.hpp"
#include "wparted/core/partition_table.hpp"

using namespace wparted;
using wptest::Image;

namespace {

constexpr const char* kEsp = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B";
constexpr const char* kMsr = "E3C9E316-0B5C-4DB8-817D-F92DF00215AE";
constexpr const char* kBasic = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7";
constexpr const char* kLinux = "0FC63DAF-8483-4772-8E79-3D69D8477DE4";

DiskLayout parse(const Image& im, bool contents = true) {
    MemoryBlockReader r(im.data, im.ss);
    return parseDiskLayout(r, contents);
}

bool hasDiag(const DiskLayout& l, const std::string& code) {
    for (const auto& d : l.diagnostics)
        if (d.code == code) return true;
    return false;
}

constexpr std::uint64_t MiB = 1024 * 1024;

}  // namespace

// --- primitives -------------------------------------------------------------

TEST("crc32 matches the IEEE reference value") {
    const char* s = "123456789";
    CHECK_EQ(crc32(reinterpret_cast<const std::uint8_t*>(s), 9), 0xCBF43926u);
    CHECK_EQ(crc32(nullptr, 0), 0u);
}

TEST("guid round-trips between text and on-disk order") {
    Guid g;
    REQUIRE(Guid::parse(kEsp, g));
    CHECK_EQ(g.bytes[0], 0x28);  // first field stored little endian
    CHECK_EQ(g.bytes[3], 0xC1);
    CHECK_EQ(g.bytes[8], 0xBA);  // last 8 bytes stored as-is
    CHECK_EQ(g.toString(), std::string(kEsp));
    CHECK(!Guid::parse("not-a-guid", g));
    CHECK(!Guid::parse("C12A7328-F81F-11D2-BA4B-00A0C93EC93Z", g));
}

TEST("utf16 names decode including surrogate pairs") {
    const std::uint8_t name[] = {'E', 0, 'F', 0, 'I', 0, 0x3D, 0xD8, 0x00, 0xDE, 0, 0};
    CHECK_EQ(utf16leToUtf8(name, 6), std::string("EFI\xF0\x9F\x98\x80"));
    const std::uint8_t lone[] = {0x00, 0xD8, 'A', 0};
    CHECK_EQ(utf16leToUtf8(lone, 2), std::string("\xEF\xBF\xBD" "A"));
}

TEST("formatSize uses binary units like GParted") {
    CHECK_EQ(formatSize(0), std::string("0 B"));
    CHECK_EQ(formatSize(1023), std::string("1023 B"));
    CHECK_EQ(formatSize(1024), std::string("1.00 KiB"));
    CHECK_EQ(formatSize(512 * MiB), std::string("512.00 MiB"));
    CHECK_EQ(formatSize(1536ull * 1024 * MiB), std::string("1.50 TiB"));
}

TEST("readers reject out-of-range reads") {
    MemoryBlockReader r(std::vector<std::uint8_t>(1024, 7), 512);
    std::uint8_t b[16];
    CHECK(r.read(1008, b, 16));
    CHECK(!r.read(1009, b, 16));
    CHECK(!r.read(~0ull, b, 1));
    SubRangeReader s(r, 512, 256);
    CHECK(s.read(240, b, 16));
    CHECK(!s.read(241, b, 16));
}

// --- GPT --------------------------------------------------------------------

TEST("gpt: typical Windows layout parses with types, names and flags") {
    Image im(256 * MiB, 512);
    im.gpt({{kEsp, 2048, 206847, "EFI system partition", 0},
            {kMsr, 206848, 239615, "Microsoft reserved partition", 0},
            {kBasic, 239616, 520191, "Basic data partition", 0}});
    writeNtfs(im, 239616ull * 512);
    DiskLayout l = parse(im);
    REQUIRE(l.type == TableType::Gpt);
    CHECK(l.gptPrimaryValid);
    CHECK(l.gptBackupValid);
    CHECK(!l.hasErrors());
    REQUIRE(l.partitions.size() == 3);
    CHECK_EQ(l.partitions[0].name, std::string("EFI system partition"));
    CHECK_EQ(partitionTypeDescription(l, l.partitions[0]), std::string("EFI System"));
    auto flags = partitionFlags(l, l.partitions[0]);
    CHECK(flags.size() == 2 && flags[0] == "boot" && flags[1] == "esp");
    CHECK(isSystemCriticalType(l, l.partitions[0]));
    CHECK(isSystemCriticalType(l, l.partitions[1]));
    CHECK(!isSystemCriticalType(l, l.partitions[2]));
    CHECK(l.partitions[2].fs.type == FsType::Ntfs);
    CHECK_EQ(l.partitions[2].fs.uuid, std::string("1122334455667788"));
    CHECK_EQ(l.diskGuid.toString(), std::string("DEADBEEF-0000-4000-8000-0123456789AB"));
}

TEST("gpt: corrupt primary header falls back to backup and reports an error") {
    Image im(64 * MiB, 512);
    im.gpt({{kLinux, 2048, 4095, "root", 0}});
    im.data[512 + 40] ^= 0xFF;  // damage primary header body -> CRC mismatch
    DiskLayout l = parse(im);
    REQUIRE(l.type == TableType::Gpt);
    CHECK(!l.gptPrimaryValid);
    CHECK(l.gptBackupValid);
    CHECK(hasDiag(l, "gpt.primary_invalid"));
    CHECK(l.partitions.size() == 1);
}

TEST("gpt: corrupt entry array is detected by CRC") {
    Image im(64 * MiB, 512);
    im.gpt({{kLinux, 2048, 4095, "root", 0}});
    im.data[2 * 512 + 60] ^= 0x01;           // primary array
    im.data[im.data.size() - 33 * 512 + 60] ^= 0x01;  // backup array (starts 33 sectors before the end)
    DiskLayout l = parse(im);
    CHECK(l.type == TableType::Unknown);
    CHECK(hasDiag(l, "gpt.no_valid_header"));
    CHECK(l.partitions.empty());
}

TEST("gpt: missing backup (image copied to larger disk) is a warning only") {
    Image im(64 * MiB, 512);
    im.gpt({{kLinux, 2048, 4095, "root", 0}});
    std::memset(im.at(im.data.size() - 512), 0, 512);
    DiskLayout l = parse(im);
    CHECK(l.type == TableType::Gpt);
    CHECK(hasDiag(l, "gpt.backup_invalid"));
    CHECK(!l.hasErrors());
}

TEST("gpt: 4096-byte logical sectors") {
    Image im(64 * MiB, 4096);
    im.gpt({{kBasic, 256, 8191, "data", 0}});
    writeFat32(im, 256ull * 4096, "USBSTICK   ");
    DiskLayout l = parse(im);
    REQUIRE(l.type == TableType::Gpt);
    CHECK_EQ(l.sectorSize, 4096u);
    REQUIRE(l.partitions.size() == 1);
    CHECK_EQ(l.partitions[0].sectorCount * 4096, (8191ull - 256 + 1) * 4096);
    CHECK(l.partitions[0].fs.type == FsType::Fat32);
    CHECK_EQ(l.partitions[0].fs.label, std::string("USBSTICK"));
}

TEST("gpt: sector size mismatch is diagnosed, not reinterpreted") {
    Image im(64 * MiB, 512);
    im.gpt({{kBasic, 2048, 4095, "data", 0}});
    MemoryBlockReader r(im.data, 4096);  // device claims 4K, table was written for 512
    DiskLayout l = parseDiskLayout(r);
    CHECK(l.type == TableType::Unknown);
    CHECK(hasDiag(l, "gpt.sector_size_mismatch"));
}

TEST("gpt: overlapping and out-of-range entries are errors") {
    Image im(64 * MiB, 512);
    im.gpt({{kLinux, 2048, 10000, "a", 0}, {kLinux, 9000, 20000, "b", 0}, {kLinux, 30000, 999999999, "c", 0}});
    DiskLayout l = parse(im);
    CHECK(hasDiag(l, "layout.overlap"));
    CHECK(hasDiag(l, "gpt.entry_outside_usable"));
    CHECK(hasDiag(l, "layout.beyond_device"));
}

TEST("gpt: attribute bits map to flags") {
    Image im(64 * MiB, 512);
    im.gpt({{kBasic, 2048, 4095, "hidden", (1ull << 62) | (1ull << 63) | (1ull << 2)}});
    DiskLayout l = parse(im);
    REQUIRE(l.partitions.size() == 1);
    auto f = partitionFlags(l, l.partitions[0]);
    CHECK(std::find(f.begin(), f.end(), "hidden") != f.end());
    CHECK(std::find(f.begin(), f.end(), "no_automount") != f.end());
    CHECK(std::find(f.begin(), f.end(), "legacy_boot") != f.end());
    CHECK(std::find(f.begin(), f.end(), "msftdata") != f.end());
}

// --- MBR --------------------------------------------------------------------

TEST("mbr: primaries plus extended with logical chain") {
    Image im(128 * MiB, 512);
    im.mbrEntry(0, 0, 0x07, 2048, 20480, true);
    im.mbrEntry(0, 1, 0x0F, 22528, 100000);
    // EBR 1 at 22528: logical at +2048, next EBR at ext+40960
    im.mbrEntry(22528, 0, 0x83, 2048, 18432);
    im.mbrEntry(22528, 1, 0x05, 40960, 30000);
    // EBR 2 at 22528+40960: logical swap, end of chain
    im.mbrEntry(22528 + 40960, 0, 0x82, 2048, 20000);
    writeNtfs(im, 2048ull * 512);
    writeExt4(im, (22528ull + 2048) * 512, "home");
    writeSwap(im, (22528ull + 40960 + 2048) * 512);
    im.put32(440, 0x1234ABCD);

    DiskLayout l = parse(im);
    REQUIRE(l.type == TableType::Mbr);
    CHECK_EQ(l.mbrSignature, 0x1234ABCDu);
    CHECK(!l.hasErrors());
    REQUIRE(l.partitions.size() == 4);
    CHECK(l.partitions[0].kind == PartitionKind::Primary);
    CHECK(l.partitions[0].mbrBootable);
    CHECK(l.partitions[0].fs.type == FsType::Ntfs);
    CHECK(l.partitions[1].kind == PartitionKind::Extended);
    CHECK(l.partitions[2].kind == PartitionKind::Logical);
    CHECK_EQ(l.partitions[2].number, 5);
    CHECK_EQ(l.partitions[2].firstLba, 22528ull + 2048);
    CHECK(l.partitions[2].fs.type == FsType::Ext4);
    CHECK_EQ(l.partitions[2].fs.label, std::string("home"));
    CHECK_EQ(l.partitions[3].number, 6);
    CHECK(l.partitions[3].fs.type == FsType::LinuxSwap);
    auto f = partitionFlags(l, l.partitions[1]);
    CHECK(std::find(f.begin(), f.end(), "lba") != f.end());
}

TEST("mbr: EBR loop is detected and terminated") {
    Image im(64 * MiB, 512);
    im.mbrEntry(0, 0, 0x05, 2048, 60000);
    im.mbrEntry(2048, 0, 0x83, 2048, 1000);
    im.mbrEntry(2048, 1, 0x05, 0, 60000);  // points back to itself
    DiskLayout l = parse(im);
    CHECK(hasDiag(l, "mbr.ebr_loop"));
    CHECK_EQ(l.partitions.size(), std::size_t{2});
}

TEST("mbr: empty valid table yields no partitions and one free segment") {
    Image im(64 * MiB, 512);
    im.data[510] = 0x55;
    im.data[511] = 0xAA;
    DiskLayout l = parse(im);
    CHECK(l.type == TableType::Mbr);
    CHECK(l.partitions.empty());
    auto segs = buildSegments(l);
    REQUIRE(segs.size() == 1);
    CHECK(segs[0].kind == Segment::Kind::Unallocated);
}

TEST("mbr: invalid boot indicator is not treated as a partition table") {
    Image im(64 * MiB, 512);
    im.mbrEntry(0, 0, 0x83, 2048, 1000);
    im.data[446] = 0x12;
    DiskLayout l = parse(im);
    CHECK(l.type == TableType::Unknown);
    CHECK(hasDiag(l, "mbr.invalid_entries"));
}

TEST("no table: whole-device FAT32 (superfloppy USB stick)") {
    Image im(64 * MiB, 512);
    writeFat32(im, 0, "STICK      ");
    DiskLayout l = parse(im);
    CHECK(l.type == TableType::None);
    REQUIRE(l.partitions.size() == 1);
    CHECK(l.partitions[0].kind == PartitionKind::WholeDevice);
    CHECK(l.partitions[0].fs.type == FsType::Fat32);
}

TEST("blank device has no table and no partitions") {
    Image im(8 * MiB, 512);
    DiskLayout l = parse(im);
    CHECK(l.type == TableType::None);
    CHECK(l.partitions.empty());
    CHECK(!l.hasErrors());
}

TEST("tiny device reports an error instead of reading out of bounds") {
    Image im(512, 512);
    DiskLayout l = parse(im);
    CHECK(l.type == TableType::Unknown);
    CHECK(hasDiag(l, "device.too_small"));
}

// --- file system signatures ---------------------------------------------------

TEST("signature detection for common file systems") {
    struct Case { const char* name; FsType expect; void (*write)(Image&); };
    const Case cases[] = {
        {"bitlocker", FsType::BitLocker, [](Image& i) { i.putStr(3, "-FVE-FS-", 8); }},
        {"exfat", FsType::ExFat, [](Image& i) { i.putStr(3, "EXFAT   ", 8); }},
        {"refs", FsType::ReFs, [](Image& i) { i.putStr(3, "ReFS\0\0\0\0", 8); }},
        {"luks", FsType::Luks, [](Image& i) { i.putStr(0, "LUKS\xBA\xBE", 6); }},
        {"xfs", FsType::Xfs, [](Image& i) { i.putStr(0, "XFSB", 4); i.putStr(108, "xfsdata", 7); }},
        {"btrfs", FsType::Btrfs, [](Image& i) { i.putStr(0x10040, "_BHRfS_M", 8); }},
        {"lvm2", FsType::Lvm2Pv, [](Image& i) { i.putStr(512, "LABELONE", 8); i.putStr(512 + 24, "LVM2 001", 8); }},
        {"apfs", FsType::Apfs, [](Image& i) { i.putStr(32, "NXSB", 4); }},
        {"f2fs", FsType::F2fs, [](Image& i) { i.put32(1024, 0xF2F52010u); }},
        {"jfs", FsType::Jfs, [](Image& i) { i.putStr(32768, "JFS1", 4); }},
        {"reiserfs", FsType::ReiserFs, [](Image& i) { i.putStr(65536 + 52, "ReIsEr2Fs", 9); }},
        {"reiser4", FsType::Reiser4, [](Image& i) { i.putStr(65536, "ReIsEr4", 7); }},
        {"hfs+", FsType::HfsPlus, [](Image& i) { i.putStr(1024, "H+", 2); }},
        {"nilfs2", FsType::Nilfs2, [](Image& i) { i.put16(1024 + 6, 0x3434); }},
        {"md-raid", FsType::LinuxRaid, [](Image& i) { i.put32(4096, 0xA92B4EFCu); }},
        {"udf", FsType::Udf, [](Image& i) { i.putStr(32768 + 2048 + 1, "NSR02", 5); }},
        {"swap", FsType::LinuxSwap, [](Image& i) { wptest::writeSwap(i, 0); }},
        {"ext2", FsType::Ext2, [](Image& i) { i.put16(1024 + 0x38, 0xEF53); }},
        {"ext3", FsType::Ext3, [](Image& i) { i.put16(1024 + 0x38, 0xEF53); i.put32(1024 + 0x5C, 4); }},
        {"ext4", FsType::Ext4, [](Image& i) { wptest::writeExt4(i, 0, "root"); }},
        {"zfs", FsType::Zfs, [](Image& i) { i.put64(128 * 1024 + 2048, 0x00bab10cull); }},
    };
    for (const auto& c : cases) {
        Image im(1 * MiB, 512);
        c.write(im);
        MemoryBlockReader r(im.data, 512);
        FsDetection d = detectFilesystem(r);
        if (d.type != c.expect) std::printf("    case %s detected as %s\n", c.name, fsTypeName(d.type));
        CHECK(d.type == c.expect);
    }
}

TEST("signature detection reads labels") {
    Image im(1 * MiB, 512);
    im.putStr(0, "XFSB", 4);
    im.putStr(108, "xfsdata", 7);
    MemoryBlockReader r(im.data, 512);
    CHECK_EQ(detectFilesystem(r).label, std::string("xfsdata"));
}

TEST("zeroed partition is unknown") {
    MemoryBlockReader r(std::vector<std::uint8_t>(1 * MiB, 0), 512);
    CHECK(detectFilesystem(r).type == FsType::Unknown);
}

TEST("detection on a partition smaller than the probe window") {
    Image im(4096, 512);
    im.putStr(3, "NTFS    ", 8);
    MemoryBlockReader r(im.data, 512);
    CHECK(detectFilesystem(r).type == FsType::Ntfs);
}

TEST("capability table claims only what is implemented") {
    // No fake features: an action counts as available only if the code behind
    // it exists (fs_tools for file systems, raw block moves/copies in the executor).
    auto available = [](Support s) { return s == Support::Supported || s == Support::Limited; };
    for (FsType t : allFsTypes()) {
        const auto& c = capabilitiesFor(t);
        CHECK(c.type == t);
        CHECK(available(c.create) == fstools::canCreate(t));
        CHECK(available(c.grow) == fstools::canResize(t));
        CHECK(available(c.shrink) == fstools::canResize(t));
        CHECK(available(c.check) == fstools::canCheck(t));
        CHECK(available(c.label) == fstools::canLabel(t));
        CHECK(available(c.uuid) == fstools::canSetUuid(t));
        CHECK(!available(c.move) || c.move == Support::Limited);  // raw block move only
        CHECK(!available(c.copy) || c.copy == Support::Limited);
    }
    for (FsType t : {FsType::Luks, FsType::Lvm2Pv, FsType::LinuxRaid, FsType::Zfs, FsType::BitLocker})
        CHECK(!available(capabilitiesFor(t).copy));  // duplicate identities
}

// --- layout view --------------------------------------------------------------

TEST("segments include gaps >= 1 MiB and nest logicals") {
    Image im(128 * MiB, 512);
    im.mbrEntry(0, 0, 0x07, 2048, 20480);
    im.mbrEntry(0, 1, 0x0F, 40960, 100000);
    im.mbrEntry(40960, 0, 0x83, 2048, 18432);
    DiskLayout l = parse(im, false);
    auto segs = buildSegments(l);
    // The 2047-sector gap after the MBR is below 1 MiB and hidden (as in GParted):
    // ntfs, gap, extended{gap, logical, gap}, trailing gap
    REQUIRE(segs.size() == 4);
    CHECK(segs[0].kind == Segment::Kind::Partition);
    CHECK(segs[1].kind == Segment::Kind::Unallocated);
    CHECK(segs[2].partitionIndex == 1);
    REQUIRE(segs[2].children.size() == 3);
    CHECK(segs[2].children[0].kind == Segment::Kind::Unallocated);
    CHECK(segs[2].children[1].kind == Segment::Kind::Partition);
    CHECK(segs[2].children[2].kind == Segment::Kind::Unallocated);
    CHECK(segs[3].kind == Segment::Kind::Unallocated);
    std::uint64_t sum = 0;
    for (const auto& s : segs) sum += s.sectorCount;
    CHECK_EQ(sum, l.totalSectors() - 2048);
}

TEST("gpt segments are limited to the usable area") {
    Image im(64 * MiB, 512);
    im.gpt({{kLinux, 2048, 4095, "a", 0}});
    DiskLayout l = parse(im, false);
    auto segs = buildSegments(l);
    // 34..2047 is smaller than 1 MiB and therefore hidden.
    REQUIRE(segs.size() == 2);
    CHECK(segs[0].kind == Segment::Kind::Partition);
    CHECK_EQ(segs[1].firstLba, 4096u);
    CHECK_EQ(segs[1].firstLba + segs[1].sectorCount - 1, l.lastUsableLba);
}

// --- JSON ---------------------------------------------------------------------

TEST("json output escapes strings and contains partitions") {
    CHECK_EQ(JsonWriter::escape("a\"b\\c\n\x01"), std::string("\"a\\\"b\\\\c\\n\\u0001\""));
    Image im(64 * MiB, 512);
    im.gpt({{kBasic, 2048, 4095, "Data \"quoted\"", 0}});
    std::string j = layoutToJson(parse(im));
    CHECK(j.find("\"table\":\"gpt\"") != std::string::npos);
    CHECK(j.find("Data \\\"quoted\\\"") != std::string::npos);
    CHECK(j.front() == '{' && j.back() == '}');
}

// --- robustness ---------------------------------------------------------------

TEST("fuzz: random and mutated images never crash the parser") {
    std::mt19937 rng(20261027);
    Image base(8 * MiB, 512);
    base.gpt({{kEsp, 2048, 4095, "esp", 0}, {kBasic, 4096, 8191, "data", 0}});
    base.mbrEntry(0, 1, 0x05, 9000, 4000);  // nonsense extended entry next to protective entry
    for (int iter = 0; iter < 400; ++iter) {
        Image im = base;
        int mutations = 1 + static_cast<int>(rng() % 64);
        for (int m = 0; m < mutations; ++m) {
            std::size_t off = rng() % (iter % 2 ? 64 * 512 : im.data.size());
            im.data[off] = static_cast<std::uint8_t>(rng());
        }
        DiskLayout l = parse(im);
        (void)buildSegments(l);
        (void)layoutToJson(l);
    }
    for (int iter = 0; iter < 100; ++iter) {
        std::vector<std::uint8_t> junk(64 * 1024);
        for (auto& b : junk) b = static_cast<std::uint8_t>(rng());
        junk[510] = 0x55;
        junk[511] = 0xAA;
        MemoryBlockReader r(junk, 512);
        DiskLayout l = parseDiskLayout(r);
        (void)buildSegments(l);
    }
    CHECK(true);
}

// --- file reader (integration with a real image file on disk) -----------------

TEST("FileBlockReader parses an image file written to the temp directory") {
    Image im(16 * MiB, 512);
    im.gpt({{kLinux, 2048, 8191, "img", 0}});
    writeExt4(im, 2048ull * 512, "imgroot");
    char path[512];
    std::snprintf(path, sizeof path, "wparted_test_image_%u.img", static_cast<unsigned>(std::random_device{}() & 0xFFFF));
    {
        std::FILE* f = nullptr;
#ifdef _WIN32
        fopen_s(&f, path, "wb");
#else
        f = std::fopen(path, "wb");
#endif
        REQUIRE(f != nullptr);
        std::fwrite(im.data.data(), 1, im.data.size(), f);
        std::fclose(f);
    }
    {
        FileBlockReader r;
        REQUIRE(r.open(path, 512));
        CHECK_EQ(r.size(), im.data.size());
        DiskLayout l = parseDiskLayout(r);
        CHECK(l.type == TableType::Gpt);
        REQUIRE(l.partitions.size() == 1);
        CHECK_EQ(l.partitions[0].fs.label, std::string("imgroot"));
    }
    std::remove(path);
}

int main() {
    int failedTests = 0;
    for (auto& t : wptest::registry()) {
        int before = wptest::failures();
        try {
            t.fn();
        } catch (const wptest::AbortTest&) {
        } catch (const std::exception& e) {
            wptest::fail(__FILE__, __LINE__, std::string("exception: ") + e.what());
        }
        bool ok = wptest::failures() == before;
        if (!ok) ++failedTests;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", t.name);
    }
    std::printf("\n%zu tests, %d failed\n", wptest::registry().size(), failedTests);
    return failedTests == 0 ? 0 : 1;
}
