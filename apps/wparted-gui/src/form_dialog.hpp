// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Small modal form dialog built from code (no dialog templates), used for the
// GParted-style operation dialogs (New, Resize/Move, Label, Flags, ...).
#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "theme.hpp"

namespace wparted::ui {

struct FormField {
    enum class Kind { Number, Text, Choice, Check, Note, Preview };
    Kind kind = Kind::Text;
    std::wstring label;
    // Number
    std::uint64_t number = 0, minimum = 0, maximum = UINT64_MAX;
    // Text
    std::wstring text;
    std::size_t maxLength = 64;
    // Choice
    std::vector<std::wstring> choices;
    int selected = 0;
    // Check
    bool checked = false;
    bool enabled = true;
    // Preview (graphical placement bar, always shown at the top of the form)
    std::uint64_t previewTotal = 0, previewStart = 0, previewSize = 0;
    COLORREF previewColor = RGB(0x2F, 0xB6, 0x8A);
    std::wstring previewLabel;
};

// Called whenever a value changes; may update other fields (e.g. "free space
// following") and returns an error message that blocks OK, or empty.
using FormUpdate = std::function<std::wstring(std::vector<FormField>&, int changedIndex)>;

// Shows the form modally. Returns true if the user confirmed with OK; the
// values are written back into `fields`.
bool runForm(HWND owner, const std::wstring& title, const std::wstring& intro, std::vector<FormField>& fields,
             const Palette& palette, HFONT font, UINT dpi, const FormUpdate& update = {});

}  // namespace wparted::ui
