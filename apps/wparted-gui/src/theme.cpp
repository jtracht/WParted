// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#include "theme.hpp"

#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <cwchar>

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

COLORREF g_accentLight = RGB(0x00, 0x5F, 0xB8);  // Fluent default accent ("Blue")
COLORREF g_accentDark = RGB(0x60, 0xCD, 0xFF);
bool g_highContrast = false;  // Windows high-contrast mode (refreshHighContrast)

int luminance(COLORREF c) { return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000; }

int CALLBACK fontFound(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found) {
    *reinterpret_cast<bool*>(found) = true;
    return 0;
}

bool fontInstalled(const wchar_t* face) {
    bool found = false;
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(lf.lfFaceName, face, LF_FACESIZE - 1);
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(dc, &lf, fontFound, reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}

const wchar_t* uiFace() {
    static const wchar_t* face = fontInstalled(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    return face;
}

const wchar_t* iconFace() {
    static const wchar_t* face = fontInstalled(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons"
                                 : fontInstalled(L"Segoe MDL2 Assets") ? L"Segoe MDL2 Assets"
                                                                       : nullptr;
    return face;
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

void refreshAccentColor() {
    // HKCU\...\DWM\AccentColor is ABGR (Windows 8+). Adjust per theme so that
    // accent text and fills keep sufficient contrast.
    DWORD v = 0, size = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, nullptr,
                     &v, &size) != ERROR_SUCCESS)
        return;
    const COLORREF c = RGB(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF);
    COLORREF lightVariant = c, darkVariant = c;
    while (luminance(lightVariant) > 110) lightVariant = blend(lightVariant, RGB(0, 0, 0), 15);
    while (luminance(darkVariant) < 150) darkVariant = blend(darkVariant, RGB(255, 255, 255), 15);
    g_accentLight = lightVariant;
    g_accentDark = darkVariant;
}

void refreshHighContrast() {
    HIGHCONTRASTW hc{sizeof hc};
    g_highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

bool highContrastActive() { return g_highContrast; }

// System colours only; muted and semantic colours collapse to the text colour
// because high-contrast themes define no secondary text colours.
static Palette highContrastPalette() {
    const COLORREF win = GetSysColor(COLOR_WINDOW), text = GetSysColor(COLOR_WINDOWTEXT);
    const COLORREF face = GetSysColor(COLOR_BTNFACE), hi = GetSysColor(COLOR_HIGHLIGHT);
    const COLORREF hiText = GetSysColor(COLOR_HIGHLIGHTTEXT), gray = GetSysColor(COLOR_GRAYTEXT);
    Palette p{false, win, face, win, win, face, text, text, text, gray, hi, hiText, hi,
              win, text, text, text, gray, blend(win, hi, 35), win};
    p.highContrast = true;
    return p;
}

const Palette& palette(bool dark) {
    static Palette light, darkP, hc;
    if (g_highContrast) {
        hc = highContrastPalette();
        return hc;
    }
    light = Palette{false,
                    RGB(0xF3, 0xF3, 0xF3), RGB(0xFB, 0xFB, 0xFB), RGB(0xFF, 0xFF, 0xFF), RGB(0xF0, 0xF0, 0xF0),
                    RGB(0xE6, 0xE6, 0xE6), RGB(0xE5, 0xE5, 0xE5),
                    RGB(0x1A, 0x1A, 0x1A), RGB(0x5D, 0x5D, 0x5D), RGB(0xA0, 0xA0, 0xA0),
                    g_accentLight, RGB(0xFF, 0xFF, 0xFF), blend(RGB(0xFF, 0xFF, 0xFF), g_accentLight, 14),
                    RGB(0xFF, 0xF4, 0xCE), RGB(0x9D, 0x5D, 0x00), RGB(0xC4, 0x2B, 0x1C), RGB(0x0F, 0x7B, 0x0F),
                    RGB(0xC8, 0xC8, 0xC8), RGB(0xD6, 0xE9, 0xF8), RGB(0xFF, 0xFF, 0xFF)};
    darkP = Palette{true,
                    RGB(0x20, 0x20, 0x20), RGB(0x2D, 0x2D, 0x2D), RGB(0x2B, 0x2B, 0x2B), RGB(0x34, 0x34, 0x34),
                    RGB(0x29, 0x29, 0x29), RGB(0x3D, 0x3D, 0x3D),
                    RGB(0xFF, 0xFF, 0xFF), RGB(0xC5, 0xC5, 0xC5), RGB(0x78, 0x78, 0x78),
                    g_accentDark, RGB(0x00, 0x00, 0x00), blend(RGB(0x2B, 0x2B, 0x2B), g_accentDark, 22),
                    RGB(0x43, 0x35, 0x19), RGB(0xFC, 0xE1, 0x00), RGB(0xFF, 0x99, 0xA4), RGB(0x6C, 0xCB, 0x5F),
                    RGB(0x55, 0x55, 0x55), RGB(0x2F, 0x4A, 0x62), RGB(0x33, 0x33, 0x33)};
    return dark ? darkP : light;
}

void applyTitleBarTheme(HWND hwnd, bool dark) {
    using DwmSetAttr = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    static auto setAttr = loadProc<DwmSetAttr>(L"dwmapi.dll", "DwmSetWindowAttribute");
    if (!setAttr) return;
    BOOL value = dark ? TRUE : FALSE;
    // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (Windows 10 20H1+), 19 on 1809-1909.
    if (FAILED(setAttr(hwnd, 20, &value, sizeof value))) setAttr(hwnd, 19, &value, sizeof value);
    // 35 = DWMWA_CAPTION_COLOR (Windows 11): title bar blends into the window.
    // DWMWA_COLOR_DEFAULT (0xFFFFFFFF) leaves the caption to the system in high contrast.
    COLORREF caption = g_highContrast ? 0xFFFFFFFF : palette(dark).window;
    setAttr(hwnd, 35, &caption, sizeof caption);
}

void applyControlTheme(HWND control, bool dark, const wchar_t* darkClass) {
    using SetTheme = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    static auto setTheme = loadProc<SetTheme>(L"uxtheme.dll", "SetWindowTheme");
    if (setTheme) setTheme(control, dark ? darkClass : L"Explorer", nullptr);
}

COLORREF fsColor(FsType t) {
    switch (t) {
        case FsType::Ntfs: return RGB(0x2F, 0xB6, 0x8A);
        case FsType::Fat12:
        case FsType::Fat16: return RGB(0x3C, 0xA4, 0x3C);
        case FsType::Fat32: return RGB(0x4C, 0xB8, 0x2E);
        case FsType::ExFat: return RGB(0x2E, 0x8B, 0x57);
        case FsType::ReFs: return RGB(0x3B, 0x8E, 0xD0);
        case FsType::BitLocker: return RGB(0x6B, 0x5B, 0x95);
        case FsType::Ext2: return RGB(0x7F, 0x9E, 0xC0);
        case FsType::Ext3: return RGB(0x5A, 0x7E, 0xA8);
        case FsType::Ext4: return RGB(0x3A, 0x5F, 0x8F);
        case FsType::Btrfs: return RGB(0xE8, 0x84, 0x3C);
        case FsType::Xfs: return RGB(0xC9, 0xA2, 0x27);
        case FsType::F2fs: return RGB(0xD0, 0x4A, 0x2E);
        case FsType::LinuxSwap:
        case FsType::LinuxSuspend: return RGB(0xB8, 0x5C, 0x52);
        case FsType::Luks: return RGB(0x7A, 0x5C, 0xB0);
        case FsType::Lvm2Pv: return RGB(0x9C, 0x7C, 0xC8);
        case FsType::LinuxRaid: return RGB(0x8A, 0x6D, 0x4D);
        case FsType::HfsPlus:
        case FsType::Hfs: return RGB(0xC9, 0x8E, 0x86);
        case FsType::Apfs: return RGB(0x80, 0x80, 0x88);
        case FsType::Udf: return RGB(0x1F, 0x6F, 0x8F);
        case FsType::Zfs: return RGB(0x6B, 0x8E, 0x23);
        default: return RGB(0x78, 0x78, 0x78);
    }
}

COLORREF readableOn(COLORREF bg) { return luminance(bg) > 150 ? RGB(0x1A, 0x1A, 0x1A) : RGB(0xFF, 0xFF, 0xFF); }

COLORREF blend(COLORREF a, COLORREF b, int pct) {
    auto mix = [pct](int x, int y) { return x + (y - x) * pct / 100; };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)), mix(GetBValue(a), GetBValue(b)));
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

HFONT createUiFont(UINT dpi, FontRole role) {
    // Fluent type ramp: Caption 12, Body 14, Subtitle 20, Title 28 (px at 96 DPI).
    int px = 14, weight = FW_NORMAL;
    switch (role) {
        case FontRole::Body: break;
        case FontRole::BodyStrong: weight = FW_SEMIBOLD; break;
        case FontRole::Caption: px = 12; break;
        case FontRole::Subtitle: px = 20; weight = FW_SEMIBOLD; break;
        case FontRole::Title: px = 28; weight = FW_SEMIBOLD; break;
    }
    LOGFONTW lf{};
    lf.lfHeight = -scale(px, dpi);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcsncpy_s(lf.lfFaceName, uiFace(), LF_FACESIZE - 1);
    return CreateFontIndirectW(&lf);
}

bool hasIconFont() { return iconFace() != nullptr; }

HFONT createIconFont(UINT dpi, int sizePx) {
    if (!iconFace()) return nullptr;
    LOGFONTW lf{};
    lf.lfHeight = -scale(sizePx, dpi);
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcsncpy_s(lf.lfFaceName, iconFace(), LF_FACESIZE - 1);
    return CreateFontIndirectW(&lf);
}

void fillRect(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void fillRounded(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF stroke) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, stroke);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

void drawPill(HDC dc, const RECT& r, COLORREF fill, COLORREF textColor, const wchar_t* text, HFONT font, UINT dpi) {
    HGDIOBJ of = SelectObject(dc, font);
    SIZE sz{};
    GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &sz);
    const int h = std::min<int>(r.bottom - r.top, sz.cy + scale(6, dpi));
    const int w = std::min<int>(r.right - r.left, sz.cx + scale(16, dpi));
    RECT pill{r.left, r.top + (r.bottom - r.top - h) / 2, r.left + w, r.top + (r.bottom - r.top - h) / 2 + h};
    fillRounded(dc, pill, h, fill, fill);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, textColor);
    DrawTextW(dc, text, -1, &pill, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, of);
}

}  // namespace wparted::ui
