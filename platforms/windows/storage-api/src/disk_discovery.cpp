// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Windows disk and volume discovery. SAFETY: this translation unit must never
// request write access or issue modifying IOCTLs. tools/ci/check_readonly.py
// enforces this in CI.
#include "wparted/win/disk_discovery.hpp"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <memory>

#include "wparted/core/log.hpp"

namespace wparted::win {

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

namespace {

struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE x) : h(x) {}
    ~Handle() { if (valid()) CloseHandle(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    bool valid() const { return h != INVALID_HANDLE_VALUE && h != nullptr; }
};

std::string lastErrorText(DWORD err) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   err, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring w = buf ? buf : L"";
    if (buf) LocalFree(buf);
    while (!w.empty() && (w.back() == L'\n' || w.back() == L'\r' || w.back() == L' ' || w.back() == L'.')) w.pop_back();
    return narrow(w) + " (" + std::to_string(err) + ")";
}

// Opens a device for metadata queries only (no data access).
HANDLE openQueryOnly(const std::wstring& path) {
    return CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
}

// Opens a device for reading. Requires administrative rights for physical drives.
HANDLE openReadOnly(const std::wstring& path) {
    return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
}

const char* busTypeName(int t) {
    static const char* names[] = {"Unknown", "SCSI", "ATAPI", "ATA", "IEEE 1394", "SSA", "Fibre Channel",
                                  "USB", "RAID", "iSCSI", "SAS", "SATA", "SD", "MMC", "Virtual",
                                  "File-backed virtual", "Storage Spaces", "NVMe", "SCM", "UFS"};
    if (t >= 0 && t < static_cast<int>(sizeof names / sizeof names[0])) return names[t];
    return "Other";
}

std::string descriptorString(const std::vector<BYTE>& buf, DWORD offset) {
    if (offset == 0 || offset >= buf.size()) return {};
    std::string s;
    for (DWORD i = offset; i < buf.size() && buf[i]; ++i) s += static_cast<char>(buf[i]);
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

void queryDeviceDescriptor(HANDLE h, PhysicalDisk& d) {
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    STORAGE_DESCRIPTOR_HEADER hdr{};
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &hdr, sizeof hdr, &got, nullptr) || hdr.Size == 0)
        return;
    std::vector<BYTE> buf(hdr.Size);
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, buf.data(), static_cast<DWORD>(buf.size()), &got,
                         nullptr))
        return;
    buf.resize(got);
    if (buf.size() < sizeof(STORAGE_DEVICE_DESCRIPTOR)) return;
    const auto* dd = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buf.data());
    d.vendor = descriptorString(buf, dd->VendorIdOffset);
    d.product = descriptorString(buf, dd->ProductIdOffset);
    d.revision = descriptorString(buf, dd->ProductRevisionOffset);
    d.serial = descriptorString(buf, dd->SerialNumberOffset);
    d.busType = busTypeName(static_cast<int>(dd->BusType));
    d.removableMedia = dd->RemovableMedia != FALSE;
}

void queryAlignment(HANDLE h, PhysicalDisk& d) {
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageAccessAlignmentProperty;
    q.QueryType = PropertyStandardQuery;
    STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR a{};
    DWORD got = 0;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &a, sizeof a, &got, nullptr) &&
        got >= sizeof a) {
        if (a.BytesPerLogicalSector) d.logicalSectorSize = a.BytesPerLogicalSector;
        d.physicalSectorSize = a.BytesPerPhysicalSector;
    }
}

void querySeekPenalty(HANDLE h, PhysicalDisk& d) {
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceSeekPenaltyProperty;
    q.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR s{};
    DWORD got = 0;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &s, sizeof s, &got, nullptr) && got >= sizeof s)
        d.solidState = s.IncursSeekPenalty ? Tristate::No : Tristate::Yes;
}

bool queryGeometry(HANDLE h, PhysicalDisk& d) {
    DISK_GEOMETRY_EX g{};
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &g, sizeof g, &got, nullptr)) return false;
    d.sizeBytes = static_cast<std::uint64_t>(g.DiskSize.QuadPart);
    if (g.Geometry.BytesPerSector) d.logicalSectorSize = g.Geometry.BytesPerSector;
    return true;
}

// BlockReader over a raw device handle. Windows requires sector-aligned
// offsets/lengths and suitably aligned buffers for raw disk I/O, so reads go
// through a page-aligned bounce buffer.
class DeviceReader final : public BlockReader {
public:
    DeviceReader(HANDLE h, std::uint64_t size, std::uint32_t sectorSize) : h_(h), size_(size), ss_(sectorSize) {}
    std::uint64_t size() const override { return size_; }
    std::uint32_t sectorSize() const override { return ss_; }

    bool read(std::uint64_t offset, void* buffer, std::size_t len) override {
        if (offset > size_ || len > size_ - offset) return false;
        auto* out = static_cast<std::uint8_t*>(buffer);
        while (len > 0) {
            const std::uint64_t alignedStart = offset / ss_ * ss_;
            const std::size_t skip = static_cast<std::size_t>(offset - alignedStart);
            const std::size_t want = std::min<std::size_t>(len, kChunk - ss_);
            std::size_t span = (skip + want + ss_ - 1) / ss_ * ss_;
            span = static_cast<std::size_t>(std::min<std::uint64_t>(span, size_ - alignedStart));
            void* bounce = VirtualAlloc(nullptr, span, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!bounce) return false;
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(alignedStart & 0xFFFFFFFFu);
            ov.OffsetHigh = static_cast<DWORD>(alignedStart >> 32);
            DWORD got = 0;
            BOOL ok = ReadFile(h_, bounce, static_cast<DWORD>(span), &got, &ov);
            const std::size_t usable = got > skip ? std::min<std::size_t>(got - skip, want) : 0;
            if (ok && usable == want) std::memcpy(out, static_cast<std::uint8_t*>(bounce) + skip, want);
            VirtualFree(bounce, 0, MEM_RELEASE);
            if (!ok || usable != want) return false;
            out += want;
            offset += want;
            len -= want;
        }
        return true;
    }

private:
    static constexpr std::size_t kChunk = 1024 * 1024;
    HANDLE h_;
    std::uint64_t size_;
    std::uint32_t ss_;
};

// Compares WParted's own parse with what the Windows partition manager
// reports. A mismatch usually means a pending/unflushed change or a parser
// limitation; either way it must be visible to the user.
void compareWithOsLayout(HANDLE h, PhysicalDisk& d) {
    std::vector<BYTE> buf(sizeof(DRIVE_LAYOUT_INFORMATION_EX) + 255 * sizeof(PARTITION_INFORMATION_EX));
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0, buf.data(), static_cast<DWORD>(buf.size()), &got,
                         nullptr))
        return;
    const auto* dl = reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(buf.data());
    std::vector<std::uint64_t> osStarts, ours;
    for (DWORD i = 0; i < dl->PartitionCount; ++i) {
        const auto& pi = dl->PartitionEntry[i];
        if (pi.PartitionLength.QuadPart == 0) continue;
        if (pi.PartitionStyle == PARTITION_STYLE_MBR && pi.Mbr.PartitionType == 0) continue;
        osStarts.push_back(static_cast<std::uint64_t>(pi.StartingOffset.QuadPart));
    }
    for (const auto& p : d.layout.partitions) ours.push_back(p.firstLba * d.layout.sectorSize);
    std::sort(osStarts.begin(), osStarts.end());
    std::sort(ours.begin(), ours.end());
    d.osLayoutCompared = true;
    // Windows reports extended containers inconsistently across versions, so
    // only require that every partition Windows knows is also known to us.
    bool ok = std::includes(ours.begin(), ours.end(), osStarts.begin(), osStarts.end());
    if (!ok) {
        d.layout.diagnostics.push_back(
            {Severity::Warning, "os.layout_mismatch",
             "Windows reports " + std::to_string(osStarts.size()) + " partition(s) that differ from the on-disk table read by "
             "WParted. Refresh after pending changes, and do not plan operations until this is resolved."});
    }
}

std::vector<VolumeInfo> enumerateVolumes() {
    std::vector<VolumeInfo> result;
    wchar_t windowsVolume[MAX_PATH] = L"";
    {
        wchar_t winDir[MAX_PATH], mount[MAX_PATH];
        if (GetSystemWindowsDirectoryW(winDir, MAX_PATH) && GetVolumePathNameW(winDir, mount, MAX_PATH))
            GetVolumeNameForVolumeMountPointW(mount, windowsVolume, MAX_PATH);
    }
    wchar_t name[MAX_PATH];
    HANDLE find = FindFirstVolumeW(name, MAX_PATH);
    if (find == INVALID_HANDLE_VALUE) return result;
    do {
        VolumeInfo v;
        v.guidPath = name;
        v.containsWindows = _wcsicmp(name, windowsVolume) == 0;

        std::wstring devPath = v.guidPath;
        if (!devPath.empty() && devPath.back() == L'\\') devPath.pop_back();
        {
            Handle h(openQueryOnly(devPath));
            if (h.valid()) {
                std::vector<BYTE> buf(sizeof(VOLUME_DISK_EXTENTS) + 31 * sizeof(DISK_EXTENT));
                DWORD got = 0;
                if (DeviceIoControl(h.h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, buf.data(),
                                    static_cast<DWORD>(buf.size()), &got, nullptr)) {
                    const auto* ext = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buf.data());
                    v.extentCount = static_cast<int>(ext->NumberOfDiskExtents);
                    if (ext->NumberOfDiskExtents > 0) {
                        v.diskNumber = ext->Extents[0].DiskNumber;
                        v.startingOffset = static_cast<std::uint64_t>(ext->Extents[0].StartingOffset.QuadPart);
                        v.extentLength = static_cast<std::uint64_t>(ext->Extents[0].ExtentLength.QuadPart);
                    }
                }
            }
        }
        // Avoid "insert disk" dialogs for empty card readers.
        UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
        wchar_t label[MAX_PATH + 1] = L"", fs[MAX_PATH + 1] = L"";
        if (GetVolumeInformationW(v.guidPath.c_str(), label, MAX_PATH, nullptr, nullptr, nullptr, fs, MAX_PATH)) {
            v.label = label;
            v.fileSystem = fs;
            ULARGE_INTEGER freeToCaller, total, totalFree;
            if (GetDiskFreeSpaceExW(v.guidPath.c_str(), &freeToCaller, &total, &totalFree)) {
                v.hasUsage = true;
                v.totalBytes = total.QuadPart;
                v.freeBytes = totalFree.QuadPart;
            }
        }
        SetErrorMode(oldMode);
        DWORD needed = 0;
        GetVolumePathNamesForVolumeNameW(v.guidPath.c_str(), nullptr, 0, &needed);
        if (needed) {
            std::vector<wchar_t> paths(needed);
            if (GetVolumePathNamesForVolumeNameW(v.guidPath.c_str(), paths.data(), needed, &needed))
                for (const wchar_t* p = paths.data(); *p; p += wcslen(p) + 1) v.mountPoints.emplace_back(p);
        }
        if (v.extentCount > 0) result.push_back(std::move(v));
    } while (FindNextVolumeW(find, name, MAX_PATH));
    FindVolumeClose(find);
    return result;
}

}  // namespace

bool isProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD got = 0;
    BOOL ok = GetTokenInformation(token, TokenElevation, &e, sizeof e, &got);
    CloseHandle(token);
    return ok && e.TokenIsElevated;
}

std::string PhysicalDisk::displayName() const { return "Disk " + std::to_string(number); }

std::string PhysicalDisk::modelName() const {
    std::string m = vendor;
    if (!product.empty()) m += (m.empty() ? "" : " ") + product;
    return m.empty() ? "Unknown device" : m;
}

bool PhysicalDisk::containsWindows() const {
    for (const auto& v : volumes)
        if (v.containsWindows) return true;
    return false;
}

const VolumeInfo* volumeForPartition(const PhysicalDisk& disk, const PartitionEntry& p) {
    const std::uint64_t start = p.firstLba * disk.logicalSectorSize;
    for (const auto& v : disk.volumes)
        if (v.startingOffset == start) return &v;
    return nullptr;
}

DiscoveryResult discoverDisks() {
    const auto t0 = std::chrono::steady_clock::now();
    DiscoveryResult r;
    r.elevated = isProcessElevated();
    log::info(std::string("Discovery started (elevated: ") + (r.elevated ? "yes" : "no") + ")");

    std::vector<VolumeInfo> volumes = enumerateVolumes();

    // PhysicalDrive numbers are not guaranteed to be contiguous (removed USB
    // devices leave gaps), so probe a fixed range.
    for (std::uint32_t n = 0; n < 64; ++n) {
        std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(n);
        Handle q(openQueryOnly(path));
        if (!q.valid()) continue;
        PhysicalDisk d;
        d.number = n;
        d.path = path;
        queryDeviceDescriptor(q.h, d);
        queryAlignment(q.h, d);
        querySeekPenalty(q.h, d);
        if (!queryGeometry(q.h, d)) {
            log::warn("Disk " + std::to_string(n) + ": no geometry (no media?)");
            continue;
        }
        for (const auto& v : volumes)
            if (v.diskNumber == n) d.volumes.push_back(v);

        Handle rd(openReadOnly(path));
        if (rd.valid()) {
            d.rawReadable = true;
            DWORD got = 0;
            SetLastError(0);
            if (DeviceIoControl(rd.h, IOCTL_DISK_IS_WRITABLE, nullptr, 0, nullptr, 0, &got, nullptr))
                d.writeProtected = Tristate::No;
            else if (GetLastError() == ERROR_WRITE_PROTECT)
                d.writeProtected = Tristate::Yes;
            DeviceReader reader(rd.h, d.sizeBytes, d.logicalSectorSize);
            d.layout = parseDiskLayout(reader, true);
            d.layoutAvailable = true;
            compareWithOsLayout(rd.h, d);
        } else {
            d.accessError = lastErrorText(GetLastError());
            log::warn("Disk " + std::to_string(n) + ": raw read access denied: " + d.accessError);
        }
        log::info("Disk " + std::to_string(n) + ": " + d.modelName() + ", " + formatSize(d.sizeBytes) + ", " + d.busType +
                  ", table " + (d.layoutAvailable ? tableTypeName(d.layout.type) : "n/a") + ", " +
                  std::to_string(d.layout.partitions.size()) + " partition(s)");
        r.disks.push_back(std::move(d));
    }
    r.durationMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    log::info("Discovery finished: " + std::to_string(r.disks.size()) + " disk(s) in " +
              std::to_string(static_cast<int>(r.durationMs)) + " ms");
    return r;
}

}  // namespace wparted::win
