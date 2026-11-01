// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Design tokens (the look of the WParted website, see docs/design/wparted-design-system.md)
// and drawing helpers shared by all custom-drawn controls.
#pragma once

#include <windows.h>

#include "wparted/core/filesystem.hpp"

namespace wparted::ui {

enum class ThemeMode { System = 0, Light = 1, Dark = 2 };

struct Palette {
    bool dark;
    // Surfaces
    COLORREF window;      // layer behind cards (Mica-like neutral)
    COLORREF panel;       // control fill (buttons, inputs)
    COLORREF card;        // cards and lists
    COLORREF subtle;      // hover fill
    COLORREF pressed;     // pressed fill
    COLORREF border;      // card/control stroke
    // Text
    COLORREF text;        // primary
    COLORREF mutedText;   // secondary
    COLORREF disabledText;
    // Semantic
    COLORREF accent;      // brand violet
    COLORREF onAccent;    // text on accent
    COLORREF selection;   // selected row fill
    COLORREF warning;     // caution background (info bar)
    COLORREF caution;     // caution foreground
    COLORREF danger;
    COLORREF success;
    // Disk map
    COLORREF unallocated, used, unused;
    // Windows high-contrast mode: colours come from GetSysColor, no dark-mode
    // control themes and no custom-drawn menu bar.
    bool highContrast = false;
    COLORREF accentLip = 0;  // the darker lower edge of the primary button
};

ThemeMode loadThemeMode();
void saveThemeMode(ThemeMode m);
bool systemPrefersDark();
// Reads the high-contrast setting; call again on WM_SETTINGCHANGE / WM_SYSCOLORCHANGE.
void refreshHighContrast();
bool highContrastActive();
const Palette& palette(bool dark);

// Title bar: dark mode (Windows 10 1809+) and caption colour matching the
// window background (Windows 11); ignored where unsupported.
void applyTitleBarTheme(HWND hwnd, bool dark);
void applyControlTheme(HWND control, bool dark, const wchar_t* darkClass = L"DarkMode_Explorer");

// Colour used for a file system in the disk map and the list.
COLORREF fsColor(FsType t);
// Black or white, whichever reads better on `bg`.
COLORREF readableOn(COLORREF bg);
COLORREF blend(COLORREF a, COLORREF b, int percentB);

// DPI helpers (per monitor).
UINT dpiForWindow(HWND hwnd);
inline int scale(int px, UINT dpi) { return MulDiv(px, static_cast<int>(dpi), 96); }

enum class FontRole { Body, BodyStrong, Caption, Section, Subtitle, Title };
// Segoe UI Variable (Windows 11) or Segoe UI, sized per role.
HFONT createUiFont(UINT dpi, FontRole role = FontRole::Body);
inline HFONT createUiFont(UINT dpi, bool bold) { return createUiFont(dpi, bold ? FontRole::BodyStrong : FontRole::Body); }
// Segoe Fluent Icons / Segoe MDL2 Assets, or nullptr without an icon font.
HFONT createIconFont(UINT dpi, int sizePx);
bool hasIconFont();

// Icon glyphs (code points shared by Segoe Fluent Icons and Segoe MDL2 Assets).
namespace glyph {
constexpr wchar_t Refresh = 0xE72C, Add = 0xE710, Delete = 0xE74D, Resize = 0xE740, Copy = 0xE8C8, Paste = 0xE77F,
                  Undo = 0xE7A7, Apply = 0xE73E, Info = 0xE946, Warning = 0xE7BA, Error = 0xEA39, Shield = 0xEA18,
                  HardDrive = 0xEDA2, Usb = 0xE88E, Disc = 0xE958, Save = 0xE74E, Open = 0xE8E5, Check = 0xE73E;
}

// Drawing primitives. Rounded shapes are anti-aliased against what is already in the DC,
// so the background has to be painted first.
void fillRounded(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF stroke);
void strokeRounded(HDC dc, const RECT& r, int radius, int width, COLORREF color);
void fillRect(HDC dc, const RECT& r, COLORREF c);
// A card: rounded surface resting on a soft lower edge.
void drawCard(HDC dc, const RECT& r, int radius, const Palette& p, UINT dpi);
// The primary button surface: a pill on a darker lip that flattens when pressed.
void drawPrimaryFill(HDC dc, const RECT& r, const Palette& p, UINT dpi, bool disabled, bool pressed, bool hot);
// Diagonal stripes for free space.
void fillStripes(HDC dc, const RECT& r, int radius, COLORREF a, COLORREF b, UINT dpi);
// Parti, the mascot, from the icon resource; size in device pixels.
void drawParti(HDC dc, int x, int y, int size);
void drawPill(HDC dc, const RECT& r, COLORREF fill, COLORREF textColor, const wchar_t* text, HFONT font, UINT dpi);

}  // namespace wparted::ui
