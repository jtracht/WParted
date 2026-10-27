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

#include "commands.hpp"
#include "menubar.hpp"
#include "strings.hpp"
#include "wparted/core/json.hpp"
#include "wparted/core/log.hpp"
#include "wparted/win/report.hpp"
#include "wparted_version.h"

namespace wparted::ui {

namespace {

constexpr wchar_t kWindowClass[] = L"WPartedMainWindow";
constexpr UINT WM_APP_DISCOVERED = WM_APP + 50;

// Every command now has a (planning) implementation; availability depends on
// the selection and is computed in MainWindow::updateCommandState().
bool isPlanned(int) { return false; }

struct ToolButton {
    int command;
    S label;
};
const ToolButton kToolButtons[] = {
    {IDM_REFRESH, S::BtnRefresh}, {IDM_NEW, S::BtnNew},     {IDM_DELETE, S::BtnDelete}, {IDM_RESIZE, S::BtnResizeMove},
    {IDM_COPY, S::BtnCopy},       {IDM_PASTE, S::BtnPaste}, {IDM_UNDO, S::BtnUndo},     {IDM_APPLY_OPS, S::BtnApply},
};
constexpr std::size_t kApplyIndex = 7;  // laid out separately at the right edge

wchar_t commandGlyph(int id) {
    switch (id) {
        case IDM_REFRESH: return glyph::Refresh;
        case IDM_NEW: return glyph::Add;
        case IDM_DELETE: return glyph::Delete;
        case IDM_RESIZE: return glyph::Resize;
        case IDM_COPY: return glyph::Copy;
        case IDM_PASTE: return glyph::Paste;
        case IDM_UNDO: return glyph::Undo;
        case IDM_APPLY_OPS: return glyph::Apply;
        default: return 0;
    }
}

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

// Command bar button: Fluent "subtle" style (transparent until hovered),
// the primary action (Apply) uses the accent colour.
void drawButton(const DRAWITEMSTRUCT& di, const Palette& p, UINT dpi, HFONT textFont, HFONT iconFont) {
    const bool disabled = (di.itemState & ODS_DISABLED) != 0;
    const bool pressed = (di.itemState & ODS_SELECTED) != 0;
    const bool hot = !disabled && GetPropW(di.hwndItem, kHotProp) != nullptr;
    const int id = GetDlgCtrlID(di.hwndItem);
    const bool primary = id == IDM_APPLY_OPS;
    fillRect(di.hDC, di.rcItem, p.window);
    const int radius = scale(8, dpi);
    COLORREF fg = disabled ? p.disabledText : p.text;
    if (primary) {
        const COLORREF fill = disabled ? blend(p.window, p.border, 70) : pressed ? blend(p.accent, p.window, 25)
                              : hot ? blend(p.accent, p.window, 12) : p.accent;
        fillRounded(di.hDC, di.rcItem, radius, fill, fill);
        fg = disabled ? p.disabledText : p.onAccent;
    } else if (pressed || hot) {
        fillRounded(di.hDC, di.rcItem, radius, pressed ? p.pressed : p.subtle, pressed ? p.pressed : p.subtle);
    }
    wchar_t text[64];
    GetWindowTextW(di.hwndItem, text, 64);
    SetBkMode(di.hDC, TRANSPARENT);
    SetTextColor(di.hDC, fg);
    RECT r = di.rcItem;
    const wchar_t g = commandGlyph(id);
    HGDIOBJ old = SelectObject(di.hDC, textFont);
    SIZE ts{};
    GetTextExtentPoint32W(di.hDC, text, static_cast<int>(wcslen(text)), &ts);
    const int iconW = (iconFont && g) ? scale(16, dpi) + scale(8, dpi) : 0;
    int x = r.left + (r.right - r.left - iconW - ts.cx) / 2;
    if (iconW) {
        RECT ir{x, r.top, x + scale(16, dpi), r.bottom};
        SelectObject(di.hDC, iconFont);
        if (!primary && !disabled) SetTextColor(di.hDC, p.accent);
        DrawTextW(di.hDC, &g, 1, &ir, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SetTextColor(di.hDC, fg);
        x += iconW;
    }
    RECT tr2{x, r.top, r.right, r.bottom};
    SelectObject(di.hDC, textFont);
    DrawTextW(di.hDC, text, -1, &tr2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(di.hDC, old);
    if ((di.itemState & ODS_FOCUS) && !(di.itemState & ODS_NOFOCUSRECT)) {
        RECT f = di.rcItem;
        InflateRect(&f, -scale(2, dpi), -scale(2, dpi));
        DrawFocusRect(di.hDC, &f);
    }
}

// Explicit CALLBACK (__stdcall on x86): a lambda would only convert with MSVC.
HRESULT CALLBACK aboutDialogCallback(HWND, UINT n, WPARAM, LPARAM lp, LONG_PTR) {
    if (n == TDN_HYPERLINK_CLICKED)
        ShellExecuteW(nullptr, L"open", reinterpret_cast<LPCWSTR>(lp), nullptr, nullptr, SW_SHOWNORMAL);
    return S_OK;
}


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
    if (!registerInspectorClass(inst)) {
        log::error("RegisterClassExW(inspector) failed, error " + std::to_string(GetLastError()));
        return false;
    }
    refreshAccentColor();
    refreshHighContrast();
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
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int ww = std::min<int>(scale(1280, dpi_), work.right - work.left - 40);
    const int wh = std::min<int>(scale(840, dpi_), work.bottom - work.top - 40);
    SetWindowPos(hwnd_, nullptr, work.left + (work.right - work.left - ww) / 2, work.top + (work.bottom - work.top - wh) / 2,
                 ww, wh, SWP_NOZORDER);
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
    for (HFONT* f : {&font_, &sectionFont_, &iconFont_, &captionFont_})
        if (*f) DeleteObject(*f);
    font_ = createUiFont(dpi_, FontRole::Body);
    sectionFont_ = createUiFont(dpi_, FontRole::BodyStrong);
    captionFont_ = createUiFont(dpi_, FontRole::Caption);
    iconFont_ = createIconFont(dpi_, 16);
    auto setFont = [this](HWND c) { if (c) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE); };
    for (HWND b : buttons_) setFont(b);
    for (HWND c : {combo_, list_, pending_, status_, statusRight_, banner_, bannerBtn_}) setFont(c);
    SendMessageW(status_, WM_SETFONT, reinterpret_cast<WPARAM>(captionFont_), TRUE);
    SendMessageW(statusRight_, WM_SETFONT, reinterpret_cast<WPARAM>(captionFont_), TRUE);
    // The image list only sets a comfortable row height (Fluent list rows are 32-40 px).
    if (fsImages_) ImageList_Destroy(fsImages_);
    fsImages_ = ImageList_Create(1, scale(34, dpi_), ILC_COLOR24, 1, 0);
    if (list_) ListView_SetImageList(list_, fsImages_, LVSIL_SMALL);
    if (pending_) SendMessageW(pending_, LB_SETITEMHEIGHT, 0, scale(28, dpi_));
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
    banner_ = CreateWindowExW(0, L"STATIC", tr(S::NotElevatedBanner), WS_CHILD | WS_CLIPSIBLINGS | SS_OWNERDRAW | SS_NOPREFIX,
                              0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(IDC_BANNER), inst_, nullptr);
    std::wstring restart = tr(S::MenuRestartAdmin);
    restart.erase(std::remove(restart.begin(), restart.end(), L'&'), restart.end());
    bannerBtn_ = CreateWindowExW(0, L"BUTTON", restart.c_str(), WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10,
                                 hwnd_, reinterpret_cast<HMENU>(IDC_BANNER_BTN), inst_, nullptr);
    SendMessageW(bannerBtn_, BCM_SETSHIELD, 0, TRUE);
    map_ = CreateWindowExW(0, kDiskMapClass, L"Disk map", child | WS_TABSTOP, 0, 0, 10, 10, hwnd_,
                           reinterpret_cast<HMENU>(IDC_MAP), inst_, nullptr);
    list_ = CreateWindowExW(0, WC_LISTVIEWW, tr(S::SectionPartitions),
                            child | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER, 0, 0, 10, 10, hwnd_,
                            reinterpret_cast<HMENU>(IDC_LIST), inst_, nullptr);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const S cols[ColCount] = {S::ColPartition, S::ColName, S::ColFileSystem, S::ColMountPoint, S::ColLabel,
                              S::ColSize, S::ColUsed, S::ColUnused, S::ColFlags, S::ColStatus};
    const int widths[ColCount] = {64, 130, 96, 80, 100, 84, 80, 80, 100, 130};
    for (int i = 0; i < ColCount; ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.fmt = (i >= ColSize && i <= ColUnused) ? LVCFMT_RIGHT : LVCFMT_LEFT;
        c.cx = scale(widths[i], dpi_);
        c.pszText = const_cast<wchar_t*>(tr(cols[i]));
        ListView_InsertColumn(list_, i, &c);
    }
    pending_ = CreateWindowExW(0, L"LISTBOX", L"Pending operations",
                               child | WS_TABSTOP | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | WS_VSCROLL,
                               0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(IDC_PENDING), inst_, nullptr);
    SendMessageW(pending_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tr(S::PendingNone)));
    inspector_ = CreateWindowExW(0, kInspectorClass, L"Details", child, 0, 0, 10, 10, hwnd_, nullptr, inst_, nullptr);
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
    item(app, IDM_OPEN_IMAGE, S::MenuOpenImage);
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
    AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
    item(edit, IDM_SAVE_PLAN, S::MenuSavePlan);

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
    item(device, IDM_RESCUE, S::MenuRescue);

    partitionMenu_ = CreatePopupMenu();
    item(partitionMenu_, IDM_NEW, S::MenuNew);
    item(partitionMenu_, IDM_DELETE, S::MenuDelete);
    AppendMenuW(partitionMenu_, MF_SEPARATOR, 0, nullptr);
    item(partitionMenu_, IDM_RESIZE, S::MenuResizeMove);
    AppendMenuW(partitionMenu_, MF_SEPARATOR, 0, nullptr);
    item(partitionMenu_, IDM_COPY, S::MenuCopy);
    item(partitionMenu_, IDM_PASTE, S::MenuPaste);
    AppendMenuW(partitionMenu_, MF_SEPARATOR, 0, nullptr);
    formatMenu_ = CreatePopupMenu();
    for (int i = 0; i < kFormatChoiceCount; ++i) {
        const FsType fs = kFormatChoices[i];
        AppendMenuW(formatMenu_, MF_STRING, static_cast<UINT_PTR>(IDM_FORMAT_BASE + i),
                    fs == FsType::Unknown ? tr(S::FormatCleared) : w(fsTypeName(fs)).c_str());
    }
    AppendMenuW(partitionMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(formatMenu_), tr(S::MenuFormatTo));
    AppendMenuW(partitionMenu_, MF_SEPARATOR, 0, nullptr);
    item(partitionMenu_, IDM_NAME, S::MenuName);
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
        {FCONTROL | FVIRTKEY, 'O', IDM_OPEN_IMAGE}, {FCONTROL | FVIRTKEY, 'S', IDM_SAVE_PLAN},
        {FCONTROL | FVIRTKEY, 'C', IDM_COPY}, {FCONTROL | FVIRTKEY, 'V', IDM_PASTE},
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

bool MainWindow::showBanner() const {
    const win::PhysicalDisk* d = currentDisk();
    return result_ && !result_->elevated && !(d && d->busType == "Image file");
}

void MainWindow::layout() {
    if (!list_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    // Fluent spacing: 4 px grid, 16 px page margins, 8/12 px between blocks.
    const int pad = scale(16, dpi_), gap = scale(8, dpi_), tight = scale(4, dpi_);
    const int btnH = scale(36, dpi_);
    int x = pad, y = scale(10, dpi_);
    HDC dc = GetDC(hwnd_);
    HGDIOBJ old = SelectObject(dc, font_);
    auto buttonWidth = [&](std::size_t i) {
        wchar_t text[64];
        GetWindowTextW(buttons_[i], text, 64);
        SIZE sz{};
        GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &sz);
        return sz.cx + scale(24, dpi_) + (iconFont_ ? scale(24, dpi_) : 0);
    };
    auto placeTool = [&](std::size_t i, int bx, int bw) {
        MoveWindow(buttons_[i], bx, y, bw, btnH, TRUE);
        TTTOOLINFOW ti{sizeof ti};
        ti.hwnd = hwnd_;
        ti.uId = static_cast<UINT_PTR>(kToolButtons[i].command);
        ti.rect = RECT{bx, y, bx + bw, y + btnH};
        SendMessageW(tooltip_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
    };
    for (std::size_t i = 0; i < buttons_.size(); ++i) {
        if (i == kApplyIndex) continue;
        const int bw = buttonWidth(i);
        placeTool(i, x, bw);
        x += bw + ((i == 0 || i == 5) ? scale(16, dpi_) : tight);  // groups: refresh | edit | undo
    }
    const int applyW = buttonWidth(kApplyIndex) + scale(8, dpi_);
    placeTool(kApplyIndex, rc.right - pad - applyW, applyW);
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
    const int comboW = std::min<int>(scale(380, dpi_), std::max<int>(scale(200, dpi_), rc.right - pad - applyW - gap - x - gap));
    MoveWindow(combo_, rc.right - pad - applyW - gap - comboW, y + scale(4, dpi_), comboW, scale(300, dpi_), TRUE);
    y += btnH + scale(10, dpi_);

    const bool banner = showBanner();
    ShowWindow(banner_, banner ? SW_SHOW : SW_HIDE);
    ShowWindow(bannerBtn_, banner ? SW_SHOW : SW_HIDE);
    if (banner) {
        const int bh = scale(44, dpi_), bbw = scale(220, dpi_);
        MoveWindow(banner_, pad, y, rc.right - 2 * pad, bh, TRUE);
        MoveWindow(bannerBtn_, rc.right - pad - bbw - scale(8, dpi_), y + scale(7, dpi_), bbw, bh - scale(14, dpi_), TRUE);
        SetWindowPos(bannerBtn_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        y += bh + gap;
    }
    const int mapH = scale(136, dpi_);
    MoveWindow(map_, pad, y, rc.right - 2 * pad, mapH, TRUE);
    y += mapH + scale(12, dpi_);

    const int statusH = scale(24, dpi_);
    const bool pendingVisible = IsWindowVisible(pending_) != FALSE;
    const int pendingH = pendingVisible ? scale(140, dpi_) : 0;
    const int bottom = rc.bottom - statusH - tight;
    const int sectionH = scale(36, dpi_);
    const int inspectorW = std::min<int>(scale(320, dpi_), (rc.right - 2 * pad) / 3);
    const int listBottom = bottom - (pendingVisible ? pendingH + scale(12, dpi_) : 0);
    listCard_ = RECT{pad, y, rc.right - pad - inspectorW - scale(12, dpi_), listBottom};
    MoveWindow(list_, listCard_.left + 1, listCard_.top + sectionH, listCard_.right - listCard_.left - 2,
               std::max(0, static_cast<int>(listCard_.bottom - listCard_.top) - sectionH - scale(6, dpi_)), TRUE);
    MoveWindow(inspector_, rc.right - pad - inspectorW, y, inspectorW, std::max(0, listBottom - y), TRUE);
    {  // the Status column takes the remaining width (no horizontal scrolling)
        int used = 0;
        for (int c = 0; c < ColStatus; ++c) used += ListView_GetColumnWidth(list_, c);
        const int avail = static_cast<int>(listCard_.right - listCard_.left) - 2 - GetSystemMetrics(SM_CXVSCROLL) - used;
        ListView_SetColumnWidth(list_, ColStatus, std::max(scale(100, dpi_), avail));
    }
    if (pendingVisible) {
        pendingCard_ = RECT{pad, listBottom + scale(12, dpi_), rc.right - pad, bottom};
        MoveWindow(pending_, pendingCard_.left + scale(8, dpi_), pendingCard_.top + sectionH,
                   pendingCard_.right - pendingCard_.left - scale(16, dpi_),
                   std::max(0, static_cast<int>(pendingCard_.bottom - pendingCard_.top) - sectionH - scale(8, dpi_)), TRUE);
    } else {
        pendingCard_ = RECT{};
    }
    const int rightW = scale(240, dpi_);
    MoveWindow(status_, pad, bottom + scale(2, dpi_), rc.right - 3 * pad - rightW, statusH, TRUE);
    MoveWindow(statusRight_, rc.right - pad - rightW, bottom + scale(2, dpi_), rightW, statusH, TRUE);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void MainWindow::paintBackground(HDC dc) {
    const Palette& p = palette(isDark());
    RECT rc;
    GetClientRect(hwnd_, &rc);
    fillRect(dc, rc, p.window);
    const int radius = scale(8, dpi_);
    SetBkMode(dc, TRANSPARENT);
    auto section = [&](const RECT& card, const std::wstring& title) {
        if (card.right <= card.left) return;
        fillRounded(dc, card, radius, p.card, p.border);
        RECT t{card.left + scale(16, dpi_), card.top, card.right - scale(16, dpi_), card.top + scale(36, dpi_)};
        HGDIOBJ old = SelectObject(dc, sectionFont_);
        SetTextColor(dc, p.text);
        DrawTextW(dc, title.c_str(), -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old);
    };
    section(listCard_, tr(S::SectionPartitions));
    std::wstring pendingTitle = tr(S::SectionPending);
    const win::PhysicalDisk* d = currentDisk();
    auto it = d ? queues_.find(d->path) : queues_.end();
    if (it != queues_.end() && !it->second.empty()) pendingTitle += L"  (" + std::to_wstring(it->second.operations().size()) + L")";
    section(pendingCard_, pendingTitle);
}

void MainWindow::drawPendingItem(const DRAWITEMSTRUCT& di) {
    const Palette& p = palette(isDark());
    if (di.itemID == static_cast<UINT>(-1)) return;
    const bool selected = (di.itemState & ODS_SELECTED) != 0;
    fillRect(di.hDC, di.rcItem, selected ? p.selection : p.card);
    wchar_t text[1024] = L"";
    SendMessageW(di.hwndItem, LB_GETTEXT, di.itemID, reinterpret_cast<LPARAM>(text));
    const LRESULT kind = SendMessageW(di.hwndItem, LB_GETITEMDATA, di.itemID, 0);
    // 1-4: risk low..critical, 10 warning, 11 error, 0 info
    COLORREF dot = p.mutedText;
    if (kind == 1) dot = p.success;
    else if (kind == 2) dot = p.accent;
    else if (kind == 3) dot = p.caution;
    else if (kind == 4 || kind == 11) dot = p.danger;
    else if (kind == 10) dot = p.caution;
    RECT r = di.rcItem;
    const int cy = (r.top + r.bottom) / 2;
    const int x = r.left + scale(kind >= 10 ? 28 : 8, dpi_);
    SetBkMode(di.hDC, TRANSPARENT);
    HGDIOBJ old = SelectObject(di.hDC, font_);
    if (kind >= 10 && iconFont_) {
        const wchar_t g = kind == 11 ? glyph::Error : glyph::Warning;
        RECT ir{x, r.top, x + scale(18, dpi_), r.bottom};
        SelectObject(di.hDC, iconFont_);
        SetTextColor(di.hDC, dot);
        DrawTextW(di.hDC, &g, 1, &ir, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(di.hDC, font_);
    } else if (kind > 0 && kind < 10) {
        RECT dr{x, cy - scale(4, dpi_), x + scale(8, dpi_), cy + scale(4, dpi_)};
        fillRounded(di.hDC, dr, scale(8, dpi_), dot, dot);
    }
    RECT tr2{x + scale(kind > 0 ? 22 : 0, dpi_), r.top, r.right - scale(8, dpi_), r.bottom};
    SetTextColor(di.hDC, selected && p.highContrast ? p.onAccent : kind >= 10 ? dot : p.text);
    DrawTextW(di.hDC, text, -1, &tr2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(di.hDC, old);
}

LRESULT MainWindow::listCustomDraw(NMLVCUSTOMDRAW* cd) {
    switch (cd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT:
            cd->nmcd.uItemState &= ~static_cast<UINT>(CDIS_SELECTED | CDIS_FOCUS | CDIS_HOT);
            return CDRF_NOTIFYPOSTPAINT | CDRF_NOTIFYSUBITEMDRAW;
        case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
            // Selection is drawn as a subtle accent tint (Fluent list style)
            // instead of the theme's grey/blue highlight.
            const Palette& p = palette(isDark());
            const int item = static_cast<int>(cd->nmcd.dwItemSpec);
            const bool selected = ListView_GetItemState(list_, item, LVIS_SELECTED) != 0;
            cd->nmcd.uItemState &= ~static_cast<UINT>(CDIS_SELECTED | CDIS_FOCUS | CDIS_HOT);
            const int col = cd->iSubItem;
            cd->clrTextBk = selected ? p.selection : p.card;
            cd->clrText = (col == ColUsed || col == ColUnused || col == ColFlags || col == ColMount) ? p.mutedText : p.text;
            if (selected && p.highContrast) cd->clrText = p.onAccent;  // HighlightText on Highlight
            return CDRF_DODEFAULT;
        }
        case CDDS_ITEMPOSTPAINT: {
            // File system as a coloured pill (the cell text stays for screen readers).
            const int item = static_cast<int>(cd->nmcd.dwItemSpec);
            if (item < 0 || item >= static_cast<int>(view_.rows.size()) || !view_.layout) return CDRF_DODEFAULT;
            const Palette& p = palette(isDark());
            const Row& row = view_.rows[static_cast<std::size_t>(item)];
            RECT cell{};
            ListView_GetSubItemRect(list_, item, ColFs, LVIR_BOUNDS, &cell);
            const bool selected = ListView_GetItemState(list_, item, LVIS_SELECTED) != 0;
            RECT bg = cell;
            fillRect(cd->nmcd.hdc, bg, selected ? p.selection : p.card);
            std::wstring label;
            COLORREF color = p.unallocated;
            if (row.kind == Segment::Kind::Unallocated) {
                label = view_.tableRead ? tr(S::Unallocated) : tr(S::UnknownRegion);
            } else {
                const auto& part = view_.layout->partitions[static_cast<std::size_t>(row.partitionIndex)];
                label = part.kind == PartitionKind::Extended ? std::wstring(L"extended") : w(fsTypeName(part.fs.type));
                color = part.kind == PartitionKind::Extended ? p.mutedText : fsColor(part.fs.type);
            }
            RECT pill = cell;
            pill.left += scale(6, dpi_);
            pill.right -= scale(4, dpi_);
            InflateRect(&pill, 0, -scale(5, dpi_));
            drawPill(cd->nmcd.hdc, pill, blend(selected ? p.selection : p.card, color, p.dark ? 40 : 22), p.text,
                     label.c_str(), captionFont_, dpi_);
            return CDRF_DODEFAULT;
        }
        default:
            return CDRF_DODEFAULT;
    }
}

void MainWindow::applyTheme() {
    const Palette& p = palette(isDark());
    for (HBRUSH* b : {&windowBrush_, &panelBrush_, &warnBrush_})
        if (*b) DeleteObject(*b);
    windowBrush_ = CreateSolidBrush(p.window);
    panelBrush_ = CreateSolidBrush(p.card);
    warnBrush_ = CreateSolidBrush(p.warning);
    applyTitleBarTheme(hwnd_, p.dark);
    applyMenuTheme(p.dark);
    DrawMenuBar(hwnd_);
    RemoveWindowSubclass(list_, listSubclass, 1);
    SetWindowSubclass(list_, listSubclass, 1, reinterpret_cast<DWORD_PTR>(&p));
    applyControlTheme(bannerBtn_, p.dark);
    applyControlTheme(combo_, p.dark, L"DarkMode_CFD");
    applyControlTheme(list_, p.dark, L"DarkMode_Explorer");
    applyControlTheme(ListView_GetHeader(list_), p.dark, L"DarkMode_ItemsView");
    applyControlTheme(pending_, p.dark);
    applyControlTheme(bannerBtn_, p.dark);
    updateInspector();
    ListView_SetBkColor(list_, p.card);
    ListView_SetTextBkColor(list_, p.card);
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
    for (const auto& img : images_) result_->disks.push_back(img);  // opened disk images survive a refresh
    // A plan is only kept if the disk still has exactly the layout it was made for.
    for (auto it = queues_.begin(); it != queues_.end();) {
        bool keep = false;
        for (const auto& d : result_->disks)
            if (d.path == it->first && d.layoutAvailable && layoutToJson(d.layout) == layoutToJson(it->second.original()))
                keep = true;
        if (!keep) log::warn("Discarded pending operations for " + win::narrow(it->first) + ": the disk changed");
        it = keep ? std::next(it) : queues_.erase(it);
    }
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
    if (!pendingImage_.empty()) {
        std::wstring img;
        img.swap(pendingImage_);
        openImage(img);
    }
}

void MainWindow::selectDisk(int index) {
    diskIndex_ = index;
    selectedRow_ = -1;
    if (result_ && index >= 0 && index < static_cast<int>(result_->disks.size())) {
        SendMessageW(combo_, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
        for (std::size_t i = 0; i < result_->disks.size(); ++i)
            CheckMenuItem(devicesMenu_, IDM_DEVICE_BASE + static_cast<UINT>(i),
                          MF_BYCOMMAND | (static_cast<int>(i) == index ? MF_CHECKED : MF_UNCHECKED));
    }
    refreshView();
    layout();  // the info bar depends on the device (hidden for disk images)
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
            const std::wstring indent = r.depth ? L"    " : L"";
            if (r.kind == Segment::Kind::Unallocated) {
                const wchar_t* gap = view_.tableRead ? tr(S::Unallocated) : tr(S::UnknownRegion);
                cells[ColPartition] = indent + gap;
                cells[ColFs] = gap;
                cells[ColSize] = sizeText(r.sectorCount * l.sectorSize);
            } else {
                const auto& p = l.partitions[static_cast<std::size_t>(r.partitionIndex)];
                const auto* vol = viewVolume(view_, p);
                cells[ColPartition] = indent + (p.kind == PartitionKind::WholeDevice ? std::wstring(L"(whole device)")
                                                : p.number > 0 ? L"#" + std::to_wstring(p.number)
                                                               : std::wstring(tr(S::VolumeOnly)));
                cells[ColName] = w(p.name);
                cells[ColFs] = p.kind == PartitionKind::Extended ? std::wstring(L"extended") : w(fsTypeName(p.fs.type));
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
            it.mask = LVIF_TEXT | LVIF_PARAM;
            it.iItem = static_cast<int>(i);
            it.pszText = const_cast<wchar_t*>(cells[ColPartition].c_str());
            it.lParam = static_cast<LPARAM>(i);
            ListView_InsertItem(list_, &it);
            for (int c = 1; c < ColCount; ++c) {
                LVITEMW sub{};
                sub.mask = LVIF_TEXT;
                sub.iItem = static_cast<int>(i);
                sub.iSubItem = c;
                sub.pszText = const_cast<wchar_t*>(cells[c].c_str());
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
    updateCommandState();
    updateInspector();
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
            if (d.severity != Severity::Info) text += L"  ⚠ " + diagnosticText(d);
    SetWindowTextW(status_, text.c_str());
    SetWindowTextW(statusRight_, result_->elevated ? tr(S::ElevatedStatus) : tr(S::NotElevatedStatus));
}

void MainWindow::showPartitionInfo() {
    if (selectedRow_ < 0 || !view_.layout) return;
    const Row& r = view_.rows[static_cast<std::size_t>(selectedRow_)];
    if (r.kind != Segment::Kind::Partition) return;
    const auto& l = *view_.layout;
    const auto& p = l.partitions[static_cast<std::size_t>(r.partitionIndex)];
    const auto* vol = viewVolume(view_, p);
    const std::uint64_t ss = l.sectorSize;
    const std::uint64_t startBytes = p.firstLba * ss;
    std::wstring t;
    auto line = [&t](const wchar_t* k, const std::wstring& v) { t += std::wstring(k) + L": " + v + L"\n"; };
    line(tr(S::InfoFileSystem), formatText(S::ValDetection, {w(fsTypeName(p.fs.type)), supportText(capabilitiesFor(p.fs.type).detect)}));
    if (!p.fs.label.empty()) line(tr(S::InfoLabel), w(p.fs.label));
    if (!p.fs.uuid.empty()) line(tr(S::InfoFileSystemUuidSerial), w(p.fs.uuid));
    line(tr(S::InfoPartitionType), w(partitionTypeDescription(l, p)));
    if (l.type == TableType::Gpt) {
        line(tr(S::InfoTypeGuid), w(p.typeGuid.toString()));
        line(tr(S::InfoPartitionGuid), w(p.uniqueGuid.toString()));
        line(tr(S::InfoName), w(p.name));
        wchar_t attr[32];
        swprintf_s(attr, sizeof attr / sizeof attr[0], L"0x%016llX", static_cast<unsigned long long>(p.gptAttributes));
        line(tr(S::InfoAttributes), attr);
    }
    line(tr(S::InfoKind), tr(p.kind == PartitionKind::Logical    ? S::KindLogical
                             : p.kind == PartitionKind::Extended ? S::KindExtended
                                                                 : S::KindPrimary));
    line(tr(S::InfoFirstSector), std::to_wstring(p.firstLba) + L" (" + sizeText(startBytes) + L")");
    line(tr(S::InfoLastSector), std::to_wstring(p.lastLba()));
    line(tr(S::InfoTotalSectors), std::to_wstring(p.sectorCount));
    line(tr(S::InfoSize), sizeText(p.sectorCount * ss) + L" (" + formatText(S::ValBytes, {std::to_wstring(p.sectorCount * ss)}) + L")");
    line(tr(S::InfoAlignedTo1Mib), tr(startBytes % (1024 * 1024) == 0 ? S::ValYes : S::ValNo));
    if (view_.disk->physicalSectorSize)
        line(tr(S::InfoAlignedToPhysicalSector), tr(startBytes % view_.disk->physicalSectorSize == 0 ? S::ValYes : S::ValNo));
    std::wstring flags;
    for (const auto& f : partitionFlags(l, p)) flags += (flags.empty() ? L"" : L", ") + w(f);
    line(tr(S::InfoFlags), flags.empty() ? L"-" : flags);
    line(tr(S::InfoSystemCritical), tr(isSystemCriticalType(l, p) ? S::ValProtected : S::ValNo));
    if (vol) {
        std::wstring mounts;
        for (const auto& m : vol->mountPoints) mounts += (mounts.empty() ? L"" : L", ") + m;
        line(tr(S::InfoWindowsVolume), vol->guidPath);
        line(tr(S::InfoMountPoints), mounts.empty() ? L"-" : mounts);
        line(tr(S::InfoWindowsFileSystem), vol->fileSystem);
        if (vol->hasUsage) {
            line(tr(S::InfoUsed), sizeText(vol->totalBytes - vol->freeBytes));
            line(tr(S::InfoUnused), sizeText(vol->freeBytes));
        }
        if (vol->containsWindows) line(tr(S::InfoContainsWindows), tr(S::ValYes));
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
    line(tr(S::InfoModel), w(d.modelName()));
    line(tr(S::InfoSerial), d.serial.empty() ? L"-" : w(d.serial));
    line(tr(S::InfoFirmwareRevision), d.revision.empty() ? L"-" : w(d.revision));
    line(tr(S::InfoPath), d.path);
    line(tr(S::InfoInterface), w(d.busType));
    line(tr(S::InfoSize), sizeText(d.sizeBytes) + L" (" + formatText(S::ValBytes, {std::to_wstring(d.sizeBytes)}) + L")");
    line(tr(S::InfoLogicalSectorSize), formatText(S::ValBytes, {std::to_wstring(d.logicalSectorSize)}));
    line(tr(S::InfoPhysicalSectorSize), d.physicalSectorSize ? formatText(S::ValBytes, {std::to_wstring(d.physicalSectorSize)}) : std::wstring(tr(S::ValUnknown)));
    line(tr(S::InfoSolidStateNoSeekPenalty), tristateText(d.solidState));
    line(tr(S::InfoRemovableMedia), tr(d.removableMedia ? S::ValYes : S::ValNo));
    line(tr(S::InfoWriteProtected), tristateText(d.writeProtected));
    line(tr(S::InfoPartitionTable), d.layoutAvailable ? w(tableTypeName(d.layout.type)) : formatText(S::ValNotReadable, {w(d.accessError)}));
    if (d.layoutAvailable && d.layout.type == TableType::Gpt) {
        line(tr(S::InfoDiskGuid), w(d.layout.diskGuid.toString()));
        line(tr(S::InfoUsableSectors), std::to_wstring(d.layout.firstUsableLba) + L" - " + std::to_wstring(d.layout.lastUsableLba));
        line(tr(S::InfoGptPrimaryBackupHeader), std::wstring(tr(d.layout.gptPrimaryValid ? S::ValValid : S::ValInvalid)) + L" / " +
                                                 tr(d.layout.gptBackupValid ? S::ValValid : S::ValInvalid));
    }
    if (d.layoutAvailable && d.layout.type == TableType::Mbr) {
        wchar_t sig[16];
        swprintf_s(sig, sizeof sig / sizeof sig[0], L"%08X", d.layout.mbrSignature);
        line(tr(S::InfoMbrDiskSignature), sig);
    }
    line(tr(S::InfoContainsWindows), tr(d.containsWindows() ? S::ValYes : S::ValNo));
    if (view_.layout && !view_.layout->diagnostics.empty()) {
        t += L"\n" + std::wstring(tr(S::InfoDiagnostics)) + L":\n";
        for (const auto& diag : view_.layout->diagnostics) t += L"• " + diagnosticText(diag) + L"\n";
    }
    // Images are named "Image <file>" already; avoid "Image  Image <file>".
    const std::string model = d.modelName(), name = d.displayName();
    std::wstring head = model.compare(0, name.size(), name) == 0 ? w(model) : w(name) + L"  " + w(model);
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
        t += w(fsTypeName(f)) + L":  " + tr(S::ColFsDetect) + L" " + supportText(c.detect) + L";  " + tr(S::ColFsUsage) +
             L" " + supportText(c.readUsage) + L"\n";
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
    if (id >= IDM_FORMAT_BASE && id < IDM_FORMAT_BASE + kFormatChoiceCount) {
        planFormat(kFormatChoices[id - IDM_FORMAT_BASE]);
        return;
    }
    if (id >= IDM_DEVICE_BASE && result_ && id < IDM_DEVICE_BASE + static_cast<int>(result_->disks.size())) {
        selectDisk(id - IDM_DEVICE_BASE);
        updateStatus();
        return;
    }
    switch (id) {
        case IDM_REFRESH: startDiscovery(); break;
        case IDM_OPEN_IMAGE: chooseImage(); break;
        case IDM_SAVE_PLAN: savePlan(); break;
        case IDM_NEW: planNew(); break;
        case IDM_DELETE: planDelete(); break;
        case IDM_RESIZE: planResize(); break;
        case IDM_COPY: planCopy(); break;
        case IDM_PASTE: planPaste(); break;
        case IDM_LABEL: planLabel(); break;
        case IDM_NAME: planName(); break;
        case IDM_FLAGS: planFlags(); break;
        case IDM_CHECK: planSimple(OpType::Check); break;
        case IDM_UUID: planSimple(OpType::NewUuid); break;
        case IDM_CREATE_TABLE: planCreateTable(); break;
        case IDM_UNDO:
            if (auto* q = currentQueue(); q && q->undoLast()) refreshView();
            break;
        case IDM_CLEAR_OPS:
            if (auto* q = currentQueue()) { q->clear(); refreshView(); }
            break;
        case IDM_APPLY_OPS: showApplySummary(); break;
        case IDM_RESCUE: startRescue(); break;
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
    if (font_) {
        LRESULT handled = 0;
        if (handleMenuBarMessage(hwnd_, msg, wp, lp, palette(isDark()), font_, handled)) return handled;
    }
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
                const int widths[ColCount] = {64, 130, 96, 80, 100, 84, 80, 80, 100, 130};
                ListView_SetColumnWidth(list_, i, scale(widths[i], dpi_));
            }
            layout();
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) {
                refreshAccentColor();  // accent colour and light/dark preference
                refreshHighContrast();
                applyTheme();
            } else if (wp == SPI_SETHIGHCONTRAST) {
                refreshHighContrast();
                applyTheme();
            }
            break;
        case WM_SYSCOLORCHANGE:
            refreshHighContrast();
            applyTheme();
            break;
        case WM_DRAWITEM: {
            const auto* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (di->CtlType == ODT_BUTTON) {
                drawButton(*di, palette(isDark()), dpi_, font_, iconFont_);
                return TRUE;
            }
            if (di->CtlType == ODT_LISTBOX && di->hwndItem == pending_) {
                drawPendingItem(*di);
                return TRUE;
            }
            if (di->CtlType == ODT_STATIC && di->hwndItem == banner_) {
                // Fluent InfoBar (caution): rounded, icon, message.
                const Palette& p = palette(isDark());
                fillRect(di->hDC, di->rcItem, p.window);
                fillRounded(di->hDC, di->rcItem, scale(8, dpi_), p.warning, blend(p.warning, p.caution, 30));
                SetBkMode(di->hDC, TRANSPARENT);
                RECT r = di->rcItem;
                r.left += scale(16, dpi_);
                if (iconFont_) {
                    RECT ir{r.left, r.top, r.left + scale(20, dpi_), r.bottom};
                    HGDIOBJ old = SelectObject(di->hDC, iconFont_);
                    SetTextColor(di->hDC, p.caution);
                    const wchar_t g = glyph::Warning;
                    DrawTextW(di->hDC, &g, 1, &ir, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(di->hDC, old);
                    r.left += scale(32, dpi_);
                }
                r.right -= scale(240, dpi_);
                wchar_t text[512];
                GetWindowTextW(banner_, text, 512);
                HGDIOBJ old = SelectObject(di->hDC, font_);
                SetTextColor(di->hDC, p.text);
                DrawTextW(di->hDC, text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                SelectObject(di->hDC, old);
                return TRUE;
            }
            break;
        }
        case WM_GETFONT:
            return reinterpret_cast<LRESULT>(font_);
        case WM_ERASEBKGND:
            paintBackground(reinterpret_cast<HDC>(wp));
            return 1;
        case WM_MEASUREITEM: {
            auto* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
            if (mi->CtlType == ODT_LISTBOX) {
                mi->itemHeight = static_cast<UINT>(scale(28, dpi_));
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN: {
            const Palette& p = palette(isDark());
            HDC dc = reinterpret_cast<HDC>(wp);
            HWND ctl = reinterpret_cast<HWND>(lp);
            const bool warn = ctl == banner_;
            SetTextColor(dc, (ctl == status_ || ctl == statusRight_) ? p.mutedText : p.text);
            SetBkColor(dc, warn ? p.warning : (msg == WM_CTLCOLORLISTBOX ? p.card : p.window));
            return reinterpret_cast<LRESULT>(warn ? warnBrush_ : (msg == WM_CTLCOLORLISTBOX ? panelBrush_ : windowBrush_));
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id == IDC_COMBO && HIWORD(wp) == CBN_SELCHANGE) {
                selectDisk(static_cast<int>(SendMessageW(combo_, CB_GETCURSEL, 0, 0)));
                updateStatus();
                return 0;
            }
            // Exceptions must never cross the window procedure boundary.
            try {
                onCommand(id);
            } catch (const std::exception& e) {
                log::error(std::string("Command ") + std::to_string(id) + " failed: " + e.what());
                MessageBoxW(hwnd_, win::widen(e.what()).c_str(), tr(S::AppTitle), MB_ICONERROR);
            }
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
                if (nm->code == NM_CUSTOMDRAW) return listCustomDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(lp));
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
        case WM_APP + 51:  // rescue scan finished
            onRescueDone(lp);
            return 0;
        case WM_DESTROY: {
            if (worker_.joinable()) worker_.join();
            if (rescueWorker_.joinable()) rescueWorker_.join();
            MSG m;
            while (PeekMessageW(&m, hwnd_, WM_APP_DISCOVERED, WM_APP_DISCOVERED, PM_REMOVE))
                delete reinterpret_cast<win::DiscoveryResult*>(m.lParam);
            for (HBRUSH b : {windowBrush_, panelBrush_, warnBrush_})
                if (b) DeleteObject(b);
            for (HFONT f : {font_, sectionFont_, iconFont_, captionFont_})
                if (f) DeleteObject(f);
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
