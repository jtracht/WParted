// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#include <windows.h>

#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <string>

#include "main_window.hpp"
#include "strings.hpp"
#include "wparted/core/log.hpp"
#include "wparted/win/disk_discovery.hpp"
#include "wparted_version.h"

namespace {

void openLog() {
    wchar_t* base = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) return;
    std::wstring dir = std::wstring(base) + L"\\WParted\\logs";
    CoTaskMemFree(base);
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    wparted::log::openFile(wparted::win::narrow(dir + L"\\wparted.log"));
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    // Harden DLL search order (KB2533623 on Windows 7; no-op where unsupported).
    using SetDefaultDllDirs = BOOL(WINAPI*)(DWORD);
    if (auto fn = reinterpret_cast<SetDefaultDllDirs>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetDefaultDllDirectories"))))
        fn(LOAD_LIBRARY_SEARCH_SYSTEM32);
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);
    wparted::ui::initLanguage();
    openLog();
    wparted::log::info(std::string("WParted ") + WPARTED_VERSION_STRING + " starting (read-only build)");

    wparted::ui::MainWindow window;
    // Like GParted, an image file can be given on the command line (opened read-only).
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        if (argc >= 2) window.setStartupImage(argv[1]);
        LocalFree(argv);
    }
    if (!window.create(inst, show)) {
        wparted::log::error("Main window creation failed");
        return 1;
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (TranslateAcceleratorW(window.hwnd(), window.accelerators(), &msg)) continue;
        if (IsDialogMessageW(window.hwnd(), &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    wparted::log::info("WParted exiting");
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
