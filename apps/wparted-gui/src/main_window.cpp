// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#include "main_window.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "strings.hpp"
#include "wparted/core/log.hpp"
#include "wparted/win/report.hpp"
#include "wparted_version.h"

namespace wparted::ui {

namespace {

constexpr wchar_t kWindowClass[] = L"WPartedMainWindow";
constexpr UINT WM_APP_DISCOVERED = WM_APP + 50;

enum : int {
    IDC_COMBO = 100, IDC_MAP, IDC_LIST, IDC_PENDING, IDC_STATUS, IDC_STATUS_RIGHT, IDC_BANNER, IDC_BANNER_BTN,
    IDC_BTN_FIRST = 120,  // toolbar buttons map 1:1 to the command ids below
    IDM_REFRESH = 1000, IDM_RESTART_ADMIN, IDM_QUIT,
    IDM_UNDO, IDM_CLEAR_OPS, IDM_APPLY_OPS,
    IDM_DEVICE_INFO, IDM_PENDING_OPS, IDM_THEME_SYSTEM, IDM_THEME_LIGHT, IDM_THEME_DARK, IDM_FS_SUPPORT,
    IDM_CREATE_TABLE, IDM_EXPORT_REPORT,
    IDM_NEW, IDM_DELETE, IDM_RESIZE, IDM_COPY, IDM_PASTE, IDM_FORMAT, IDM_FLAGS, IDM_CHECK, IDM_LABEL, IDM_UUID,
    IDM_PART_INFO,
    IDM_OPEN_LOGS, IDM_HELP, IDM_ABOUT,
    IDM_DEVICE_BASE = 2000,
};

// Commands that modify disks. They exist in the UI to keep the GParted
// workflow familiar, but stay disabled until the operation engine ships.
const int kPlannedCommands[] = {IDM_UNDO, IDM_CLEAR_OPS, IDM_APPLY_OPS, IDM_CREATE_TABLE, IDM_NEW, IDM_DELETE,
                                IDM_RESIZE, IDM_COPY, IDM_PASTE, IDM_FORMAT, IDM_FLAGS, IDM_CHECK, IDM_LABEL, IDM_UUID};

bool isPlanned(int id) {
    for (int p : kPlannedCommands)
        if (p == id) return true;
    return false;
}

struct ToolButton {
    int command;
    S label;
};
const ToolButton kToolButtons[] = {
    {IDM_REFRESH, S::BtnRefresh}, {IDM_NEW, S::BtnNew},   {IDM_DELETE, S::BtnDelete}, {IDM_RESIZE, S::BtnResizeMove},
    {IDM_COPY, S::BtnCopy},       {IDM_PASTE, S::BtnPaste}, {IDM_UNDO, S::BtnUndo},   {IDM_APPLY_OPS, S::BtnApply},
};

enum Column { ColPartition, ColName, ColFs, ColMount, ColLabel, ColSize, ColUsed, ColUnused, ColFlags, ColStatus, ColCount };

std::wstring w(const std::string& s) { return win::widen(s); }
std::wstring sizeText(std::uint64_t bytes) { return w(formatSize(bytes)); }

// Toolbar buttons are owner-drawn so that the disabled state is clearly
// visible in both themes (the dark common-control theme does not grey them).
constexpr wchar_t kHotProp[] = L"WPartedHot";

LRESULT CALLBACK buttonSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_MOUSEMOVE && !GetPropW(h, kHotProp)) {
        TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        SetPropW(h, kHotProp, reinterpret_cast<HANDLE>(1));
        InvalidateRect(h, nullptr, FALSE);
    } else if (msg == WM_MOUSELEAVE) {
        RemovePropW(h, kHotProp);
        InvalidateRect(h, nullptr, FALSE);
    } else if (msg == WM_NCDESTROY) {
        RemovePropW(h, kHotProp);
        RemoveWindowSubclass(h, buttonSubclass, 1);
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// Colours the list view header text, which otherwise stays black in dark mode.
LRESULT CALLBACK listSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    if (msg == WM_NOTIFY) {
        auto* nm = reinterpret_cast<NMHDR*>(lp);
        if (nm->hwndFrom == ListView_GetHeader(h) && nm->code == NM_CUSTOMDRAW) {
            auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(lp);
            if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                SetTextColor(cd->hdc, reinterpret_cast<const Palette*>(ref)->text);
                return CDRF_DODEFAULT;
            }
        }
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(h, listSubclass, 1);
    }
    return DefSubclassProc(h, msg, wp, lp);
}

void drawButton(const DRAWITEMSTRUCT& di, const Palette& p, UINT dpi) {
    const bool disabled = (di.itemState & ODS_DISABLED) != 0;
    const bool pressed = (di.itemState & ODS_SELECTED) != 0;
    const bool hot = !disabled && GetPropW(di.hwndItem, kHotProp) != nullptr;
    COLORREF bg = p.panel;
    if (pressed) bg = p.border;
    else if (hot) bg = p.dark ? RGB(0x3A, 0x3A, 0x3A) : RGB(0xE5, 0xF1, 0xFB);
    HBRUSH b = CreateSolidBrush(p.window);
    FillRect(di.hDC, &di.rcItem, b);
    DeleteObject(b);
    HBRUSH fillBrush = CreateSolidBrush(disabled ? p.window : bg);
    HPEN pen = CreatePen(PS_SOLID, 1, hot ? p.accent : p.border);
    HGDIOBJ oldB = SelectObject(di.hDC, fillBrush), oldP = SelectObject(di.hDC, pen);
    const int radius = scale(6, dpi);
    RoundRect(di.hDC, di.rcItem.left, di.rcItem.top, di.rcItem.right, di.rcItem.bottom, radius, radius);
    SelectObject(di.hDC, oldB);
    SelectObject(di.hDC, oldP);
    DeleteObject(fillBrush);
    DeleteObject(pen);
    wchar_t text[64];
    GetWindowTextW(di.hwndItem, text, 64);
    SetBkMode(di.hDC, TRANSPARENT);
    SetTextColor(di.hDC, disabled ? (p.dark ? RGB(0x6E, 0x6E, 0x6E) : RGB(0xA0, 0xA0, 0xA0)) : p.text);
    RECT r = di.rcItem;
    DrawTextW(di.hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if ((di.itemState & ODS_FOCUS) && !(di.itemState & ODS_NOFOCUSRECT)) {
        RECT f = di.rcItem;
        InflateRect(&f, -scale(3, dpi), -scale(3, dpi));
        DrawFocusRect(di.hDC, &f);
    }
}

// Explicit CALLBACK (__stdcall on x86): a lambda would only convert with MSVC.
HRESULT CALLBACK aboutDialogCallback(HWND, UINT n, WPARAM, LPARAM lp, LONG_PTR) {
    if (n == TDN_HYPERLINK_CLICKED)
        ShellExecuteW(nullptr, L"open", reinterpret_cast<LPCWSTR>(lp), nullptr, nullptr, SW_SHOWNORMAL);
    return S_OK;
}

int fsImageIndex(FsType t) { return static_cast<int>(t); }  // image list follows FsType order
int unallocatedImage() { return static_cast<int>(FsType::Minix) + 1; }
int extendedImage() { return static_cast<int>(FsType::Minix) + 2; }

}  // namespace

bool MainWindow::create(HINSTANCE inst, int show) {
    inst_ = inst;
    themeMode_ = loadThemeMode();
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), 0));
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) {
        log::error("RegisterClassExW(main) failed, error " + std::to_string(GetLastError()));
        return false;
    }
    if (!registerDiskMapClass(inst)) {
        log::error("RegisterClassExW(disk map) failed, error " + std::to_string(GetLastError()));
        return false;
    }

    std::wstring title = std::wstring(tr(S::AppTitle)) + L" " + WPARTED_VERSION_STRING_W + L" - " + tr(S::ReadOnlyBuild);
    hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT, kWindowClass, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, 1100, 720, nullptr, nullptr, inst, this);
    if (!hwnd_) {
        log::error("CreateWindowExW(main) failed, error " + std::to_string(GetLastError()));
        return false;
    }
    // Size the window for the actual monitor DPI.
    dpi_ = dpiForWindow(hwnd_);
    SetWindowPos(hwnd_, nullptr, 0, 0, scale(1100, dpi_), scale(720, dpi_), SWP_NOMOVE | SWP_NOZORDER);
    ShowWindow(hwnd_, show);
    UpdateWindow(hwnd_);
    startDiscovery();
    return true;
}

LRESULT CALLBACK MainWindow::proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    return self ? self->handle(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

bool MainWindow::isDark() const {
    return themeMode_ == ThemeMode::Dark || (themeMode_ == ThemeMode::System && systemPrefersDark());
}

void MainWindow::recreateFonts() {
    if (font_) DeleteObject(font_);
    font_ = createUiFont(dpi_);
    auto setFont = [this](HWND c) { if (c) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE); };
    for (HWND b : buttons_) setFont(b);
    for (HWND c : {combo_, list_, pending_, status_, statusRight_, banner_, bannerBtn_}) setFont(c);

    if (fsImages_) ImageList_Destroy(fsImages_);
    const int sz = scale(12, dpi_);
    fsImages_ = ImageList_Create(sz, sz, ILC_COLOR24, unallocatedImage() + 2, 0);
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    auto add = [&](COLORREF c) {
        HBITMAP bmp = CreateCompatibleBitmap(screen, sz, sz);
        HGDIOBJ old = SelectObject(dc, bmp);
        RECT r{0, 0, sz, sz};
        HBRUSH frameBrush = CreateSolidBrush(RGB(0x40, 0x40, 0x40));
        HBRUSH b = CreateSolidBrush(c);
        FillRect(dc, &r, b);
        FrameRect(dc, &r, frameBrush);
        DeleteObject(b);
        DeleteObject(frameBrush);
        SelectObject(dc, old);
        ImageList_Add(fsImages_, bmp, nullptr);
        DeleteObject(bmp);
    };
    for (int i = 0; i <= static_cast<int>(FsType::Minix); ++i) add(fsColor(static_cast<FsType>(i)));
    add(palette(false).unallocated);
    add(RGB(0x7D, 0xFC, 0xFE));
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    if (list_) ListView_SetImageList(list_, fsImages_, LVSIL_SMALL);
}

void MainWindow::createChildren() {
    const DWORD child = WS_CHILD | WS_VISIBLE;
    for (const auto& tb : kToolButtons) {
        HWND b = CreateWindowExW(0, L"BUTTON", tr(tb.label), child | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, hwnd_,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(tb.command)), inst_, nullptr);
        SetWindowSubclass(b, buttonSubclass, 1, 0);
        if (isPlanned(tb.command)) EnableWindow(b, FALSE);
        buttons_.push_back(b);
    }
    combo_ = CreateWindowExW(0, WC_COMBOBOXW, L"", child | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 10, 300,
                             hwnd_, reinterpret_cast<HMENU>(IDC_COMBO), inst_, nullptr);
    banner_ = CreateWindowExW(0, L"STATIC", tr(S::NotElevatedBanner), WS_CHILD | SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX,
                              0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(IDC_BANNER), inst_, nullptr);
    std::wstring restart = tr(S::MenuRestartAdmin);
    restart.erase(std::remove(restart.begin(), restart.end(), L'&'), restart.end());
    bannerBtn_ = CreateWindowExW(0, L"BUTTON", restart.c_str(), WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10,
                                 hwnd_, reinterpret_cast<HMENU>(IDC_BANNER_BTN), inst_, nullptr);
    SendMessageW(bannerBtn_, BCM_SETSHIELD, 0, TRUE);
    map_ = CreateWindowExW(0, kDiskMapClass, L"Disk map", child | WS_TABSTOP, 0, 0, 10, 10, hwnd_,
                           reinterpret_cast<HMENU>(IDC_MAP), inst_, nullptr);
    list_ = CreateWindowExW(0, WC_LISTVIEWW, L"Partitions",
                            child | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 0, 0, 10, 10, hwnd_,
                            reinterpret_cast<HMENU>(IDC_LIST), inst_, nullptr);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_SUBITEMIMAGES);
    const S cols[ColCount] = {S::ColPartition, S::ColName, S::ColFileSystem, S::ColMountPoint, S::ColLabel,
                              S::ColSize, S::ColUsed, S::ColUnused, S::ColFlags, S::ColStatus};
    const int widths[ColCount] = {110, 150, 110, 100, 110, 90, 90, 90, 120, 150};
    for (int i = 0; i < ColCount; ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.fmt = (i >= ColSize && i <= ColUnused) ? LVCFMT_RIGHT : LVCFMT_LEFT;
        c.cx = scale(widths[i], dpi_);
        c.pszText = const_cast<wchar_t*>(tr(cols[i]));
        ListView_InsertColumn(list_, i, &c);
    }
    pending_ = CreateWindowExW(0, L"LISTBOX", L"Pending operations", child | WS_TABSTOP | LBS_NOINTEGRALHEIGHT | WS_VSCROLL,
                               0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(IDC_PENDING), inst_, nullptr);
    SendMessageW(pending_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tr(S::PendingNone)));
    status_ = CreateWindowExW(0, L"STATIC", L"", child | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS | SS_NOPREFIX, 0, 0, 10, 10,
                              hwnd_, reinterpret_cast<HMENU>(IDC_STATUS), inst_, nullptr);
    statusRight_ = CreateWindowExW(0, L"STATIC", L"", child | SS_RIGHT | SS_NOPREFIX, 0, 0, 10, 10, hwnd_,
                                   reinterpret_cast<HMENU>(IDC_STATUS_RIGHT), inst_, nullptr);

    // Tooltips on the parent window rect so they also work for disabled buttons.
    tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0,
                               0, hwnd_, nullptr, inst_, nullptr);
    for (std::size_t i = 0; i < buttons_.size(); ++i) {
        TTTOOLINFOW ti{sizeof ti};
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = hwnd_;
        ti.uId = static_cast<UINT_PTR>(kToolButtons[i].command);
        ti.lpszText = const_cast<wchar_t*>(isPlanned(kToolButtons[i].command) ? tr(S::PlannedFeature) : tr(kToolButtons[i].label));
        SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    }
    SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, scale(400, dpi_));
    recreateFonts();
}

void MainWindow::buildMenu() {
    auto item = [](HMENU m, int id, S text) { AppendMenuW(m, MF_STRING | (isPlanned(id) ? MF_GRAYED : 0), id, tr(text)); };
    menu_ = CreateMenu();
    HMENU app = CreatePopupMenu();
    item(app, IDM_REFRESH, S::MenuRefresh);
    devicesMenu_ = CreatePopupMenu();
    AppendMenuW(app, MF_POPUP, reinterpret_cast<UINT_PTR>(devicesMenu_), tr(S::MenuDevices));
    AppendMenuW(app, MF_SEPARATOR, 0, nullptr);
    item(app, IDM_RESTART_ADMIN, S::MenuRestartAdmin);
    AppendMenuW(app, MF_SEPARATOR, 0, nullptr);
    item(app, IDM_QUIT, S::MenuQuit);

    HMENU edit = CreatePopupMenu();
    item(edit, IDM_UNDO, S::MenuUndo);
    item(edit, IDM_CLEAR_OPS, S::MenuClearOps);
    item(edit, IDM_APPLY_OPS, S::MenuApplyOps);

    HMENU view = CreatePopupMenu();
    item(view, IDM_DEVICE_INFO, S::MenuDeviceInfo);
    item(view, IDM_PENDING_OPS, S::MenuPendingOps);
    CheckMenuItem(view, IDM_PENDING_OPS, MF_CHECKED);
    HMENU theme = CreatePopupMenu();
    item(theme, IDM_THEME_SYSTEM, S::MenuThemeSystem);
    item(theme, IDM_THEME_LIGHT, S::MenuThemeLight);
    item(theme, IDM_THEME_DARK, S::MenuThemeDark);
    AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(theme), tr(S::MenuTheme));
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    item(view, IDM_FS_SUPPORT, S::MenuFsSupport);

    HMENU device = CreatePopupMenu();
    item(device, IDM_CREATE_TABLE, S::MenuCreateTable);
    AppendMenuW(device, MF_SEPARATOR, 0, nullptr);
    item(device, IDM_EXPORT_REPORT, S::MenuExportReport);

    partitionMenu_ = CreatePopupMenu();
    item(partitionMenu_, IDM_NEW, S::MenuNew);
    item(partitionMenu_, IDM_DELETE, S::MenuDelete);
    AppendMenuW(partitionMenu_, MF_SEPARATOR, 0, nullptr);
    item(partitionMenu_, IDM_RESIZE, S::MenuResizeMove);
    AppendMenuW(partitionMenu_, MF_SEPARATOR, 0, nullptr);
    item(partitionMenu_, IDM_COPY, S::MenuCopy);
    item(partitionMenu_, IDM_PASTE, S::MenuPaste);
    AppendMenuW(partitionMenu_, MF_SEPARATOR, 0, nullptr);
    item(partitionMenu_, IDM_FORMAT, S::MenuFormatTo);
    AppendMenuW(partitionMenu_, MF_SEPARATOR, 0, nullptr);
    item(partitionMenu_, IDM_LABEL, S::MenuLabelFs);
    item(partitionMenu_, IDM_UUID, S::MenuNewUuid);
    item(partitionMenu_, IDM_FLAGS, S::MenuManageFlags);
    item(partitionMenu_, IDM_CHECK, S::MenuCheck);
    AppendMenuW(partitionMenu_, MF_SEPARATOR, 0, nullptr);
    item(partitionMenu_, IDM_PART_INFO, S::MenuPartitionInfo);

    HMENU tools = CreatePopupMenu();
    item(tools, IDM_OPEN_LOGS, S::MenuOpenLogFolder);

    HMENU help = CreatePopupMenu();
    item(help, IDM_HELP, S::MenuHelpContents);
    AppendMenuW(help, MF_SEPARATOR, 0, nullptr);
    item(help, IDM_ABOUT, S::MenuAbout);

    AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(app), tr(S::MenuApp));
    AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), tr(S::MenuEdit));
    AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(view), tr(S::MenuView));
    AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(device), tr(S::MenuDevice));
    AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(partitionMenu_), tr(S::MenuPartition));
    AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(tools), tr(S::MenuTools));
    AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(help), tr(S::MenuHelp));
    SetMenu(hwnd_, menu_);

    ACCEL acc[] = {
        {FCONTROL | FVIRTKEY, 'R', IDM_REFRESH}, {FVIRTKEY, VK_F5, IDM_REFRESH}, {FCONTROL | FVIRTKEY, 'Q', IDM_QUIT},
        {FVIRTKEY, VK_F1, IDM_HELP},            {FCONTROL | FVIRTKEY, 'Z', IDM_UNDO},
        {FCONTROL | FVIRTKEY, VK_RETURN, IDM_APPLY_OPS}, {FVIRTKEY, VK_INSERT, IDM_NEW}, {FVIRTKEY, VK_DELETE, IDM_DELETE},
    };
    accel_ = CreateAcceleratorTableW(acc, static_cast<int>(sizeof acc / sizeof acc[0]));
}

void MainWindow::rebuildDeviceMenu() {
    while (GetMenuItemCount(devicesMenu_) > 0) DeleteMenu(devicesMenu_, 0, MF_BYPOSITION);
    SendMessageW(combo_, CB_RESETCONTENT, 0, 0);
    if (!result_) return;
    for (std::size_t i = 0; i < result_->disks.size(); ++i) {
        const auto& d = result_->disks[i];
        std::wstring text = w(d.displayName()) + L"  " + w(d.modelName()) + L"  (" + sizeText(d.sizeBytes) + L")";
        if (d.containsWindows()) text += L"  [Windows]";
        AppendMenuW(devicesMenu_, MF_STRING, IDM_DEVICE_BASE + i, text.c_str());
        SendMessageW(combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
}

void MainWindow::layout() {
    if (!list_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int pad = scale(8, dpi_), gap = scale(4, dpi_);
    const int btnH = scale(28, dpi_);
    int x = pad, y = pad;
    HDC dc = GetDC(hwnd_);
    HGDIOBJ old = SelectObject(dc, font_);
    for (std::size_t i = 0; i < buttons_.size(); ++i) {
        wchar_t text[64];
        GetWindowTextW(buttons_[i], text, 64);
        SIZE sz{};
        GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &sz);
        int bw = sz.cx + scale(20, dpi_);
        MoveWindow(buttons_[i], x, y, bw, btnH, TRUE);
        TTTOOLINFOW ti{sizeof ti};
        ti.hwnd = hwnd_;
        ti.uId = static_cast<UINT_PTR>(kToolButtons[i].command);
        ti.rect = RECT{x, y, x + bw, y + btnH};
        SendMessageW(tooltip_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
        x += bw + ((i == 0 || i == 5) ? pad * 2 : gap);  // groups: refresh | edit ops | queue
    }
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
    const int comboW = std::max(scale(260, dpi_), static_cast<int>(rc.right - x - pad));
    MoveWindow(combo_, std::max(x, static_cast<int>(rc.right - pad - std::min(comboW, scale(420, dpi_)))), y,
               std::min(comboW, scale(420, dpi_)), scale(300, dpi_), TRUE);
    y += btnH + pad;

    const bool showBanner = result_ && !result_->elevated;
    ShowWindow(banner_, showBanner ? SW_SHOW : SW_HIDE);
    ShowWindow(bannerBtn_, showBanner ? SW_SHOW : SW_HIDE);
    if (showBanner) {
        const int bh = scale(34, dpi_), bbw = scale(230, dpi_);
        MoveWindow(banner_, pad, y, rc.right - 2 * pad - bbw - gap, bh, TRUE);
        MoveWindow(bannerBtn_, rc.right - pad - bbw, y + scale(3, dpi_), bbw, bh - scale(6, dpi_), TRUE);
        y += bh + pad;
    }
    const int mapH = scale(78, dpi_);
    MoveWindow(map_, pad, y, rc.right - 2 * pad, mapH, TRUE);
    y += mapH + pad;

    const int statusH = scale(22, dpi_);
    const bool pendingVisible = IsWindowVisible(pending_) != FALSE;
    const int pendingH = pendingVisible ? scale(56, dpi_) : 0;
    const int bottom = rc.bottom - pad - statusH;
    const int listBottom = bottom - (pendingVisible ? pendingH + pad : 0);
    MoveWindow(list_, pad, y, rc.right - 2 * pad, std::max(0, listBottom - y), TRUE);
    if (pendingVisible) MoveWindow(pending_, pad, listBottom + pad, rc.right - 2 * pad, pendingH, TRUE);
    const int rightW = scale(220, dpi_);
    MoveWindow(status_, pad, bottom + scale(2, dpi_), rc.right - 3 * pad - rightW, statusH, TRUE);
    MoveWindow(statusRight_, rc.right - pad - rightW, bottom + scale(2, dpi_), rightW, statusH, TRUE);
}

void MainWindow::applyTheme() {
    const Palette& p = palette(isDark());
    for (HBRUSH* b : {&windowBrush_, &panelBrush_, &warnBrush_})
        if (*b) DeleteObject(*b);
    windowBrush_ = CreateSolidBrush(p.window);
    panelBrush_ = CreateSolidBrush(p.panel);
    warnBrush_ = CreateSolidBrush(p.warning);
    applyTitleBarTheme(hwnd_, p.dark);
    RemoveWindowSubclass(list_, listSubclass, 1);
    SetWindowSubclass(list_, listSubclass, 1, reinterpret_cast<DWORD_PTR>(&p));
    applyControlTheme(bannerBtn_, p.dark);
    applyControlTheme(combo_, p.dark, L"DarkMode_CFD");
    applyControlTheme(list_, p.dark, L"DarkMode_Explorer");
    applyControlTheme(ListView_GetHeader(list_), p.dark, L"DarkMode_ItemsView");
    applyControlTheme(pending_, p.dark);
    ListView_SetBkColor(list_, p.window);
    ListView_SetTextBkColor(list_, p.window);
    ListView_SetTextColor(list_, p.text);
    SendMessageW(map_, DM_SETVIEW, reinterpret_cast<WPARAM>(&p), reinterpret_cast<LPARAM>(view_.layout ? &view_ : nullptr));
    if (selectedRow_ >= 0) SendMessageW(map_, DM_SETSELECTION, static_cast<WPARAM>(selectedRow_), 0);
    for (int id : {IDM_THEME_SYSTEM, IDM_THEME_LIGHT, IDM_THEME_DARK})
        CheckMenuItem(menu_, id, MF_BYCOMMAND | (id - IDM_THEME_SYSTEM == static_cast<int>(themeMode_) ? MF_CHECKED : MF_UNCHECKED));
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

void MainWindow::startDiscovery() {
    if (scanning_) return;
    if (worker_.joinable()) worker_.join();
    scanning_ = true;
    EnableWindow(buttons_[0], FALSE);
    EnableMenuItem(menu_, IDM_REFRESH, MF_BYCOMMAND | MF_GRAYED);
    SetWindowTextW(status_, tr(S::Scanning));
    HWND target = hwnd_;
    worker_ = std::thread([target] {
        auto* r = new win::DiscoveryResult(win::discoverDisks());
        if (!PostMessageW(target, WM_APP_DISCOVERED, 0, reinterpret_cast<LPARAM>(r))) delete r;
    });
}

void MainWindow::onDiscovered(win::DiscoveryResult* result) {
    scanning_ = false;
    std::uint32_t previousDisk = (result_ && diskIndex_ >= 0) ? result_->disks[static_cast<std::size_t>(diskIndex_)].number : 0;
    result_.reset(result);
    fallbackLayouts_.clear();
    for (const auto& d : result_->disks) fallbackLayouts_.push_back(d.layoutAvailable ? DiskLayout{} : win::layoutFromVolumes(d));
    EnableWindow(buttons_[0], TRUE);
    EnableMenuItem(menu_, IDM_REFRESH, MF_BYCOMMAND | MF_ENABLED);
    EnableMenuItem(menu_, IDM_RESTART_ADMIN, MF_BYCOMMAND | (result_->elevated ? MF_GRAYED : MF_ENABLED));
    rebuildDeviceMenu();
    int index = result_->disks.empty() ? -1 : 0;
    for (std::size_t i = 0; i < result_->disks.size(); ++i)
        if (result_->disks[i].number == previousDisk) index = static_cast<int>(i);
    layout();
    selectDisk(index);
    updateStatus();
}

void MainWindow::selectDisk(int index) {
    diskIndex_ = index;
    selectedRow_ = -1;
    view_ = DiskView{};
    if (result_ && index >= 0 && index < static_cast<int>(result_->disks.size())) {
        const auto& d = result_->disks[static_cast<std::size_t>(index)];
        view_.disk = &d;
        view_.layout = d.layoutAvailable ? &d.layout : &fallbackLayouts_[static_cast<std::size_t>(index)];
        view_.tableRead = d.layoutAvailable;
        view_.rows = flattenSegments(*view_.layout, buildSegments(*view_.layout));
        SendMessageW(combo_, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
        for (std::size_t i = 0; i < result_->disks.size(); ++i)
            CheckMenuItem(devicesMenu_, IDM_DEVICE_BASE + static_cast<UINT>(i),
                          MF_BYCOMMAND | (static_cast<int>(i) == index ? MF_CHECKED : MF_UNCHECKED));
    }
    SendMessageW(map_, DM_SETVIEW, reinterpret_cast<WPARAM>(&palette(isDark())),
                 reinterpret_cast<LPARAM>(view_.layout ? &view_ : nullptr));
    fillPartitionList();
    selectRow(-1, false);
}

void MainWindow::fillPartitionList() {
    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list_);
    if (view_.layout) {
        const auto& l = *view_.layout;
        for (std::size_t i = 0; i < view_.rows.size(); ++i) {
            const Row& r = view_.rows[i];
            std::wstring cells[ColCount];
            int image = unallocatedImage();
            const std::wstring indent = r.depth ? L"    " : L"";
            if (r.kind == Segment::Kind::Unallocated) {
                const wchar_t* gap = view_.tableRead ? tr(S::Unallocated) : tr(S::UnknownRegion);
                cells[ColPartition] = indent + gap;
                cells[ColFs] = gap;
                cells[ColSize] = sizeText(r.sectorCount * l.sectorSize);
            } else {
                const auto& p = l.partitions[static_cast<std::size_t>(r.partitionIndex)];
                const auto* vol = win::volumeForPartition(*view_.disk, p);
                cells[ColPartition] = indent + (p.kind == PartitionKind::WholeDevice ? std::wstring(L"(whole device)")
                                                : p.number > 0 ? L"#" + std::to_wstring(p.number)
                                                               : std::wstring(tr(S::VolumeOnly)));
                cells[ColName] = w(p.name);
                if (p.kind == PartitionKind::Extended) {
                    cells[ColFs] = L"extended";
                    image = extendedImage();
                } else {
                    cells[ColFs] = w(fsTypeName(p.fs.type));
                    image = fsImageIndex(p.fs.type);
                }
                std::wstring label = w(p.fs.label);
                if (vol) {
                    for (const auto& m : vol->mountPoints) cells[ColMount] += (cells[ColMount].empty() ? L"" : L", ") + m;
                    if (!vol->label.empty()) label = vol->label;
                    if (vol->hasUsage) {
                        cells[ColUsed] = sizeText(vol->totalBytes - vol->freeBytes);
                        cells[ColUnused] = sizeText(vol->freeBytes);
                    }
                }
                cells[ColLabel] = label;
                cells[ColSize] = sizeText(p.sectorCount * l.sectorSize);
                for (const auto& f : partitionFlags(l, p)) cells[ColFlags] += (cells[ColFlags].empty() ? L"" : L", ") + w(f);
                std::wstring status;
                auto addStatus = [&status](const wchar_t* s) { status += (status.empty() ? L"" : L", ") + std::wstring(s); };
                if (vol && vol->containsWindows) addStatus(tr(S::StatusWindows));
                else if (vol && !vol->mountPoints.empty()) addStatus(tr(S::StatusMounted));
                if (isSystemCriticalType(l, p)) addStatus(tr(S::StatusSystemCritical));
                if (p.fs.type == FsType::BitLocker) addStatus(tr(S::StatusEncrypted));
                if (!view_.disk->layoutAvailable) addStatus(tr(S::StatusNotReadable));
                cells[ColStatus] = status;
            }
            LVITEMW it{};
            it.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
            it.iItem = static_cast<int>(i);
            it.pszText = const_cast<wchar_t*>(cells[ColPartition].c_str());
            it.lParam = static_cast<LPARAM>(i);
            it.iImage = -1;
            ListView_InsertItem(list_, &it);
            for (int c = 1; c < ColCount; ++c) {
                LVITEMW sub{};
                sub.mask = LVIF_TEXT | (c == ColFs ? LVIF_IMAGE : 0);
                sub.iItem = static_cast<int>(i);
                sub.iSubItem = c;
                sub.pszText = const_cast<wchar_t*>(cells[c].c_str());
                sub.iImage = image;
                SendMessageW(list_, LVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&sub));
            }
        }
    }
    SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list_, nullptr, TRUE);
}

void MainWindow::selectRow(int row, bool fromMap) {
    selectedRow_ = row;
    if (fromMap) {
        ListView_SetItemState(list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        if (row >= 0) {
            ListView_SetItemState(list_, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(list_, row, FALSE);
        }
    } else {
        SendMessageW(map_, DM_SETSELECTION, static_cast<WPARAM>(row), 0);
    }
    const bool isPartition = row >= 0 && view_.rows[static_cast<std::size_t>(row)].kind == Segment::Kind::Partition;
    EnableMenuItem(menu_, IDM_PART_INFO, MF_BYCOMMAND | (isPartition ? MF_ENABLED : MF_GRAYED));
}

void MainWindow::updateStatus() {
    if (!result_) return;
    wchar_t buf[256];
    if (result_->disks.empty())
        wcscpy_s(buf, sizeof buf / sizeof buf[0], tr(S::NoDisks));
    else
        swprintf_s(buf, sizeof buf / sizeof buf[0], tr(S::ScanDoneFmt), static_cast<int>(result_->disks.size()), static_cast<int>(result_->durationMs));
    std::wstring text = buf;
    if (view_.layout)
        for (const auto& d : view_.layout->diagnostics)
            if (d.severity != Severity::Info) text += L"  ⚠ " + w(d.message);
    SetWindowTextW(status_, text.c_str());
    SetWindowTextW(statusRight_, result_->elevated ? tr(S::ElevatedStatus) : tr(S::NotElevatedStatus));
}

void MainWindow::showPartitionInfo() {
    if (selectedRow_ < 0 || !view_.layout) return;
    const Row& r = view_.rows[static_cast<std::size_t>(selectedRow_)];
    if (r.kind != Segment::Kind::Partition) return;
    const auto& l = *view_.layout;
    const auto& p = l.partitions[static_cast<std::size_t>(r.partitionIndex)];
    const auto* vol = win::volumeForPartition(*view_.disk, p);
    const std::uint64_t ss = l.sectorSize;
    const std::uint64_t startBytes = p.firstLba * ss;
    std::wstring t;
    auto line = [&t](const wchar_t* k, const std::wstring& v) { t += std::wstring(k) + L": " + v + L"\n"; };
    line(L"File system", w(fsTypeName(p.fs.type)) + L" (detection: " + w(supportName(capabilitiesFor(p.fs.type).detect)) + L")");
    if (!p.fs.label.empty()) line(L"Label", w(p.fs.label));
    if (!p.fs.uuid.empty()) line(L"File system UUID / serial", w(p.fs.uuid));
    line(L"Partition type", w(partitionTypeDescription(l, p)));
    if (l.type == TableType::Gpt) {
        line(L"Type GUID", w(p.typeGuid.toString()));
        line(L"Partition GUID", w(p.uniqueGuid.toString()));
        line(L"Name", w(p.name));
        wchar_t attr[32];
        swprintf_s(attr, sizeof attr / sizeof attr[0], L"0x%016llX", static_cast<unsigned long long>(p.gptAttributes));
        line(L"Attributes", attr);
    }
    line(L"Kind", w(partitionKindName(p.kind)));
    line(L"First sector", std::to_wstring(p.firstLba) + L" (" + sizeText(startBytes) + L")");
    line(L"Last sector", std::to_wstring(p.lastLba()));
    line(L"Total sectors", std::to_wstring(p.sectorCount));
    line(L"Size", sizeText(p.sectorCount * ss) + L" (" + std::to_wstring(p.sectorCount * ss) + L" bytes)");
    line(L"Aligned to 1 MiB", startBytes % (1024 * 1024) == 0 ? L"yes" : L"no");
    if (view_.disk->physicalSectorSize)
        line(L"Aligned to physical sector", startBytes % view_.disk->physicalSectorSize == 0 ? L"yes" : L"no");
    std::wstring flags;
    for (const auto& f : partitionFlags(l, p)) flags += (flags.empty() ? L"" : L", ") + w(f);
    line(L"Flags", flags.empty() ? L"-" : flags);
    line(L"System critical", isSystemCriticalType(l, p) ? L"yes - protected from changes" : L"no");
    if (vol) {
        std::wstring mounts;
        for (const auto& m : vol->mountPoints) mounts += (mounts.empty() ? L"" : L", ") + m;
        line(L"Windows volume", vol->guidPath);
        line(L"Mount points", mounts.empty() ? L"-" : mounts);
        line(L"Windows file system", vol->fileSystem);
        if (vol->hasUsage) {
            line(L"Used", sizeText(vol->totalBytes - vol->freeBytes));
            line(L"Unused", sizeText(vol->freeBytes));
        }
        if (vol->containsWindows) line(L"Contains Windows", L"yes");
    }
    std::wstring head = L"#" + std::to_wstring(p.number) + L"  " + w(fsTypeName(p.fs.type)) + L"  " + sizeText(p.sectorCount * ss);
    TASKDIALOGCONFIG cfg{sizeof cfg};
    cfg.hwndParent = hwnd_;
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    cfg.pszWindowTitle = tr(S::InfoTitle);
    cfg.pszMainInstruction = head.c_str();
    cfg.pszContent = t.c_str();
    cfg.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    cfg.pszMainIcon = TD_INFORMATION_ICON;
    TaskDialogIndirect(&cfg, nullptr, nullptr, nullptr);
}

void MainWindow::showDeviceInfo() {
    if (!view_.disk) return;
    const auto& d = *view_.disk;
    std::wstring t;
    auto line = [&t](const wchar_t* k, const std::wstring& v) { t += std::wstring(k) + L": " + v + L"\n"; };
    line(L"Model", w(d.modelName()));
    line(L"Serial", d.serial.empty() ? L"-" : w(d.serial));
    line(L"Firmware revision", d.revision.empty() ? L"-" : w(d.revision));
    line(L"Path", d.path);
    line(L"Interface", w(d.busType));
    line(L"Size", sizeText(d.sizeBytes) + L" (" + std::to_wstring(d.sizeBytes) + L" bytes)");
    line(L"Logical sector size", std::to_wstring(d.logicalSectorSize) + L" bytes");
    line(L"Physical sector size", d.physicalSectorSize ? std::to_wstring(d.physicalSectorSize) + L" bytes" : L"unknown");
    line(L"Solid state (no seek penalty)", w(win::tristateName(d.solidState)));
    line(L"Removable media", d.removableMedia ? L"yes" : L"no");
    line(L"Write protected", w(win::tristateName(d.writeProtected)));
    line(L"Partition table", d.layoutAvailable ? w(tableTypeName(d.layout.type)) : L"not readable: " + w(d.accessError));
    if (d.layoutAvailable && d.layout.type == TableType::Gpt) {
        line(L"Disk GUID", w(d.layout.diskGuid.toString()));
        line(L"Usable sectors", std::to_wstring(d.layout.firstUsableLba) + L" - " + std::to_wstring(d.layout.lastUsableLba));
        line(L"GPT primary / backup header", std::wstring(d.layout.gptPrimaryValid ? L"valid" : L"INVALID") + L" / " +
                                                 (d.layout.gptBackupValid ? L"valid" : L"INVALID"));
    }
    if (d.layoutAvailable && d.layout.type == TableType::Mbr) {
        wchar_t sig[16];
        swprintf_s(sig, sizeof sig / sizeof sig[0], L"%08X", d.layout.mbrSignature);
        line(L"MBR disk signature", sig);
    }
    line(L"Contains Windows", d.containsWindows() ? L"yes" : L"no");
    if (view_.layout && !view_.layout->diagnostics.empty()) {
        t += L"\nDiagnostics:\n";
        for (const auto& diag : view_.layout->diagnostics) t += L"• " + w(diag.message) + L"\n";
    }
    std::wstring head = w(d.displayName()) + L"  " + w(d.modelName());
    TASKDIALOGCONFIG cfg{sizeof cfg};
    cfg.hwndParent = hwnd_;
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    cfg.pszWindowTitle = tr(S::DeviceInfoTitle);
    cfg.pszMainInstruction = head.c_str();
    cfg.pszContent = t.c_str();
    cfg.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    cfg.pszMainIcon = TD_INFORMATION_ICON;
    TaskDialogIndirect(&cfg, nullptr, nullptr, nullptr);
}

void MainWindow::showFsSupport() {
    std::wstring t;
    for (FsType f : allFsTypes()) {
        const auto& c = capabilitiesFor(f);
        t += w(fsTypeName(f)) + L":  " + tr(S::ColFsDetect) + L" " + w(supportName(c.detect)) + L";  " + tr(S::ColFsUsage) +
             L" " + w(supportName(c.readUsage)) + L"\n";
    }
    t += L"\n";
    t += tr(S::PlannedFeature);
    TASKDIALOGCONFIG cfg{sizeof cfg};
    cfg.hwndParent = hwnd_;
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    cfg.pszWindowTitle = tr(S::FsSupportTitle);
    cfg.pszMainInstruction = tr(S::FsSupportTitle);
    cfg.pszContent = t.c_str();
    cfg.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    TaskDialogIndirect(&cfg, nullptr, nullptr, nullptr);
}

void MainWindow::showAbout() {
    std::wstring head = std::wstring(L"WParted ") + WPARTED_VERSION_STRING_W;
    TASKDIALOGCONFIG cfg{sizeof cfg};
    cfg.hwndParent = hwnd_;
    cfg.hInstance = inst_;
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_ENABLE_HYPERLINKS | TDF_USE_HICON_MAIN;
    cfg.hMainIcon = LoadIconW(inst_, MAKEINTRESOURCEW(1));
    cfg.pszWindowTitle = tr(S::AboutTitle);
    cfg.pszMainInstruction = head.c_str();
    cfg.pszContent = tr(S::AboutText);
    cfg.pszFooter = L"<a href=\"https://git.tracht-digital.de/jtracht/WParted\">git.tracht-digital.de/jtracht/WParted</a>";
    cfg.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    cfg.pfCallback = aboutDialogCallback;
    TaskDialogIndirect(&cfg, nullptr, nullptr, nullptr);
}

void MainWindow::exportReport() {
    if (!result_) return;
    wchar_t file[MAX_PATH] = L"wparted-report.json";
    OPENFILENAMEW ofn{sizeof ofn};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"JSON (*.json)\0*.json\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"json";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return;
    const std::string json = win::discoveryToJson(*result_, WPARTED_VERSION_STRING);
    bool ok = false;
    HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);  // readonly-guard: allow (user-chosen report file)
    if (f != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ok = WriteFile(f, json.data(), static_cast<DWORD>(json.size()), &written, nullptr) && written == json.size();  // readonly-guard: allow
        CloseHandle(f);
    }
    log::info(std::string("Report export ") + (ok ? "succeeded" : "failed"));
    MessageBoxW(hwnd_, ok ? tr(S::ExportOk) : tr(S::ExportFailed), tr(S::AppTitle), ok ? MB_ICONINFORMATION : MB_ICONERROR);
}

void MainWindow::restartElevated() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SHELLEXECUTEINFOW sei{sizeof sei};
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.hwnd = hwnd_;
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) {
        log::info("Restarted elevated; closing unelevated instance");
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    }
}

void MainWindow::openLogFolder() {
    wchar_t* base = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        std::wstring dir = std::wstring(base) + L"\\WParted\\logs";
        ShellExecuteW(hwnd_, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        CoTaskMemFree(base);
    }
}

void MainWindow::openHelp() {
    const bool german = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_GERMAN;
    const wchar_t* url = german ? L"https://git.tracht-digital.de/jtracht/WParted/src/branch/main/docs/user/de/benutzerhandbuch.md"
                                : L"https://git.tracht-digital.de/jtracht/WParted/src/branch/main/docs/user/en/user-guide.md";
    ShellExecuteW(hwnd_, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

void MainWindow::showContextMenu(POINT pt) {
    if (pt.x == -1 && pt.y == -1) {  // keyboard invoked
        RECT r;
        GetWindowRect(GetFocus() ? GetFocus() : hwnd_, &r);
        pt = POINT{r.left + scale(20, dpi_), r.top + scale(20, dpi_)};
    }
    TrackPopupMenu(partitionMenu_, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
}

void MainWindow::onCommand(int id) {
    if (isPlanned(id)) return;  // defensive: planned actions have no implementation
    if (id >= IDM_DEVICE_BASE && result_ && id < IDM_DEVICE_BASE + static_cast<int>(result_->disks.size())) {
        selectDisk(id - IDM_DEVICE_BASE);
        updateStatus();
        return;
    }
    switch (id) {
        case IDM_REFRESH: startDiscovery(); break;
        case IDC_BANNER_BTN:
        case IDM_RESTART_ADMIN: restartElevated(); break;
        case IDM_QUIT: PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
        case IDM_DEVICE_INFO: showDeviceInfo(); break;
        case IDM_PENDING_OPS: {
            const bool vis = !IsWindowVisible(pending_);
            ShowWindow(pending_, vis ? SW_SHOW : SW_HIDE);
            CheckMenuItem(menu_, IDM_PENDING_OPS, MF_BYCOMMAND | (vis ? MF_CHECKED : MF_UNCHECKED));
            layout();
            break;
        }
        case IDM_THEME_SYSTEM:
        case IDM_THEME_LIGHT:
        case IDM_THEME_DARK:
            themeMode_ = static_cast<ThemeMode>(id - IDM_THEME_SYSTEM);
            saveThemeMode(themeMode_);
            applyTheme();
            break;
        case IDM_FS_SUPPORT: showFsSupport(); break;
        case IDM_EXPORT_REPORT: exportReport(); break;
        case IDM_PART_INFO: showPartitionInfo(); break;
        case IDM_OPEN_LOGS: openLogFolder(); break;
        case IDM_HELP: openHelp(); break;
        case IDM_ABOUT: showAbout(); break;
        default: break;
    }
}

LRESULT MainWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            dpi_ = dpiForWindow(hwnd_);
            buildMenu();
            createChildren();
            applyTheme();
            layout();
            return 0;
        case WM_SIZE:
            layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize = POINT{scale(640, dpi_), scale(420, dpi_)};
            return 0;
        }
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            recreateFonts();
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            for (int i = 0; i < ColCount; ++i) {
                const int widths[ColCount] = {110, 150, 110, 100, 110, 90, 90, 90, 120, 150};
                ListView_SetColumnWidth(list_, i, scale(widths[i], dpi_));
            }
            layout();
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0 && themeMode_ == ThemeMode::System)
                applyTheme();
            break;
        case WM_DRAWITEM: {
            const auto* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (di->CtlType == ODT_BUTTON) {
                drawButton(*di, palette(isDark()), dpi_);
                return TRUE;
            }
            break;
        }
        case WM_GETFONT:
            return reinterpret_cast<LRESULT>(font_);
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd_, &rc);
            FillRect(reinterpret_cast<HDC>(wp), &rc, windowBrush_);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN: {
            const Palette& p = palette(isDark());
            HDC dc = reinterpret_cast<HDC>(wp);
            HWND ctl = reinterpret_cast<HWND>(lp);
            const bool warn = ctl == banner_;
            SetTextColor(dc, (ctl == status_ || ctl == statusRight_) ? p.mutedText : p.text);
            SetBkColor(dc, warn ? p.warning : (msg == WM_CTLCOLORLISTBOX ? p.panel : p.window));
            return reinterpret_cast<LRESULT>(warn ? warnBrush_ : (msg == WM_CTLCOLORLISTBOX ? panelBrush_ : windowBrush_));
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id == IDC_COMBO && HIWORD(wp) == CBN_SELCHANGE) {
                selectDisk(static_cast<int>(SendMessageW(combo_, CB_GETCURSEL, 0, 0)));
                updateStatus();
                return 0;
            }
            onCommand(id);
            return 0;
        }
        case WM_MENUSELECT: {
            const int id = LOWORD(wp);
            if (!(HIWORD(wp) & MF_POPUP) && isPlanned(id))
                SetWindowTextW(status_, tr(S::PlannedFeature));
            else if (lp == 0)
                updateStatus();
            return 0;
        }
        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<NMHDR*>(lp);
            if (nm->hwndFrom == list_) {
                if (nm->code == LVN_ITEMCHANGED) {
                    auto* lv = reinterpret_cast<NMLISTVIEW*>(lp);
                    if ((lv->uChanged & LVIF_STATE) && (lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED))
                        selectRow(lv->iItem, false);
                } else if (nm->code == LVN_ITEMACTIVATE) {
                    showPartitionInfo();
                }
            }
            return 0;
        }
        case DMN_SELECT:
            selectRow(static_cast<int>(wp), true);
            return 0;
        case DMN_ACTIVATE:
            selectRow(static_cast<int>(wp), true);
            showPartitionInfo();
            return 0;
        case WM_CONTEXTMENU: {
            HWND src = reinterpret_cast<HWND>(wp);
            if (src == list_ || src == map_) showContextMenu(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return 0;
        }
        case WM_APP_DISCOVERED:
            onDiscovered(reinterpret_cast<win::DiscoveryResult*>(lp));
            return 0;
        case WM_DESTROY: {
            if (worker_.joinable()) worker_.join();
            MSG m;
            while (PeekMessageW(&m, hwnd_, WM_APP_DISCOVERED, WM_APP_DISCOVERED, PM_REMOVE))
                delete reinterpret_cast<win::DiscoveryResult*>(m.lParam);
            for (HBRUSH b : {windowBrush_, panelBrush_, warnBrush_})
                if (b) DeleteObject(b);
            if (font_) DeleteObject(font_);
            if (fsImages_) ImageList_Destroy(fsImages_);
            if (accel_) DestroyAcceleratorTable(accel_);
            PostQuitMessage(0);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace wparted::ui
