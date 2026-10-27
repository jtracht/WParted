// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#pragma once

#include <windows.h>

#include "wparted/core/filesystem.hpp"

namespace wparted::ui {

enum class ThemeMode { System = 0, Light = 1, Dark = 2 };

struct Palette {
    bool dark;
    COLORREF window, panel, text, mutedText, border, accent, selection, unallocated, used, unused, warning;
};

ThemeMode loadThemeMode();
void saveThemeMode(ThemeMode m);
bool systemPrefersDark();
const Palette& palette(bool dark);

// Applies the immersive dark title bar (Windows 10 1809+; ignored elsewhere)
// and the dark common-control theme to a window.
void applyTitleBarTheme(HWND hwnd, bool dark);
void applyControlTheme(HWND control, bool dark, const wchar_t* darkClass = L"DarkMode_Explorer");

// Colour used for a file system in the disk map and legend.
COLORREF fsColor(FsType t);

// DPI helpers that work from Windows 7 (system DPI) to Windows 11 (per monitor).
UINT dpiForWindow(HWND hwnd);
inline int scale(int px, UINT dpi) { return MulDiv(px, static_cast<int>(dpi), 96); }
HFONT createUiFont(UINT dpi, bool bold = false);

}  // namespace wparted::ui
