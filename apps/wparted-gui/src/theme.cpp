// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#include "theme.hpp"

#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <map>

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
    p.accentLip = hi;
    return p;
}

const Palette& palette(bool dark) {
    static Palette light, darkP, hc;
    if (g_highContrast) {
        hc = highContrastPalette();
        return hc;
    }
    // The colours of the website (wparted.tracht-digital.de), not the Windows accent.
    light = Palette{false,
                    RGB(0xF3, 0xF6, 0xFF), RGB(0xFF, 0xFF, 0xFF), RGB(0xFF, 0xFF, 0xFF), RGB(0xE8, 0xEC, 0xFF),
                    RGB(0xE2, 0xDB, 0xFF), RGB(0xD6, 0xDA, 0xF2),
                    RGB(0x2B, 0x25, 0x47), RGB(0x56, 0x50, 0x72), RGB(0x9A, 0x96, 0xB4),
                    RGB(0x5B, 0x3F, 0xD6), RGB(0xFF, 0xFF, 0xFF), RGB(0xE2, 0xDB, 0xFF),
                    RGB(0xFF, 0xF3, 0xC9), RGB(0x85, 0x57, 0x14), RGB(0xB3, 0x26, 0x1E), RGB(0x25, 0x67, 0x40),
                    RGB(0xE3, 0xE6, 0xF5), RGB(0xE2, 0xDB, 0xFF), RGB(0xF3, 0xF5, 0xFD)};
    light.accentLip = RGB(0x3F, 0x2A, 0xA3);
    darkP = Palette{true,
                    RGB(0x1C, 0x18, 0x30), RGB(0x2A, 0x24, 0x47), RGB(0x2A, 0x24, 0x47), RGB(0x33, 0x2B, 0x57),
                    RGB(0x36, 0x2D, 0x63), RGB(0x3D, 0x35, 0x66),
                    RGB(0xF1, 0xEE, 0xFF), RGB(0xBD, 0xB6, 0xDC), RGB(0x7A, 0x73, 0x99),
                    RGB(0xA9, 0x96, 0xFF), RGB(0x1C, 0x18, 0x30), RGB(0x36, 0x2D, 0x63),
                    RGB(0x3D, 0x33, 0x20), RGB(0xE9, 0xB4, 0x55), RGB(0xFF, 0x7A, 0x6E), RGB(0x5F, 0xD3, 0x94),
                    RGB(0x3A, 0x33, 0x61), RGB(0x36, 0x2D, 0x63), RGB(0x2A, 0x24, 0x47)};
    darkP.accentLip = RGB(0x7C, 0x66, 0xE6);
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
    // Pastels built around the website's mint, pink, butter and lavender.
    switch (t) {
        case FsType::Ntfs: return RGB(0x9F, 0xE3, 0xC8);
        case FsType::Fat12:
        case FsType::Fat16: return RGB(0xFF, 0xEA, 0xB0);
        case FsType::Fat32: return RGB(0xFF, 0xE0, 0x8A);
        case FsType::ExFat: return RGB(0xFF, 0xB3, 0xCF);
        case FsType::ReFs: return RGB(0xA9, 0xD8, 0xF5);
        case FsType::BitLocker: return RGB(0xB9, 0xAE, 0xDD);
        case FsType::Ext2: return RGB(0xDF, 0xD6, 0xFF);
        case FsType::Ext3: return RGB(0xD3, 0xC7, 0xFF);
        case FsType::Ext4: return RGB(0xC7, 0xB8, 0xFF);
        case FsType::Btrfs: return RGB(0xFF, 0xC9, 0xA3);
        case FsType::Xfs: return RGB(0xEE, 0xDC, 0x9A);
        case FsType::F2fs: return RGB(0xFF, 0xB8, 0xA8);
        case FsType::LinuxSwap:
        case FsType::LinuxSuspend: return RGB(0xF5, 0xB5, 0xAC);
        case FsType::Luks: return RGB(0xCD, 0xB8, 0xE8);
        case FsType::Lvm2Pv: return RGB(0xE3, 0xC8, 0xF5);
        case FsType::LinuxRaid: return RGB(0xD8, 0xC4, 0xAA);
        case FsType::HfsPlus:
        case FsType::Hfs: return RGB(0xF2, 0xCF, 0xC9);
        case FsType::Apfs: return RGB(0xCF, 0xCF, 0xD8);
        case FsType::Udf: return RGB(0xA6, 0xDC, 0xE0);
        case FsType::Zfs: return RGB(0xCF, 0xE3, 0xA0);
        default: return RGB(0xD3, 0xD3, 0xE0);
    }
}

COLORREF readableOn(COLORREF bg) { return luminance(bg) > 150 ? RGB(0x2B, 0x25, 0x47) : RGB(0xFF, 0xFF, 0xFF); }

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
    // Caption 12, Body 14, Section 16, Subtitle 20, Title 28 (px at 96 DPI); headings are bold, as on the website.
    int px = 14, weight = FW_NORMAL;
    switch (role) {
        case FontRole::Body: break;
        case FontRole::BodyStrong: weight = FW_BOLD; break;
        case FontRole::Caption: px = 12; break;
        case FontRole::Section: px = 16; weight = FW_BOLD; break;
        case FontRole::Subtitle: px = 20; weight = FW_BOLD; break;
        case FontRole::Title: px = 28; weight = FW_BOLD; break;
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

namespace {
COLORREF mixF(COLORREF a, COLORREF b, double t) {
    auto mix = [t](int x, int y) { return static_cast<int>(x + (y - x) * t + 0.5); };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)), mix(GetBValue(a), GetBValue(b)));
}

double coverage(double edge, double dist) { return std::min(1.0, std::max(0.0, edge - dist + 0.5)); }

void blendPixel(HDC dc, int x, int y, COLORREF c, double cov) {
    const COLORREF bg = GetPixel(dc, x, y);
    if (bg != CLR_INVALID) SetPixelV(dc, x, y, mixF(bg, c, cov));
}
}  // namespace

void fillRounded(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF stroke) {
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    const int rad = std::max(0, std::min({radius, w / 2, h / 2}));
    const bool outlined = stroke != fill;
    fillRect(dc, RECT{r.left + rad, r.top, r.right - rad, r.bottom}, fill);
    fillRect(dc, RECT{r.left, r.top + rad, r.left + rad, r.bottom - rad}, fill);
    fillRect(dc, RECT{r.right - rad, r.top + rad, r.right, r.bottom - rad}, fill);
    if (outlined) {
        fillRect(dc, RECT{r.left + rad, r.top, r.right - rad, r.top + 1}, stroke);
        fillRect(dc, RECT{r.left + rad, r.bottom - 1, r.right - rad, r.bottom}, stroke);
        fillRect(dc, RECT{r.left, r.top + rad, r.left + 1, r.bottom - rad}, stroke);
        fillRect(dc, RECT{r.right - 1, r.top + rad, r.right, r.bottom - rad}, stroke);
    }
    for (int dy = 0; dy < rad; ++dy) {
        int solidFrom = rad;  // pixels from here to the centre line are plain fill
        for (int dx = 0; dx < rad; ++dx) {
            const double d = std::hypot(rad - dx - 0.5, rad - dy - 0.5);
            const double outer = coverage(rad, d);
            if (outer <= 0) continue;
            const double inner = outlined ? coverage(rad - 1.0, d) : 1.0;
            if (outer >= 1 && inner >= 1) {
                solidFrom = dx;
                break;
            }
            const COLORREF c = mixF(stroke, fill, inner);
            blendPixel(dc, r.left + dx, r.top + dy, c, outer);
            blendPixel(dc, r.right - 1 - dx, r.top + dy, c, outer);
            blendPixel(dc, r.left + dx, r.bottom - 1 - dy, c, outer);
            blendPixel(dc, r.right - 1 - dx, r.bottom - 1 - dy, c, outer);
        }
        if (solidFrom < rad) {
            fillRect(dc, RECT{r.left + solidFrom, r.top + dy, r.left + rad, r.top + dy + 1}, fill);
            fillRect(dc, RECT{r.right - rad, r.top + dy, r.right - solidFrom, r.top + dy + 1}, fill);
            fillRect(dc, RECT{r.left + solidFrom, r.bottom - 1 - dy, r.left + rad, r.bottom - dy}, fill);
            fillRect(dc, RECT{r.right - rad, r.bottom - 1 - dy, r.right - solidFrom, r.bottom - dy}, fill);
        }
    }
}

void strokeRounded(HDC dc, const RECT& r, int radius, int width, COLORREF color) {
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    const int rad = std::max(0, std::min({radius, w / 2, h / 2}));
    fillRect(dc, RECT{r.left + rad, r.top, r.right - rad, r.top + width}, color);
    fillRect(dc, RECT{r.left + rad, r.bottom - width, r.right - rad, r.bottom}, color);
    fillRect(dc, RECT{r.left, r.top + rad, r.left + width, r.bottom - rad}, color);
    fillRect(dc, RECT{r.right - width, r.top + rad, r.right, r.bottom - rad}, color);
    for (int dy = 0; dy < rad; ++dy) {
        for (int dx = 0; dx < rad; ++dx) {
            const double d = std::hypot(rad - dx - 0.5, rad - dy - 0.5);
            const double cov = coverage(rad, d) * (1.0 - coverage(rad - static_cast<double>(width), d));
            if (cov <= 0) continue;
            blendPixel(dc, r.left + dx, r.top + dy, color, cov);
            blendPixel(dc, r.right - 1 - dx, r.top + dy, color, cov);
            blendPixel(dc, r.left + dx, r.bottom - 1 - dy, color, cov);
            blendPixel(dc, r.right - 1 - dx, r.bottom - 1 - dy, color, cov);
        }
    }
}

void drawCard(HDC dc, const RECT& r, int radius, const Palette& p, UINT dpi) {
    fillRounded(dc, r, radius, p.border, p.border);
    RECT top = r;
    top.bottom -= scale(4, dpi);
    fillRounded(dc, top, radius, p.card, p.border);
}

void drawPrimaryFill(HDC dc, const RECT& r, const Palette& p, UINT dpi, bool disabled, bool pressed, bool hot) {
    const int h = r.bottom - r.top;
    if (disabled) {
        const COLORREF fill = p.highContrast ? p.window : p.subtle;
        fillRounded(dc, r, h, fill, p.highContrast ? p.disabledText : fill);
        return;
    }
    fillRounded(dc, r, h, p.accentLip, p.accentLip);
    RECT top = r;
    top.bottom -= pressed ? scale(1, dpi) : scale(4, dpi);
    if (pressed) top.top += scale(2, dpi);
    const COLORREF fill = hot && !pressed && !p.highContrast ? blend(p.accent, p.accentLip, 25) : p.accent;
    fillRounded(dc, top, h, fill, fill);
}

void fillStripes(HDC dc, const RECT& r, int radius, COLORREF a, COLORREF b, UINT dpi) {
    fillRounded(dc, r, radius, b, b);
    const int inset = 1;
    const int rad = std::min({radius, static_cast<int>(r.right - r.left) / 2, static_cast<int>(r.bottom - r.top) / 2});
    HRGN clip = CreateRoundRectRgn(r.left + inset, r.top + inset, r.right - inset + 1, r.bottom - inset + 1, 2 * rad, 2 * rad);
    const int saved = SaveDC(dc);
    ExtSelectClipRgn(dc, clip, RGN_AND);
    HPEN pen = CreatePen(PS_SOLID, scale(7, dpi), a);
    HGDIOBJ old = SelectObject(dc, pen);
    const int h = r.bottom - r.top, step = scale(14, dpi);
    for (int x = r.left - h; x < r.right + h; x += step) {
        MoveToEx(dc, x, r.bottom, nullptr);
        LineTo(dc, x + h, r.top);
    }
    SelectObject(dc, old);
    DeleteObject(pen);
    RestoreDC(dc, saved);
    DeleteObject(clip);
}

void drawParti(HDC dc, int x, int y, int size) {
    static std::map<int, HICON> icons;  // few sizes, kept for the lifetime of the process
    HICON& icon = icons[size];
    if (!icon)
        icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, 0));
    if (icon) DrawIconEx(dc, x, y, icon, size, size, 0, nullptr, DI_NORMAL);
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
