// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Graphical disk map: proportional boxes for partitions and unallocated
// space, with nested logical partitions. Plain GDI with double buffering so
// it renders identically on Windows 7 to 11 without extra runtimes.
#include <windowsx.h>

#include <algorithm>
#include <string>
#include <vector>

#include "model.hpp"
#include "strings.hpp"

namespace wparted::ui {

namespace {

struct MapState {
    const DiskView* view = nullptr;
    const Palette* pal = nullptr;
    int selected = -1;
    std::vector<RECT> rects;  // per row
    bool focused = false;
};

MapState* state(HWND h) { return reinterpret_cast<MapState*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

// Distributes `width` pixels over items proportional to `weights`, giving each
// at least `minW` (if possible) so that tiny partitions stay clickable.
std::vector<int> distribute(const std::vector<std::uint64_t>& weights, int width, int minW) {
    const std::size_t n = weights.size();
    std::vector<int> w(n, 0);
    if (n == 0 || width <= 0) return w;
    minW = std::min(minW, width / static_cast<int>(n));
    std::vector<bool> fixed(n, false);
    int remaining = width;
    for (int iter = 0; iter < 4; ++iter) {
        long double total = 0;
        for (std::size_t i = 0; i < n; ++i)
            if (!fixed[i]) total += static_cast<long double>(weights[i]);
        bool changed = false;
        for (std::size_t i = 0; i < n; ++i) {
            if (fixed[i]) continue;
            int px = total > 0 ? static_cast<int>(weights[i] / total * remaining) : remaining / static_cast<int>(n);
            if (px < minW) {
                fixed[i] = true;
                w[i] = minW;
                changed = true;
            } else {
                w[i] = px;
            }
        }
        remaining = width;
        for (std::size_t i = 0; i < n; ++i)
            if (fixed[i]) remaining -= w[i];
        if (!changed) break;
    }
    int sum = 0;
    for (int x : w) sum += x;
    if (n) w[n - 1] += width - sum;  // absorb rounding
    return w;
}

std::wstring rowTitle(const DiskView& v, const Row& r) {
    if (r.kind == Segment::Kind::Unallocated) return v.tableRead ? tr(S::Unallocated) : L"?";
    const auto& p = v.layout->partitions[static_cast<std::size_t>(r.partitionIndex)];
    if (const auto* vol = win::volumeForPartition(*v.disk, p)) {
        if (!vol->mountPoints.empty()) return vol->mountPoints.front();
    }
    std::wstring t = p.number > 0 ? L"#" + std::to_wstring(p.number) : std::wstring(tr(S::VolumeOnly));
    if (!p.fs.label.empty()) t += L" " + win::widen(p.fs.label);
    return t;
}

void layoutRects(HWND h, MapState& s) {
    s.rects.assign(s.view ? s.view->rows.size() : 0, RECT{});
    if (!s.view || s.view->rows.empty()) return;
    RECT rc;
    GetClientRect(h, &rc);
    const UINT dpi = dpiForWindow(h);
    const int pad = scale(4, dpi);
    InflateRect(&rc, -pad, -pad);
    const auto& rows = s.view->rows;

    std::vector<std::size_t> top;
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (rows[i].depth == 0) top.push_back(i);
    std::vector<std::uint64_t> weights;
    for (auto i : top) weights.push_back(rows[i].sectorCount);
    auto widths = distribute(weights, rc.right - rc.left, scale(24, dpi));
    int x = rc.left;
    for (std::size_t k = 0; k < top.size(); ++k) {
        RECT r{x, rc.top, x + widths[k], rc.bottom};
        s.rects[top[k]] = r;
        x += widths[k];
        if (!rows[top[k]].isExtended) continue;
        std::vector<std::size_t> kids;
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (rows[i].parentRow == static_cast<int>(top[k])) kids.push_back(i);
        RECT inner = r;
        InflateRect(&inner, -scale(4, dpi), -scale(4, dpi));
        std::vector<std::uint64_t> kw;
        for (auto i : kids) kw.push_back(rows[i].sectorCount);
        auto kwid = distribute(kw, inner.right - inner.left, scale(16, dpi));
        int kx = inner.left;
        for (std::size_t j = 0; j < kids.size(); ++j) {
            s.rects[kids[j]] = RECT{kx, inner.top, kx + kwid[j], inner.bottom};
            kx += kwid[j];
        }
    }
}

void fill(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void frame(HDC dc, RECT r, COLORREF c, int thickness) {
    for (int i = 0; i < thickness; ++i) {
        HBRUSH b = CreateSolidBrush(c);
        FrameRect(dc, &r, b);
        DeleteObject(b);
        InflateRect(&r, -1, -1);
    }
}

void paint(HWND h, MapState& s) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    const Palette& pal = s.pal ? *s.pal : palette(false);
    fill(dc, rc, pal.window);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(GetParent(h), WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    const UINT dpi = dpiForWindow(h);
    const int border = scale(3, dpi);

    if (s.view) {
        const auto& rows = s.view->rows;
        // Extended containers first so that logicals paint on top.
        for (int pass = 0; pass < 2; ++pass) {
            for (std::size_t i = 0; i < rows.size(); ++i) {
                const Row& row = rows[i];
                const bool isContainer = row.isExtended;
                if ((pass == 0) != isContainer) continue;
                RECT r = s.rects[i];
                if (r.right - r.left < 2) continue;
                RECT box = r;
                InflateRect(&box, -1, -1);
                if (row.kind == Segment::Kind::Unallocated) {
                    if (s.view->tableRead) {
                        fill(dc, box, pal.unallocated);
                    } else {
                        HBRUSH hatch = CreateHatchBrush(HS_BDIAGONAL, pal.border);
                        fill(dc, box, pal.window);
                        SetBkColor(dc, pal.window);
                        FillRect(dc, &box, hatch);
                        DeleteObject(hatch);
                        SetBkMode(dc, TRANSPARENT);
                    }
                } else if (isContainer) {
                    fill(dc, box, pal.window);
                    frame(dc, box, RGB(0x7D, 0xFC, 0xFE), border);
                    continue;  // children carry the text
                } else {
                    const auto& p = s.view->layout->partitions[static_cast<std::size_t>(row.partitionIndex)];
                    fill(dc, box, pal.unused);
                    const auto* vol = win::volumeForPartition(*s.view->disk, p);
                    if (vol && vol->hasUsage && vol->totalBytes) {
                        RECT used = box;
                        InflateRect(&used, -border, -border);
                        const double frac = static_cast<double>(vol->totalBytes - vol->freeBytes) / vol->totalBytes;
                        used.right = used.left + static_cast<int>((used.right - used.left) * frac);
                        fill(dc, used, pal.used);
                    }
                    frame(dc, box, fsColor(p.fs.type), border);
                }
                // Two text lines: title and size; omitted when there is no room.
                if (box.right - box.left < scale(44, dpi)) continue;
                RECT tr1 = box;
                InflateRect(&tr1, -border - scale(2, dpi), -border);
                SetTextColor(dc, pal.text);
                std::wstring title = rowTitle(*s.view, row);
                std::wstring size = win::widen(formatSize(row.sectorCount * s.view->layout->sectorSize));
                std::wstring text = title + L"\n" + size;
                DrawTextW(dc, text.c_str(), -1, &tr1, DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | DT_WORDBREAK);
            }
        }
        if (s.selected >= 0 && s.selected < static_cast<int>(s.rects.size())) {
            RECT r = s.rects[static_cast<std::size_t>(s.selected)];
            frame(dc, r, pal.accent, scale(2, dpi));
            if (s.focused) {
                InflateRect(&r, -scale(3, dpi), -scale(3, dpi));
                DrawFocusRect(dc, &r);
            }
        }
    }
    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    if (oldFont) SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

int hitTest(const MapState& s, POINT pt) {
    if (!s.view) return -1;
    // Children first: they lie inside their extended container.
    for (int pass = 0; pass < 2; ++pass)
        for (std::size_t i = 0; i < s.rects.size(); ++i)
            if ((s.view->rows[i].depth == 1) == (pass == 0) && PtInRect(&s.rects[i], pt)) return static_cast<int>(i);
    return -1;
}

void select(HWND h, MapState& s, int row, bool notify) {
    s.selected = row;
    InvalidateRect(h, nullptr, FALSE);
    if (notify) SendMessageW(GetParent(h), DMN_SELECT, static_cast<WPARAM>(row), 0);
}

LRESULT CALLBACK mapProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    MapState* s = state(h);
    switch (msg) {
        case WM_NCCREATE:
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new MapState));
            break;
        case WM_NCDESTROY:
            delete s;
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            break;
        case DM_SETVIEW:
            s->view = reinterpret_cast<const DiskView*>(lp);
            s->pal = reinterpret_cast<const Palette*>(wp);
            s->selected = -1;
            layoutRects(h, *s);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case DM_SETSELECTION:
            select(h, *s, static_cast<int>(wp), false);
            return 0;
        case WM_SIZE:
            layoutRects(h, *s);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            paint(h, *s);
            return 0;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN: {
            SetFocus(h);
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int row = hitTest(*s, pt);
            if (row >= 0) select(h, *s, row, true);
            break;
        }
        case WM_LBUTTONDBLCLK: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int row = hitTest(*s, pt);
            if (row >= 0) SendMessageW(GetParent(h), DMN_ACTIVATE, static_cast<WPARAM>(row), 0);
            return 0;
        }
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS;
        case WM_KEYDOWN:
            if (s->view && !s->view->rows.empty()) {
                const int n = static_cast<int>(s->view->rows.size());
                if (wp == VK_RIGHT) select(h, *s, std::min(n - 1, s->selected + 1), true);
                if (wp == VK_LEFT) select(h, *s, std::max(0, s->selected - 1), true);
                if (wp == VK_HOME) select(h, *s, 0, true);
                if (wp == VK_END) select(h, *s, n - 1, true);
                if (wp == VK_RETURN && s->selected >= 0)
                    SendMessageW(GetParent(h), DMN_ACTIVATE, static_cast<WPARAM>(s->selected), 0);
            }
            return 0;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            s->focused = msg == WM_SETFOCUS;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

bool registerDiskMapClass(HINSTANCE inst) {
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = mapProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    wc.lpszClassName = kDiskMapClass;
    return RegisterClassExW(&wc) != 0;
}

}  // namespace wparted::ui
