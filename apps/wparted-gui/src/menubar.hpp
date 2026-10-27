// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Themed menu bar: paints the classic Win32 menu bar in the window
// background colour (light and dark) via the UAH menu messages, and switches
// popup menus to dark mode on Windows 10 1903+. Falls back to the system look
// everywhere else.
#pragma once

#include <windows.h>

#include "theme.hpp"

namespace wparted::ui {

// Returns true and sets `result` if the message was handled.
bool handleMenuBarMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, const Palette& pal, HFONT font, LRESULT& result);
// Popup menus follow the theme (Windows 10 1903+; no-op elsewhere).
void applyMenuTheme(bool dark);

}  // namespace wparted::ui
