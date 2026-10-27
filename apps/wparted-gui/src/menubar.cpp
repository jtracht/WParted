// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#include "menubar.hpp"

#include <cwchar>

namespace wparted::ui {

namespace {

// Undocumented but stable since Windows 10 1809; layouts as used by the
// Windows shell and widely by open-source dark-mode implementations.
constexpr UINT WM_UAHDRAWMENU = 0x0091;
constexpr UINT WM_UAHDRAWMENUITEM = 0x0092;

struct UAHMENU {
    HMENU hmenu;
    HDC hdc;
    DWORD dwFlags;
};
struct UAHMENUITEM {
    int iPosition;
    UINT dwFlags;  // UAHMENUITEMMETRICS / popup metrics follow; not needed here
};
struct UAHDRAWMENUITEM {
    DRAWITEMSTRUCT dis;
    UAHMENU um;
    UAHMENUITEM umi;
};

DWORD windowsBuild() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto fn = ntdll ? reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion"))) : nullptr;
    OSVERSIONINFOW v{sizeof v};
    if (!fn || fn(&v) != 0) return 0;
    return v.dwMajorVersion >= 10 ? v.dwBuildNumber : 0;
}

bool uahSupported() {
    static const bool ok = windowsBuild() >= 17763;  // Windows 10 1809
    return ok;
}

RECT menuBarRect(HWND hwnd) {
    MENUBARINFO mbi{sizeof mbi};
    if (!GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi)) return RECT{};
    RECT win;
    GetWindowRect(hwnd, &win);
    RECT r = mbi.rcBar;
    OffsetRect(&r, -win.left, -win.top);
    return r;
}

void drawMenuBottomLine(HWND hwnd, const Palette& pal) {
    // DefWindowProc draws a 1 px light line between menu bar and client area.
    RECT client, win;
    GetClientRect(hwnd, &client);
    MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&client), 2);
    GetWindowRect(hwnd, &win);
    OffsetRect(&client, -win.left, -win.top);
    RECT line = client;
    line.bottom = line.top;
    line.top--;
    HDC dc = GetWindowDC(hwnd);
    fillRect(dc, line, pal.window);
    ReleaseDC(hwnd, dc);
}

}  // namespace

void applyMenuTheme(bool dark) {
    if (windowsBuild() < 18362) return;  // SetPreferredAppMode exists from 1903
    HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
    if (!ux) ux = LoadLibraryExW(L"uxtheme.dll", nullptr, 0x00000800 /* LOAD_LIBRARY_SEARCH_SYSTEM32 */);
    if (!ux) return;
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    auto setMode = reinterpret_cast<SetPreferredAppModeFn>(reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(135))));
    auto flush = reinterpret_cast<FlushMenuThemesFn>(reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(136))));
    if (setMode) setMode(dark ? 2 /* ForceDark */ : 3 /* ForceLight */);
    if (flush) flush();
}

bool handleMenuBarMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, const Palette& pal, HFONT font, LRESULT& result) {
    if (!uahSupported()) return false;
    switch (msg) {
        case WM_UAHDRAWMENU: {
            auto* um = reinterpret_cast<UAHMENU*>(lp);
            RECT r = menuBarRect(hwnd);
            fillRect(um->hdc, r, pal.window);
            result = TRUE;
            return true;
        }
        case WM_UAHDRAWMENUITEM: {
            auto* dmi = reinterpret_cast<UAHDRAWMENUITEM*>(lp);
            wchar_t text[128] = L"";
            MENUITEMINFOW mii{sizeof mii};
            mii.fMask = MIIM_STRING;
            mii.dwTypeData = text;
            mii.cch = 127;
            GetMenuItemInfoW(dmi->um.hmenu, static_cast<UINT>(dmi->umi.iPosition), TRUE, &mii);
            const UINT st = dmi->dis.itemState;
            const bool hot = (st & (ODS_HOTLIGHT | ODS_SELECTED)) != 0;
            const bool disabled = (st & (ODS_INACTIVE | ODS_DISABLED | ODS_GRAYED)) != 0;
            RECT r = dmi->dis.rcItem;
            fillRect(dmi->um.hdc, r, hot ? pal.subtle : pal.window);
            SetBkMode(dmi->um.hdc, TRANSPARENT);
            SetTextColor(dmi->um.hdc, disabled ? pal.disabledText : pal.text);
            HGDIOBJ old = SelectObject(dmi->um.hdc, font);
            DrawTextW(dmi->um.hdc, text, -1, &r,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | ((st & ODS_NOACCEL) ? DT_HIDEPREFIX : 0));
            SelectObject(dmi->um.hdc, old);
            result = TRUE;
            return true;
        }
        case WM_NCPAINT:
        case WM_NCACTIVATE:
            result = DefWindowProcW(hwnd, msg, wp, lp);
            drawMenuBottomLine(hwnd, pal);
            return true;
        default:
            return false;
    }
}

}  // namespace wparted::ui
