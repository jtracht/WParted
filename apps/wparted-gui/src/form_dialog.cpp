// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#include "form_dialog.hpp"

#include <commctrl.h>

#include <cwchar>

#include "strings.hpp"

namespace wparted::ui {

namespace {

constexpr wchar_t kFormClass[] = L"WPartedForm";
constexpr int IDC_FIELD_BASE = 300;
constexpr int IDC_ERROR = 299;

struct FormState {
    std::vector<FormField>* fields = nullptr;
    const FormUpdate* update = nullptr;
    const Palette* pal = nullptr;
    HFONT font = nullptr;
    HBRUSH bg = nullptr;
    std::vector<HWND> controls;  // one per field
    HWND error = nullptr, ok = nullptr;
    bool done = false, accepted = false, syncing = false;
    bool ready = false;  // all controls created; edits notify EN_CHANGE while being built
    int footerHeight = 0;
};

std::wstring toText(std::uint64_t v) { return std::to_wstring(v); }

void readBack(FormState& s) {
    auto& f = *s.fields;
    if (s.controls.size() != f.size()) return;
    for (std::size_t i = 0; i < f.size(); ++i) {
        HWND c = s.controls[i];
        switch (f[i].kind) {
            case FormField::Kind::Number: {
                wchar_t buf[32] = L"";
                GetWindowTextW(c, buf, 32);
                f[i].number = std::wcstoull(buf, nullptr, 10);
                break;
            }
            case FormField::Kind::Text: {
                std::wstring t(f[i].maxLength + 1, L'\0');
                int n = GetWindowTextW(c, t.data(), static_cast<int>(t.size()));
                t.resize(static_cast<std::size_t>(n));
                f[i].text = t;
                break;
            }
            case FormField::Kind::Choice:
                f[i].selected = static_cast<int>(SendMessageW(c, CB_GETCURSEL, 0, 0));
                break;
            case FormField::Kind::Check:
                f[i].checked = SendMessageW(c, BM_GETCHECK, 0, 0) == BST_CHECKED;
                break;
            case FormField::Kind::Note:
            case FormField::Kind::Preview:
                break;
        }
    }
}

void writeOut(FormState& s, int except) {
    auto& f = *s.fields;
    if (s.controls.size() != f.size()) return;
    s.syncing = true;
    for (std::size_t i = 0; i < f.size(); ++i) {
        if (static_cast<int>(i) == except) continue;
        HWND c = s.controls[i];
        if (f[i].kind == FormField::Kind::Number) SetWindowTextW(c, toText(f[i].number).c_str());
        if (f[i].kind == FormField::Kind::Note) SetWindowTextW(c, f[i].text.c_str());
        if (f[i].kind == FormField::Kind::Preview) InvalidateRect(c, nullptr, FALSE);
        EnableWindow(c, f[i].enabled);
    }
    s.syncing = false;
}

void revalidate(FormState& s, int changed) {
    readBack(s);
    std::wstring err;
    auto& f = *s.fields;
    for (const auto& field : f)
        if (field.kind == FormField::Kind::Number && field.enabled &&
            (field.number < field.minimum || field.number > field.maximum))
            err = field.label + L": " + toText(field.minimum) + L" – " + toText(field.maximum);
    if (err.empty() && *s.update) err = (*s.update)(f, changed);
    writeOut(s, changed);
    SetWindowTextW(s.error, err.c_str());
    EnableWindow(s.ok, err.empty());
}

LRESULT CALLBACK formProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* s = reinterpret_cast<FormState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE:
            SetWindowLongPtrW(h, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
            break;
        case WM_COMMAND: {
            if (!s) break;
            const int id = LOWORD(wp);
            if (id == IDOK || id == IDCANCEL) {
                if (id == IDOK) {
                    revalidate(*s, -1);
                    if (!IsWindowEnabled(s->ok)) return 0;
                }
                s->accepted = id == IDOK;
                s->done = true;
                return 0;
            }
            if (id >= IDC_FIELD_BASE && s->ready && !s->syncing) {
                const int code = HIWORD(wp);
                if (code == EN_CHANGE || code == CBN_SELCHANGE || code == BN_CLICKED) revalidate(*s, id - IDC_FIELD_BASE);
            }
            return 0;
        }
        case WM_DRAWITEM: {
            if (!s) break;
            const auto* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            const Palette& pal = *s->pal;
            const UINT dpi = dpiForWindow(h);
            if (di->CtlType == ODT_BUTTON) {  // primary (accent) button
                const bool disabled = (di->itemState & ODS_DISABLED) != 0;
                const bool pressed = (di->itemState & ODS_SELECTED) != 0;
                fillRect(di->hDC, di->rcItem, pal.window);
                const COLORREF fill = disabled ? blend(pal.window, pal.border, 70)
                                      : pressed ? blend(pal.accent, pal.window, 25) : pal.accent;
                fillRounded(di->hDC, di->rcItem, scale(8, dpi), fill, fill);
                SetBkMode(di->hDC, TRANSPARENT);
                SetTextColor(di->hDC, disabled ? pal.disabledText : pal.onAccent);
                HGDIOBJ old = SelectObject(di->hDC, s->font);
                RECT r = di->rcItem;
                DrawTextW(di->hDC, L"OK", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(di->hDC, old);
                if (di->itemState & ODS_FOCUS) {
                    InflateRect(&r, -scale(3, dpi), -scale(3, dpi));
                    DrawFocusRect(di->hDC, &r);
                }
                return TRUE;
            }
            if (di->CtlType == ODT_STATIC) {  // placement preview
                const int idx = static_cast<int>(di->CtlID) - IDC_FIELD_BASE;
                if (idx < 0 || idx >= static_cast<int>(s->fields->size())) return TRUE;
                const FormField& f = (*s->fields)[static_cast<std::size_t>(idx)];
                RECT r = di->rcItem;
                fillRect(di->hDC, r, pal.card);
                RECT track = r;
                track.right -= 1;
                fillRounded(di->hDC, track, scale(6, dpi), blend(pal.card, pal.unallocated, pal.dark ? 30 : 40), pal.border);
                if (f.previewTotal > 0 && f.previewSize > 0) {
                    const double w = static_cast<double>(track.right - track.left - 2);
                    RECT part = track;
                    InflateRect(&part, -1, -1);
                    part.left = track.left + 1 + static_cast<int>(w * static_cast<double>(f.previewStart) / static_cast<double>(f.previewTotal));
                    part.right = track.left + 1 +
                                 static_cast<int>(w * static_cast<double>(f.previewStart + f.previewSize) / static_cast<double>(f.previewTotal));
                    part.right = std::max<LONG>(part.right, part.left + scale(6, dpi));
                    part.right = std::min<LONG>(part.right, track.right - 1);
                    fillRounded(di->hDC, part, scale(5, dpi), blend(pal.card, f.previewColor, pal.dark ? 30 : 18), pal.accent);
                    RECT bar = part;
                    InflateRect(&bar, -1, 0);
                    bar.top += 1;
                    bar.bottom = bar.top + scale(4, dpi);
                    fillRounded(di->hDC, bar, scale(3, dpi), f.previewColor, f.previewColor);
                    SetBkMode(di->hDC, TRANSPARENT);
                    SetTextColor(di->hDC, pal.text);
                    HGDIOBJ old = SelectObject(di->hDC, s->font);
                    RECT tr2 = part;
                    tr2.top += scale(4, dpi);
                    DrawTextW(di->hDC, f.previewLabel.c_str(), -1, &tr2,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                    SelectObject(di->hDC, old);
                }
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLOREDIT: {
            if (!s) break;
            HDC dc = reinterpret_cast<HDC>(wp);
            const bool isError = reinterpret_cast<HWND>(lp) == s->error;
            SetTextColor(dc, isError ? RGB(0xD1, 0x34, 0x38) : s->pal->text);
            SetBkColor(dc, msg == WM_CTLCOLOREDIT ? s->pal->unused : s->pal->card);
            if (msg == WM_CTLCOLOREDIT) {
                static HBRUSH editBrush = nullptr;
                static COLORREF editColor = 0;
                if (!editBrush || editColor != s->pal->unused) {
                    if (editBrush) DeleteObject(editBrush);
                    editColor = s->pal->unused;
                    editBrush = CreateSolidBrush(editColor);
                }
                return reinterpret_cast<LRESULT>(editBrush);
            }
            return reinterpret_cast<LRESULT>(s->bg);
        }
        case WM_ERASEBKGND: {
            if (!s) break;
            // Fluent dialog: content on the card layer, buttons in a footer band.
            RECT rc;
            GetClientRect(h, &rc);
            FillRect(reinterpret_cast<HDC>(wp), &rc, s->bg);
            RECT footer = rc;
            footer.top = rc.bottom - s->footerHeight;
            fillRect(reinterpret_cast<HDC>(wp), footer, s->pal->window);
            RECT line = footer;
            line.bottom = line.top + 1;
            fillRect(reinterpret_cast<HDC>(wp), line, s->pal->border);
            return 1;
        }
        case WM_CLOSE:
            if (s) s->done = true;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

bool runForm(HWND owner, const std::wstring& title, const std::wstring& intro, std::vector<FormField>& fields,
             const Palette& palette, HFONT font, UINT dpi, const FormUpdate& update) {
    static bool registered = false;
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    if (!registered) {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = formProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kFormClass;
        registered = RegisterClassExW(&wc) != 0;
    }
    FormState s;
    s.fields = &fields;
    s.update = &update;
    s.pal = &palette;
    s.font = font;
    s.bg = CreateSolidBrush(palette.card);

    const int pad = scale(12, dpi), rowH = scale(26, dpi), gap = scale(6, dpi);
    const int labelW = scale(200, dpi), ctrlW = scale(260, dpi);
    const int width = pad * 3 + labelW + ctrlW;
    const int introH = intro.empty() ? 0 : scale(48, dpi);
    const int previewH = scale(44, dpi);
    int previews = 0;
    for (const auto& f : fields)
        if (f.kind == FormField::Kind::Preview) ++previews;
    s.footerHeight = rowH + scale(36, dpi);
    int height = pad + introH + previews * (previewH + scale(12, dpi)) +
                 static_cast<int>(fields.size() - static_cast<std::size_t>(previews)) * (rowH + gap) + scale(30, dpi) +
                 s.footerHeight;

    RECT wr{0, 0, width, height};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectEx(&wr, style, FALSE, WS_EX_DLGMODALFRAME);
    RECT owr;
    GetWindowRect(owner, &owr);
    const int w = wr.right - wr.left, hgt = wr.bottom - wr.top;
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kFormClass, title.c_str(), style,
                             owr.left + (owr.right - owr.left - w) / 2, owr.top + (owr.bottom - owr.top - hgt) / 2, w,
                             hgt, owner, nullptr, inst, &s);
    if (!h) {
        DeleteObject(s.bg);
        return false;
    }
    applyTitleBarTheme(h, palette.dark);
    auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD st, int x, int y, int cw, int ch, int id) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | st, x, y, cw, ch, h,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
        return c;
    };
    int y = pad;
    if (!intro.empty()) {
        child(L"STATIC", intro.c_str(), SS_LEFT | SS_NOPREFIX, pad, y, width - 2 * pad, introH - gap, -1);
        y += introH;
    }
    // Previews first (GParted shows the graphical resizer above the numbers),
    // then the remaining fields in order. Indices stay those of `fields`.
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < fields.size(); ++i)
        if (fields[i].kind == FormField::Kind::Preview) order.push_back(i);
    for (std::size_t i = 0; i < fields.size(); ++i)
        if (fields[i].kind != FormField::Kind::Preview) order.push_back(i);
    s.controls.assign(fields.size(), nullptr);
    for (std::size_t i : order) {
        const FormField& f = fields[i];
        const int id = IDC_FIELD_BASE + static_cast<int>(i);
        if (f.kind == FormField::Kind::Preview) {
            s.controls[i] = child(L"STATIC", L"", SS_OWNERDRAW, pad, y, width - 2 * pad, previewH, id);
            y += previewH + scale(12, dpi);
            continue;
        }
        if (f.kind != FormField::Kind::Check)
            child(L"STATIC", f.label.c_str(), SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, pad, y, labelW, rowH, -1);
        const int cx = pad * 2 + labelW;
        HWND c = nullptr;
        switch (f.kind) {
            case FormField::Kind::Number:
                c = child(WC_EDITW, toText(f.number).c_str(), WS_TABSTOP | WS_BORDER | ES_NUMBER | ES_RIGHT, cx, y, ctrlW,
                          rowH, id);
                break;
            case FormField::Kind::Text:
                c = child(WC_EDITW, f.text.c_str(), WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, cx, y, ctrlW, rowH, id);
                SendMessageW(c, EM_LIMITTEXT, f.maxLength, 0);
                break;
            case FormField::Kind::Choice:
                c = child(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, cx, y, ctrlW, rowH * 8, id);
                for (const auto& ch : f.choices) SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(ch.c_str()));
                SendMessageW(c, CB_SETCURSEL, static_cast<WPARAM>(f.selected), 0);
                applyControlTheme(c, palette.dark, L"DarkMode_CFD");
                break;
            case FormField::Kind::Check:
                c = child(L"BUTTON", f.label.c_str(), WS_TABSTOP | BS_AUTOCHECKBOX, pad, y, width - 2 * pad, rowH, id);
                SendMessageW(c, BM_SETCHECK, f.checked ? BST_CHECKED : BST_UNCHECKED, 0);
                applyControlTheme(c, palette.dark);
                break;
            case FormField::Kind::Note:
                c = child(L"STATIC", f.text.c_str(), SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, cx, y, ctrlW, rowH, id);
                break;
            case FormField::Kind::Preview:
                break;
        }
        EnableWindow(c, f.enabled);
        s.controls[i] = c;
        y += rowH + gap;
    }
    s.error = child(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, pad, y, width - 2 * pad, scale(30, dpi), IDC_ERROR);
    y += scale(30, dpi) + scale(16, dpi);
    const int bw = scale(120, dpi);
    s.ok = child(L"BUTTON", L"OK", WS_TABSTOP | BS_OWNERDRAW, width - pad - 2 * bw - gap, y, bw, rowH + scale(4, dpi), IDOK);
    HWND cancel = child(L"BUTTON", tr(S::Cancel), WS_TABSTOP | BS_PUSHBUTTON, width - pad - bw, y, bw, rowH + scale(4, dpi), IDCANCEL);
    applyControlTheme(s.ok, palette.dark);
    applyControlTheme(cancel, palette.dark);

    s.ready = true;
    revalidate(s, -1);
    EnableWindow(owner, FALSE);
    ShowWindow(h, SW_SHOW);
    if (!s.controls.empty()) SetFocus(s.controls.front());
    MSG m;
    while (!s.done && GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) {
            s.done = true;
            break;
        }
        if (!IsDialogMessageW(h, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    if (s.accepted) readBack(s);
    EnableWindow(owner, TRUE);
    DestroyWindow(h);
    SetForegroundWindow(owner);
    DeleteObject(s.bg);
    return s.accepted;
}

}  // namespace wparted::ui
