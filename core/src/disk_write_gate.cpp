// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#include "wparted/core/disk_write_gate.hpp"

#include <cctype>

namespace wparted {

std::string diskWriteProblem(const DiskWriteFacts& f) {
    if (f.holdsRunningSystem) return "this disk holds the running Windows, its page file or its boot manager";
    if (f.writeProtected) return "the disk is write protected";
    if (!f.virtualDisk && !f.realDiskWritesEnabled)
        return "this build writes to attached virtual disks (VHD/VHDX) only; real disks stay locked until the security review is done";
    if (!f.layoutReadable) return "the partition table of this disk could not be read";
    if (!f.elevated) return "writing to a disk needs administrator rights";
    return {};
}

bool parsePhysicalDrivePath(const std::string& path, unsigned& number) {
    static const std::string prefix = "\\\\.\\PhysicalDrive";
    if (path.rfind(prefix, 0) != 0) return false;
    const std::string digits = path.substr(prefix.size());
    if (digits.empty() || digits.size() > 3) return false;
    unsigned n = 0;
    for (char c : digits) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        n = n * 10 + static_cast<unsigned>(c - '0');
    }
    number = n;
    return true;
}

bool isDriveLetterMountPoint(const std::string& m) {
    return m.size() == 3 && std::isalpha(static_cast<unsigned char>(m[0])) && m[1] == ':' && m[2] == '\\';
}

bool isVolumeGuidPath(const std::string& v) {
    static const std::string prefix = "\\\\?\\Volume{";
    if (v.rfind(prefix, 0) != 0 || v.size() != prefix.size() + 36 + 2 || v.compare(v.size() - 2, 2, "}\\") != 0) return false;
    for (std::size_t i = 0; i < 36; ++i) {
        const char c = v[prefix.size() + i];
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? c != '-' : !std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

}  // namespace wparted
