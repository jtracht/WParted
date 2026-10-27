// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#include "theme.hpp"

#include <dwmapi.h>
#include <uxtheme.h>

namespace wparted::ui {

namespace {
constexpr wchar_t kRegKey[] = L"Software\\WParted";

template <typename Fn>
Fn loadProc(const wchar_t* dll, const char* name) {
    HMODULE m = GetModuleHandleW(dll);
    if (!m) {
        // Full System32 path: LOAD_LIBRARY_SEARCH_SYSTEM32 needs KB2533623 on Windows 7.
        wchar_t path[MAX_PATH];
        UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n == 0 || n + wcslen(dll) + 2 > MAX_PATH) return nullptr;
        wcscat_s(path, MAX_PATH, L"\\");
        wcscat_s(path, MAX_PATH, dll);
        m = LoadLibraryW(path);
    }
    return m ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, name))) : nullptr;
}
}  // namespace

ThemeMode loadThemeMode() {
    DWORD v = 0, size = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, L"Theme", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS && v <= 2)
        return static_cast<ThemeMode>(v);
    return ThemeMode::System;
}

void saveThemeMode(ThemeMode m) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        DWORD v = static_cast<DWORD>(m);
        RegSetValueExW(key, L"Theme", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof v);
        RegCloseKey(key);
    }
}

bool systemPrefersDark() {
    DWORD v = 1, size = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS)
        return v == 0;
    return false;  // Windows 7/8 have no app dark mode
}

const Palette& palette(bool dark) {
    static const Palette light{false,
                               RGB(0xFA, 0xFA, 0xFA), RGB(0xF0, 0xF0, 0xF0), RGB(0x1B, 0x1B, 0x1B), RGB(0x5F, 0x5F, 0x5F),
                               RGB(0xC8, 0xC8, 0xC8), RGB(0x00, 0x5F, 0xB8), RGB(0x00, 0x5F, 0xB8), RGB(0xA9, 0xA9, 0xA9),
                               RGB(0xF8, 0xF4, 0xB8), RGB(0xFF, 0xFF, 0xFF), RGB(0xFF, 0xF4, 0xCE)};
    static const Palette darkP{true,
                               RGB(0x20, 0x20, 0x20), RGB(0x2B, 0x2B, 0x2B), RGB(0xEE, 0xEE, 0xEE), RGB(0xA8, 0xA8, 0xA8),
                               RGB(0x45, 0x45, 0x45), RGB(0x4C, 0xA8, 0xFF), RGB(0x4C, 0xA8, 0xFF), RGB(0x6B, 0x6B, 0x6B),
                               RGB(0x6E, 0x69, 0x3A), RGB(0x33, 0x33, 0x33), RGB(0x4D, 0x40, 0x14)};
    return dark ? darkP : light;
}

void applyTitleBarTheme(HWND hwnd, bool dark) {
    using DwmSetAttr = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    static auto setAttr = loadProc<DwmSetAttr>(L"dwmapi.dll", "DwmSetWindowAttribute");
    if (!setAttr) return;
    BOOL value = dark ? TRUE : FALSE;
    // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (Windows 10 20H1+), 19 on 1809-1909.
    if (FAILED(setAttr(hwnd, 20, &value, sizeof value))) setAttr(hwnd, 19, &value, sizeof value);
}

void applyControlTheme(HWND control, bool dark, const wchar_t* darkClass) {
    using SetTheme = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    static auto setTheme = loadProc<SetTheme>(L"uxtheme.dll", "SetWindowTheme");
    if (setTheme) setTheme(control, dark ? darkClass : L"Explorer", nullptr);
}

COLORREF fsColor(FsType t) {
    switch (t) {
        case FsType::Ntfs: return RGB(0x42, 0xE5, 0xAC);
        case FsType::Fat12:
        case FsType::Fat16: return RGB(0x00, 0xC0, 0x5A);
        case FsType::Fat32: return RGB(0x18, 0xD9, 0x18);
        case FsType::ExFat: return RGB(0x2E, 0x8B, 0x57);
        case FsType::ReFs: return RGB(0x3B, 0x9C, 0xD9);
        case FsType::BitLocker: return RGB(0x49, 0x40, 0x66);
        case FsType::Ext2: return RGB(0x9D, 0xB8, 0xD2);
        case FsType::Ext3: return RGB(0x75, 0x90, 0xAE);
        case FsType::Ext4: return RGB(0x31, 0x4E, 0x6C);
        case FsType::Btrfs: return RGB(0xFF, 0x99, 0x55);
        case FsType::Xfs: return RGB(0xEE, 0xD6, 0x80);
        case FsType::F2fs: return RGB(0xDF, 0x4B, 0x2C);
        case FsType::LinuxSwap:
        case FsType::LinuxSuspend: return RGB(0xC1, 0x66, 0x5A);
        case FsType::Luks: return RGB(0x62, 0x5B, 0x81);
        case FsType::Lvm2Pv: return RGB(0xB1, 0x9C, 0xD9);
        case FsType::LinuxRaid: return RGB(0x82, 0x69, 0x4D);
        case FsType::HfsPlus:
        case FsType::Hfs: return RGB(0xE0, 0xB6, 0xAF);
        case FsType::Apfs: return RGB(0x87, 0x87, 0x87);
        case FsType::Udf: return RGB(0x10, 0x5A, 0x7A);
        case FsType::Zfs: return RGB(0x6B, 0x8E, 0x23);
        default: return RGB(0x00, 0x00, 0x00);
    }
}

UINT dpiForWindow(HWND hwnd) {
    using GetDpi = UINT(WINAPI*)(HWND);
    static auto getDpi = loadProc<GetDpi>(L"user32.dll", "GetDpiForWindow");  // Windows 10 1607+
    if (getDpi && hwnd) return getDpi(hwnd);
    HDC dc = GetDC(nullptr);
    UINT dpi = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX));
    ReleaseDC(nullptr, dc);
    return dpi ? dpi : 96;
}

HFONT createUiFont(UINT dpi, bool bold) {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    using SpiForDpi = BOOL(WINAPI*)(UINT, UINT, PVOID, UINT, UINT);
    static auto spiForDpi = loadProc<SpiForDpi>(L"user32.dll", "SystemParametersInfoForDpi");
    if (spiForDpi) {
        spiForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
    } else {
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
        UINT sys = dpiForWindow(nullptr);
        ncm.lfMessageFont.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, static_cast<int>(dpi), static_cast<int>(sys));
    }
    if (bold) ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
    return CreateFontIndirectW(&ncm.lfMessageFont);
}

}  // namespace wparted::ui
