// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#pragma once

#include <windows.h>

#include <commctrl.h>

#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "model.hpp"
#include "theme.hpp"
#include "wparted/core/operations.hpp"
#include "wparted/core/validation.hpp"
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

    // Planning (planner.cpp). Nothing here writes to a disk.
    OperationQueue* currentQueue();
    const win::PhysicalDisk* currentDisk() const;
    ValidationContext validationContext(ValidationMode mode) const;
    void refreshView();
    void updateCommandState();
    const PartitionEntry* selectedPartition() const;
    const Row* selectedRowData() const;
    bool addOperation(Operation op);
    void planNew();
    void planDelete();
    void planResize();
    void planCopy();
    void planPaste();
    void planFormat(FsType fs);
    void planLabel();
    void planName();
    void planFlags();
    void planSimple(OpType type);
    void planCreateTable();
    void showApplySummary();
    void savePlan();
    void openImage(const std::wstring& path);
    void chooseImage();
    bool saveTextFile(const std::wstring& path, const std::string& text);
    std::wstring askSavePath(const wchar_t* defaultName, const wchar_t* filter, const wchar_t* ext);

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

    std::map<std::wstring, OperationQueue> queues_;  // pending operations per device path
    std::vector<win::PhysicalDisk> images_;           // opened disk image files (read-only)
    std::wstring pendingImage_;                       // image given on the command line
    std::wstring copySourceDisk_;
    std::uint32_t copySource_ = 0;                    // planId of the copied partition
    HMENU formatMenu_ = nullptr;

public:
    void setStartupImage(const std::wstring& path) { pendingImage_ = path; }
};

}  // namespace wparted::ui
