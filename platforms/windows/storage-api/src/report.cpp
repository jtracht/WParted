// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#include "wparted/win/report.hpp"

#include <algorithm>
#include <cwchar>

#include "wparted/core/json.hpp"

namespace wparted::win {

const char* tristateName(Tristate t) noexcept {
    return t == Tristate::Yes ? "yes" : t == Tristate::No ? "no" : "unknown";
}

FsType fsTypeFromWindowsName(const std::wstring& n) {
    if (_wcsicmp(n.c_str(), L"NTFS") == 0) return FsType::Ntfs;
    if (_wcsicmp(n.c_str(), L"FAT32") == 0) return FsType::Fat32;
    if (_wcsicmp(n.c_str(), L"FAT") == 0) return FsType::Fat16;
    if (_wcsicmp(n.c_str(), L"FAT12") == 0) return FsType::Fat12;
    if (_wcsicmp(n.c_str(), L"exFAT") == 0) return FsType::ExFat;
    if (_wcsicmp(n.c_str(), L"ReFS") == 0) return FsType::ReFs;
    if (_wcsicmp(n.c_str(), L"UDF") == 0) return FsType::Udf;
    return FsType::Unknown;
}

DiskLayout layoutFromVolumes(const PhysicalDisk& d) {
    DiskLayout l;
    l.type = TableType::Unknown;
    l.sectorSize = d.logicalSectorSize ? d.logicalSectorSize : 512;
    l.diskBytes = d.sizeBytes;
    l.firstUsableLba = 0;
    l.lastUsableLba = l.totalSectors() ? l.totalSectors() - 1 : 0;
    std::vector<const VolumeInfo*> vols;
    for (const auto& v : d.volumes)
        if (v.extentCount == 1) vols.push_back(&v);
    std::sort(vols.begin(), vols.end(), [](auto a, auto b) { return a->startingOffset < b->startingOffset; });
    for (const auto* v : vols) {
        PartitionEntry p;
        p.number = 0;  // Real partition numbers are unknown without the table.
        p.kind = PartitionKind::Primary;
        p.firstLba = v->startingOffset / l.sectorSize;
        p.sectorCount = v->extentLength / l.sectorSize;
        p.fs.type = fsTypeFromWindowsName(v->fileSystem);
        p.fs.label = narrow(v->label);
        l.partitions.push_back(p);
    }
    l.diagnostics.push_back({Severity::Info, "layout.volumes_only",
                             "The partition table could not be read without administrator rights. Only volumes that "
                             "Windows has mounted are shown; other partitions and free space may be missing."});
    return l;
}

std::string diskToJson(const PhysicalDisk& d) {
    JsonWriter w;
    w.beginObject();
    w.key("number").value(d.number);
    w.key("path").value(narrow(d.path));
    w.key("vendor").value(d.vendor);
    w.key("product").value(d.product);
    w.key("revision").value(d.revision);
    w.key("serial").value(d.serial);
    w.key("bus").value(d.busType);
    w.key("removableMedia").value(d.removableMedia);
    w.key("solidState").value(tristateName(d.solidState));
    w.key("writeProtected").value(tristateName(d.writeProtected));
    w.key("sizeBytes").value(d.sizeBytes);
    w.key("logicalSectorSize").value(d.logicalSectorSize);
    w.key("physicalSectorSize").value(d.physicalSectorSize);
    w.key("rawReadable").value(d.rawReadable);
    if (!d.accessError.empty()) w.key("accessError").value(d.accessError);
    w.key("layout");
    if (d.layoutAvailable) w.raw(layoutToJson(d.layout)); else w.null();
    w.key("volumes").beginArray();
    for (const auto& v : d.volumes) {
        w.beginObject();
        w.key("guidPath").value(narrow(v.guidPath));
        w.key("mountPoints").beginArray();
        for (const auto& m : v.mountPoints) w.value(narrow(m));
        w.endArray();
        w.key("label").value(narrow(v.label));
        w.key("fileSystem").value(narrow(v.fileSystem));
        w.key("startingOffset").value(v.startingOffset);
        w.key("lengthBytes").value(v.extentLength);
        w.key("extentCount").value(v.extentCount);
        if (v.hasUsage) {
            w.key("totalBytes").value(v.totalBytes);
            w.key("freeBytes").value(v.freeBytes);
        }
        w.key("containsWindows").value(v.containsWindows);
        w.endObject();
    }
    w.endArray();
    w.endObject();
    return w.str();
}

std::string discoveryToJson(const DiscoveryResult& r, const char* version) {
    JsonWriter w;
    w.beginObject();
    w.key("tool").value("wparted");
    w.key("version").value(version);
    w.key("elevated").value(r.elevated);
    w.key("discoveryMs").value(static_cast<std::uint64_t>(r.durationMs));
    w.key("disks").beginArray();
    for (const auto& d : r.disks) w.raw(diskToJson(d));
    w.endArray();
    w.endObject();
    return w.str();
}

}  // namespace wparted::win
