// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Details pane: shows the properties of the current selection and a hint
// for the next step of the workflow (select -> plan -> review -> apply).
#pragma once

#include <windows.h>

#include <string>
#include <utility>
#include <vector>

#include "theme.hpp"

namespace wparted::ui {

struct InspectorData {
    std::wstring title;
    std::wstring pill;           // e.g. file system name; empty = none
    COLORREF pillColor = 0;
    std::vector<std::pair<std::wstring, std::wstring>> rows;  // label, value
    std::wstring hint;           // next-step guidance
    bool warnHint = false;
};

constexpr UINT IM_SETDATA = WM_APP + 20;  // wParam: const Palette*, lParam: const InspectorData*
constexpr wchar_t kInspectorClass[] = L"WPartedInspector";
bool registerInspectorClass(HINSTANCE inst);

}  // namespace wparted::ui
