// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#pragma once

#include <windows.h>

#include <commctrl.h>

#include <memory>
#include <thread>
#include <vector>

#include "model.hpp"
#include "theme.hpp"
#include "wparted/win/disk_discovery.hpp"

namespace wparted::ui {

class MainWindow {
public:
    bool create(HINSTANCE inst, int show);
    HWND hwnd() const { return hwnd_; }
    HACCEL accelerators() const { return accel_; }

private:
    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    void createChildren();
    void buildMenu();
    void rebuildDeviceMenu();
    void layout();
    void applyTheme();
    void recreateFonts();
    void startDiscovery();
    void onDiscovered(win::DiscoveryResult* result);
    void selectDisk(int index);
    void fillPartitionList();
    void selectRow(int row, bool fromMap);
    void updateStatus();
    void onCommand(int id);
    void showPartitionInfo();
    void showDeviceInfo();
    void showFsSupport();
    void showAbout();
    void exportReport();
    void restartElevated();
    void openLogFolder();
    void openHelp();
    void showContextMenu(POINT screenPt);
    bool isDark() const;

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    HACCEL accel_ = nullptr;
    HMENU menu_ = nullptr, devicesMenu_ = nullptr, partitionMenu_ = nullptr;
    HWND combo_ = nullptr, map_ = nullptr, list_ = nullptr, pending_ = nullptr, status_ = nullptr,
         statusRight_ = nullptr, banner_ = nullptr, bannerBtn_ = nullptr, tooltip_ = nullptr;
    std::vector<HWND> buttons_;
    HFONT font_ = nullptr;
    HBRUSH windowBrush_ = nullptr, panelBrush_ = nullptr, warnBrush_ = nullptr;
    HIMAGELIST fsImages_ = nullptr;
    UINT dpi_ = 96;
    ThemeMode themeMode_ = ThemeMode::System;

    std::thread worker_;
    bool scanning_ = false;
    std::unique_ptr<win::DiscoveryResult> result_;
    std::vector<DiskLayout> fallbackLayouts_;  // per disk, used when the raw table is unreadable
    DiskView view_;
    int diskIndex_ = -1;
    int selectedRow_ = -1;
};

}  // namespace wparted::ui
