// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#include "wparted/win/disk_writer.hpp"

#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <winioctl.h>

#include <algorithm>
#include <cstring>

#include "wparted/core/helper_protocol.hpp"

namespace wparted::win {
namespace {

constexpr std::size_t kBounce = 1024 * 1024;

std::string lastError(const char* what) { return std::string(what) + " failed (" + std::to_string(GetLastError()) + ")"; }

std::wstring withoutTrailingSlash(std::wstring s) {
    if (!s.empty() && s.back() == L'\\') s.pop_back();
    return s;
}

std::wstring programDataFolder() {
    wchar_t* path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &path))) out = path;
    CoTaskMemFree(path);
    return out;
}

// Administrators own the folder; administrators and SYSTEM write, signed-in users read.
bool secureFolder(const std::wstring& path, std::string& error) {
    if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        error = lastError("creating the journal folder");
        return false;
    }
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY) || (attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
        error = "the journal folder is not a plain folder";
        return false;
    }
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAD:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)(A;OICI;GR;;;AU)", SDDL_REVISION_1,
                                                              &sd, nullptr)) {
        error = lastError("building the journal folder permissions");
        return false;
    }
    PSID owner = nullptr;
    PACL dacl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    GetSecurityDescriptorOwner(sd, &owner, &defaulted);
    GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
    const DWORD rc = SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
                                           OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                           owner, nullptr, dacl, nullptr);
    LocalFree(sd);
    if (rc != ERROR_SUCCESS) {
        error = "setting the journal folder permissions failed (" + std::to_string(rc) + ")";
        return false;
    }
    return true;
}

}  // namespace

DiskWriteFacts writeFactsFor(const PhysicalDisk& d, bool elevated) {
    DiskWriteFacts f;
    f.elevated = elevated;
    f.virtualDisk = d.busType == "File-backed virtual";
#ifdef WPARTED_ENABLE_REAL_DISK_WRITES
    f.realDiskWritesEnabled = true;
#endif
    f.holdsRunningSystem = d.containsWindows();
    for (const auto& v : d.volumes)
        if (v.containsWindows || v.isPageFileVolume || v.isSystemPartition) f.holdsRunningSystem = true;
    f.writeProtected = d.writeProtected == Tristate::Yes;
    f.layoutReadable = d.layoutAvailable;
    return f;
}

ValidationContext contextForDisk(const PhysicalDisk& d, const DiskLayout& layout, ValidationMode mode) {
    ValidationContext ctx;
    ctx.mode = mode;
    ctx.physicalSectorSize = d.physicalSectorSize;
    ctx.deviceWriteProtected = d.writeProtected == Tristate::Yes;
    for (const auto& v : d.volumes)
        if (v.extentCount > 1) ctx.managedByWindows = true;
    if (isManagedByWindows(layout)) ctx.managedByWindows = true;
    for (const auto& p : layout.partitions) {
        PartitionState st;
        if (const auto* vol = volumeForPartition(d, p)) {
            st.mounted = !vol->mountPoints.empty();
            st.containsWindows = vol->containsWindows;
            st.pageFile = vol->isPageFileVolume;
            st.bootManager = vol->isSystemPartition;
            st.bitLocker = vol->bitLocker;
            if (vol->hasUsage) st.usedBytes = vol->totalBytes - vol->freeBytes;
        }
        ctx.state[p.planId] = st;
    }
    if (mode != ValidationMode::Planning) {
        ctx.haveCurrentIdentity = true;
        ctx.currentIdentity = identityOf(layout, d.serial, d.modelName());
    }
    return ctx;
}

std::string journalPathForDisk(const PhysicalDisk& d) {
    const std::string key = helper::sha256Hex(d.serial + "|" + d.modelName() + "|" + std::to_string(d.sizeBytes)).substr(0, 24);
    return narrow(programDataFolder()) + "\\WParted\\journal\\disk-" + key + ".journal";
}

bool prepareJournalFolder(std::string& error) {
    const std::wstring root = programDataFolder();
    if (root.empty()) {
        error = "the ProgramData folder was not found";
        return false;
    }
    return secureFolder(root + L"\\WParted", error) && secureFolder(root + L"\\WParted\\journal", error);
}

PhysicalDiskDevice::~PhysicalDiskDevice() {
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    if (bounce_) VirtualFree(bounce_, 0, MEM_RELEASE);
}

bool PhysicalDiskDevice::open(const PhysicalDisk& d, std::string& error) {
    handle_ = CreateFileW(d.path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,  // readonly-guard: allow (the disk writer)
                          FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        error = lastError("opening the disk for writing");
        return false;
    }
    DWORD got = 0;
    if (!DeviceIoControl(handle_, IOCTL_DISK_IS_WRITABLE, nullptr, 0, nullptr, 0, &got, nullptr)) {
        error = "Windows reports the disk as not writable (offline or read-only)";
        return false;
    }
    DISK_GEOMETRY_EX g{};
    if (!DeviceIoControl(handle_, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &g, sizeof g, &got, nullptr)) {
        error = lastError("reading the disk geometry");
        return false;
    }
    size_ = static_cast<std::uint64_t>(g.DiskSize.QuadPart);
    sectorSize_ = g.Geometry.BytesPerSector;
    if (size_ != d.sizeBytes || sectorSize_ != d.logicalSectorSize || !sectorSize_ || sectorSize_ > kBounce) {
        error = "the disk changed since it was examined";
        return false;
    }
    bounce_ = VirtualAlloc(nullptr, kBounce, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!bounce_) {
        error = "out of memory";
        return false;
    }
    return true;
}

bool PhysicalDiskDevice::transfer(std::uint64_t offset, void* readTo, const void* writeFrom, std::size_t len) {
    if (offset > size_ || len > size_ - offset) return false;
    auto* in = static_cast<const std::uint8_t*>(writeFrom);
    auto* out = static_cast<std::uint8_t*>(readTo);
    auto* buf = static_cast<std::uint8_t*>(bounce_);
    while (len > 0) {
        const std::uint64_t start = offset / sectorSize_ * sectorSize_;
        const std::size_t skip = static_cast<std::size_t>(offset - start);
        const std::size_t want = std::min(len, kBounce - skip);
        const std::size_t span = (skip + want + sectorSize_ - 1) / sectorSize_ * sectorSize_;
        if (span > kBounce && want < sectorSize_) return false;
        const std::size_t count = span > kBounce ? kBounce - skip : want;
        const std::size_t ioSpan = (skip + count + sectorSize_ - 1) / sectorSize_ * sectorSize_;
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(start & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(start >> 32);
        DWORD done = 0;
        const bool partial = skip != 0 || count % sectorSize_ != 0;
        if (out || partial) {  // a partial write keeps the rest of its sectors
            if (!ReadFile(handle_, buf, static_cast<DWORD>(ioSpan), &done, &ov) || done != ioSpan) return false;
        }
        if (out) {
            std::memcpy(out, buf + skip, count);
            out += count;
        } else {
            std::memcpy(buf + skip, in, count);
            in += count;
            OVERLAPPED wv = OVERLAPPED{};
            wv.Offset = ov.Offset;
            wv.OffsetHigh = ov.OffsetHigh;
            if (!WriteFile(handle_, buf, static_cast<DWORD>(ioSpan), &done, &wv) || done != ioSpan) return false;  // readonly-guard: allow (the disk writer)
        }
        offset += count;
        len -= count;
    }
    return true;
}

bool PhysicalDiskDevice::read(std::uint64_t offset, void* buffer, std::size_t len) { return transfer(offset, buffer, nullptr, len); }

bool PhysicalDiskDevice::write(std::uint64_t offset, const void* buffer, std::size_t len) { return transfer(offset, nullptr, buffer, len); }

bool PhysicalDiskDevice::flush() { return FlushFileBuffers(handle_) != 0; }

bool PhysicalDiskDevice::updateProperties(std::string& error) {
    DWORD got = 0;
    if (DeviceIoControl(handle_, IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0, &got, nullptr)) return true;  // readonly-guard: allow (the disk writer)
    error = lastError("refreshing the partition table in Windows");
    return false;
}

VolumeLocks::~VolumeLocks() { release(); }

void VolumeLocks::release() {
    for (HANDLE h : handles_) CloseHandle(h);  // closing the handle ends the lock
    handles_.clear();
}

bool VolumeLocks::lockAll(const PhysicalDisk& d, std::string& error) {
    for (const auto& v : d.volumes) {
        const std::wstring path = withoutTrailingSlash(v.guidPath);
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);  // readonly-guard: allow (the disk writer)
        if (h == INVALID_HANDLE_VALUE) {
            error = lastError("opening a volume of this disk");
            release();
            return false;
        }
        handles_.push_back(h);
        DWORD got = 0;
        FlushFileBuffers(h);
        if (!DeviceIoControl(h, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr)) {  // readonly-guard: allow (the disk writer)
            error = "a volume of this disk is in use and cannot be locked (" + narrow(v.label) + "); close the programs using it";
            release();
            return false;
        }
        if (!DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr)) {  // readonly-guard: allow (the disk writer)
            error = lastError("dismounting a volume of this disk");
            release();
            return false;
        }
    }
    return true;
}

bool addMountPoint(const std::string& volume, const std::string& mountPoint, std::string& error) {
    if (SetVolumeMountPointW(widen(mountPoint).c_str(), widen(volume).c_str())) return true;  // readonly-guard: allow (the disk writer)
    error = lastError("assigning the drive letter");
    return false;
}

bool removeMountPoints(const VolumeInfo& v, std::string& error) {
    const std::wstring path = withoutTrailingSlash(v.guidPath);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);  // readonly-guard: allow (the disk writer)
    DWORD got = 0;
    if (h == INVALID_HANDLE_VALUE || !DeviceIoControl(h, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr)) {  // readonly-guard: allow (the disk writer)
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        error = "the volume is in use and cannot be unmounted; close the programs using it";
        return false;
    }
    FlushFileBuffers(h);
    bool ok = true;
    for (const auto& m : v.mountPoints)
        if (!DeleteVolumeMountPointW(m.c_str())) {  // readonly-guard: allow (the disk writer)
            error = lastError("removing the mount point");
            ok = false;
        }
    if (ok) DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr);  // readonly-guard: allow (the disk writer)
    CloseHandle(h);
    return ok;
}

}  // namespace wparted::win
