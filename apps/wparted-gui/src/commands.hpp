// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#pragma once

#include "wparted/core/filesystem.hpp"

namespace wparted::ui {

enum : int {
    IDC_COMBO = 100, IDC_MAP, IDC_LIST, IDC_PENDING, IDC_STATUS, IDC_STATUS_RIGHT, IDC_BANNER, IDC_BANNER_BTN,
    IDM_REFRESH = 1000, IDM_RESTART_ADMIN, IDM_QUIT, IDM_OPEN_IMAGE,
    IDM_UNDO, IDM_CLEAR_OPS, IDM_APPLY_OPS, IDM_SAVE_PLAN,
    IDM_DEVICE_INFO, IDM_PENDING_OPS, IDM_THEME_SYSTEM, IDM_THEME_LIGHT, IDM_THEME_DARK, IDM_FS_SUPPORT,
    IDM_CREATE_TABLE, IDM_EXPORT_REPORT, IDM_RESCUE,
    IDM_NEW, IDM_DELETE, IDM_RESIZE, IDM_COPY, IDM_PASTE, IDM_FORMAT, IDM_FLAGS, IDM_CHECK, IDM_LABEL, IDM_UUID,
    IDM_NAME, IDM_PART_INFO,
    IDM_OPEN_LOGS, IDM_HELP, IDM_ABOUT,
    IDM_DEVICE_BASE = 2000,
    IDM_FORMAT_BASE = 3000,
};

// File systems offered in "Format to" and "New" (GParted order, subset).
// FsType::Unknown stands for "cleared"/unformatted.
inline const FsType kFormatChoices[] = {
    FsType::Ntfs, FsType::Fat32, FsType::Fat16, FsType::ExFat, FsType::Ext4, FsType::Ext3, FsType::Ext2,
    FsType::Btrfs, FsType::Xfs, FsType::F2fs, FsType::LinuxSwap, FsType::Unknown,
};
constexpr int kFormatChoiceCount = static_cast<int>(sizeof kFormatChoices / sizeof kFormatChoices[0]);

}  // namespace wparted::ui
