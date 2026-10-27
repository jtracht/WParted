// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#include "inspector.hpp"

#include <windowsx.h>

#include <algorithm>

namespace wparted::ui {

namespace {

struct InspectorState {
    const InspectorData* data = nullptr;
    const Palette* pal = nullptr;
    UINT fontDpi = 0;
    HFONT title = nullptr, body = nullptr, caption = nullptr, icon = nullptr;
    int scroll = 0, contentHeight = 0;
};

InspectorState* state(HWND h) { return reinterpret_cast<InspectorState*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

void ensureFonts(InspectorState& s, UINT dpi) {
    if (s.fontDpi == dpi && s.title) return;
    for (HFONT* f : {&s.title, &s.body, &s.caption, &s.icon})
        if (*f) DeleteObject(*f);
    s.title = createUiFont(dpi, FontRole::Subtitle);
    s.body = createUiFont(dpi, FontRole::Body);
    s.caption = createUiFont(dpi, FontRole::Caption);
    s.icon = createIconFont(dpi, 16);
    s.fontDpi = dpi;
}

int textHeight(HDC dc, HFONT f, const std::wstring& text, int width) {
    HGDIOBJ old = SelectObject(dc, f);
    RECT r{0, 0, width, 0};
    DrawTextW(dc, text.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    return r.bottom;
}

void paint(HWND h, InspectorState& s) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    const Palette& pal = s.pal ? *s.pal : palette(false);
    const UINT dpi = dpiForWindow(h);
    ensureFonts(s, dpi);
    fillRect(dc, rc, pal.window);
    RECT card = rc;
    card.right -= 1;
    card.bottom -= 1;
    fillRounded(dc, card, scale(8, dpi), pal.card, pal.border);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, s.body);

    const int pad = scale(16, dpi);
    const int width = rc.right - 2 * pad;
    int y = pad - s.scroll;
    if (s.data) {
        const InspectorData& d = *s.data;
        RECT t{pad, y, rc.right - pad, y + scale(28, dpi)};
        SelectObject(dc, s.title);
        SetTextColor(dc, pal.text);
        DrawTextW(dc, d.title.c_str(), -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        y += scale(32, dpi);
        if (!d.pill.empty()) {
            RECT pr{pad, y, rc.right - pad, y + scale(24, dpi)};
            drawPill(dc, pr, d.pillColor, readableOn(d.pillColor), d.pill.c_str(), s.caption, dpi);
            y += scale(32, dpi);
        }
        for (const auto& row : d.rows) {
            RECT lr{pad, y, rc.right - pad, y + scale(16, dpi)};
            SelectObject(dc, s.caption);
            SetTextColor(dc, pal.mutedText);
            DrawTextW(dc, row.first.c_str(), -1, &lr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            y += scale(17, dpi);
            const int vh = std::max(scale(18, dpi), textHeight(dc, s.body, row.second, width));
            RECT vr{pad, y, rc.right - pad, y + vh};
            SelectObject(dc, s.body);
            SetTextColor(dc, pal.text);
            DrawTextW(dc, row.second.c_str(), -1, &vr, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
            y += vh + scale(10, dpi);
        }
        if (!d.hint.empty()) {
            y += scale(4, dpi);
            const int iconW = s.icon ? scale(24, dpi) : 0;
            const int hh = textHeight(dc, s.body, d.hint, width - iconW - scale(20, dpi)) + scale(20, dpi);
            RECT box{pad, y, rc.right - pad, y + hh};
            const COLORREF fg = d.warnHint ? pal.caution : pal.accent;
            fillRounded(dc, box, scale(6, dpi), blend(pal.card, fg, pal.dark ? 16 : 9), blend(pal.card, fg, 30));
            if (s.icon) {
                const wchar_t g = d.warnHint ? glyph::Warning : glyph::Info;
                RECT ir{box.left + scale(10, dpi), box.top + scale(10, dpi), box.left + scale(10, dpi) + iconW,
                        box.top + scale(30, dpi)};
                SelectObject(dc, s.icon);
                SetTextColor(dc, fg);
                DrawTextW(dc, &g, 1, &ir, DT_LEFT | DT_TOP | DT_SINGLELINE);
            }
            RECT hr{box.left + scale(10, dpi) + iconW, box.top + scale(10, dpi), box.right - scale(10, dpi), box.bottom};
            SelectObject(dc, s.body);
            SetTextColor(dc, pal.text);
            DrawTextW(dc, d.hint.c_str(), -1, &hr, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
            y += hh;
        }
    }
    s.contentHeight = y + s.scroll + pad;
    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    InspectorState* s = state(h);
    switch (msg) {
        case WM_NCCREATE:
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new InspectorState));
            break;
        case WM_NCDESTROY:
            if (s)
                for (HFONT f : {s->title, s->body, s->caption, s->icon})
                    if (f) DeleteObject(f);
            delete s;
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            break;
        case IM_SETDATA:
            s->pal = reinterpret_cast<const Palette*>(wp);
            s->data = reinterpret_cast<const InspectorData*>(lp);
            s->scroll = 0;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL: {
            RECT rc;
            GetClientRect(h, &rc);
            const int maxScroll = std::max(0, s->contentHeight - static_cast<int>(rc.bottom));
            s->scroll = std::clamp(s->scroll - GET_WHEEL_DELTA_WPARAM(wp) / 2, 0, maxScroll);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_SIZE:
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            paint(h, *s);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

bool registerInspectorClass(HINSTANCE inst) {
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kInspectorClass;
    return RegisterClassExW(&wc) != 0;
}

}  // namespace wparted::ui
