// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#pragma once

#include <windows.h>

#include <commctrl.h>

#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "inspector.hpp"
#include "model.hpp"
#include "strings.hpp"
#include "theme.hpp"
#include "wparted/core/operations.hpp"
#include "wparted/core/validation.hpp"
#include "wparted/win/disk_discovery.hpp"

namespace wparted::ui {

// Translated texts shared by the main window and the planner (planner.cpp).
std::wstring formatText(S pattern, std::initializer_list<std::wstring> args);  // replaces %1..%9
std::wstring diagnosticText(const Diagnostic& d);  // validation and partition-table diagnostics
const wchar_t* supportText(Support s);
const wchar_t* tristateText(win::Tristate t);

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
    void updateInspector();
    bool showBanner() const;
    void paintBackground(HDC dc);
    void drawPendingItem(const DRAWITEMSTRUCT& di);
    LRESULT listCustomDraw(NMLVCUSTOMDRAW* cd);

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
    void startRescue();
    void onRescueDone(LPARAM lp);
    void startImageApply(bool resume = false);
    void offerResume(const std::wstring& path);
    void onImageApplyProgress(WPARAM op, LPARAM percent);
    void onImageApplyDone(LPARAM lp);
    void reloadImage(const std::wstring& path);
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
    HFONT font_ = nullptr, sectionFont_ = nullptr, iconFont_ = nullptr, captionFont_ = nullptr;
    HWND inspector_ = nullptr, applyBtn_ = nullptr;
    InspectorData inspectorData_;
    RECT listCard_{}, pendingCard_{};
    HBRUSH windowBrush_ = nullptr, panelBrush_ = nullptr, warnBrush_ = nullptr;
    HIMAGELIST fsImages_ = nullptr;
    UINT dpi_ = 96;
    ThemeMode themeMode_ = ThemeMode::System;

    std::thread worker_;
    std::thread rescueWorker_;
    bool rescuing_ = false;
    std::thread applyWorker_;     // plan execution on an image file
    bool applying_ = false;
    std::size_t applyTotal_ = 0;
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
