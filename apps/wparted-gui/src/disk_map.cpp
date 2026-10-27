// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Disk card: device header (icon, model, key facts, chips) and the graphical
// disk map with proportional, rounded segments for partitions and free space,
// nested logical partitions, usage bars, hover and selection. Plain GDI with
// double buffering so it renders the same on Windows 7 to 11.
#include <windowsx.h>

#include <algorithm>
#include <string>
#include <vector>

#include "model.hpp"
#include "strings.hpp"

#ifndef WM_DPICHANGED_AFTERPARENT
#define WM_DPICHANGED_AFTERPARENT 0x02E3
#endif

namespace wparted::ui {

namespace {

struct MapState {
    const DiskView* view = nullptr;
    const Palette* pal = nullptr;
    int selected = -1;
    int hot = -1;
    bool tracking = false;
    std::vector<RECT> rects;  // per row
    bool focused = false;
    UINT fontDpi = 0;
    HFONT title = nullptr, body = nullptr, caption = nullptr, icon = nullptr;
};

MapState* state(HWND h) { return reinterpret_cast<MapState*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

void ensureFonts(MapState& s, UINT dpi) {
    if (s.fontDpi == dpi && s.title) return;
    for (HFONT* f : {&s.title, &s.body, &s.caption, &s.icon})
        if (*f) DeleteObject(*f);
    s.title = createUiFont(dpi, FontRole::BodyStrong);
    s.body = createUiFont(dpi, FontRole::BodyStrong);
    s.caption = createUiFont(dpi, FontRole::Caption);
    s.icon = createIconFont(dpi, 24);
    s.fontDpi = dpi;
}

// Height of the header part of the card.
int headerHeight(UINT dpi) { return scale(52, dpi); }

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
    if (r.kind == Segment::Kind::Unallocated) return v.tableRead ? tr(S::Unallocated) : tr(S::UnknownRegion);
    const auto& p = v.layout->partitions[static_cast<std::size_t>(r.partitionIndex)];
    if (const auto* vol = viewVolume(v, p)) {
        if (!vol->mountPoints.empty()) return vol->mountPoints.front();
    }
    std::wstring t = p.number > 0 ? L"#" + std::to_wstring(p.number) : std::wstring(tr(S::VolumeOnly));
    if (!p.fs.label.empty()) t += L"  " + win::widen(p.fs.label);
    return t;
}

void layoutRects(HWND h, MapState& s) {
    s.rects.assign(s.view ? s.view->rows.size() : 0, RECT{});
    if (!s.view || s.view->rows.empty()) return;
    RECT rc;
    GetClientRect(h, &rc);
    const UINT dpi = dpiForWindow(h);
    const int pad = scale(12, dpi), gap = scale(3, dpi);
    RECT area{rc.left + pad, rc.top + headerHeight(dpi), rc.right - pad, rc.bottom - pad};
    const auto& rows = s.view->rows;

    std::vector<std::size_t> top;
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (rows[i].depth == 0) top.push_back(i);
    std::vector<std::uint64_t> weights;
    for (auto i : top) weights.push_back(rows[i].sectorCount);
    const int usable = (area.right - area.left) - gap * static_cast<int>(top.size() > 0 ? top.size() - 1 : 0);
    auto widths = distribute(weights, usable, scale(28, dpi));
    int x = area.left;
    for (std::size_t k = 0; k < top.size(); ++k) {
        RECT r{x, area.top, x + widths[k], area.bottom};
        s.rects[top[k]] = r;
        x += widths[k] + gap;
        if (!rows[top[k]].isExtended) continue;
        std::vector<std::size_t> kids;
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (rows[i].parentRow == static_cast<int>(top[k])) kids.push_back(i);
        RECT inner = r;
        InflateRect(&inner, -scale(5, dpi), -scale(5, dpi));
        std::vector<std::uint64_t> kw;
        for (auto i : kids) kw.push_back(rows[i].sectorCount);
        const int kusable = (inner.right - inner.left) - gap * static_cast<int>(kids.size() > 0 ? kids.size() - 1 : 0);
        auto kwid = distribute(kw, kusable, scale(20, dpi));
        int kx = inner.left;
        for (std::size_t j = 0; j < kids.size(); ++j) {
            s.rects[kids[j]] = RECT{kx, inner.top, kx + kwid[j], inner.bottom};
            kx += kwid[j] + gap;
        }
    }
}

void drawHeader(HDC dc, MapState& s, const RECT& card, UINT dpi) {
    const Palette& pal = *s.pal;
    const DiskView& v = *s.view;
    const auto& d = *v.disk;
    const int pad = scale(12, dpi);
    int x = card.left + pad;
    const int midY = card.top + headerHeight(dpi) / 2 + scale(2, dpi);
    SetBkMode(dc, TRANSPARENT);
    if (s.icon) {
        const wchar_t g = d.busType == "Image file" ? glyph::Disc : d.busType == "USB" ? glyph::Usb : glyph::HardDrive;
        RECT ir{x, midY - scale(14, dpi), x + scale(28, dpi), midY + scale(14, dpi)};
        SelectObject(dc, s.icon);
        SetTextColor(dc, pal.accent);
        DrawTextW(dc, &g, 1, &ir, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        x += scale(38, dpi);
    }
    // Title: model; subtitle: size, bus, table, sector sizes.
    std::wstring title = win::widen(d.modelName());
    std::wstring sub = win::widen(formatSize(d.sizeBytes)) + L"  ·  " + win::widen(d.busType);
    if (v.layout) {
        const char* t = v.layout->type == TableType::Gpt ? "GPT" : v.layout->type == TableType::Mbr ? "MBR (msdos)"
                        : v.layout->type == TableType::None ? "-" : "?";
        if (v.tableRead) sub += L"  ·  " + win::widen(t);
    }
    sub += L"  ·  " + std::to_wstring(d.logicalSectorSize) + L" B";
    if (d.physicalSectorSize && d.physicalSectorSize != d.logicalSectorSize)
        sub += L" / " + std::to_wstring(d.physicalSectorSize) + L" B";
    RECT tr1{x, card.top + scale(8, dpi), card.right - pad, midY};
    SelectObject(dc, s.title);
    SetTextColor(dc, pal.text);
    DrawTextW(dc, title.c_str(), -1, &tr1, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    RECT tr2{x, midY + scale(1, dpi), card.right - pad, card.top + headerHeight(dpi) - scale(4, dpi)};
    SelectObject(dc, s.caption);
    SetTextColor(dc, pal.mutedText);
    DrawTextW(dc, sub.c_str(), -1, &tr2, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    // Chips, right aligned.
    std::vector<std::pair<std::wstring, COLORREF>> chips;
    if (d.busType == "Image file") chips.push_back({L"Image", pal.accent});
    if (d.containsWindows()) chips.push_back({L"Windows", pal.accent});
    if (d.solidState == win::Tristate::Yes) chips.push_back({L"SSD", pal.mutedText});
    if (d.solidState == win::Tristate::No) chips.push_back({L"HDD", pal.mutedText});
    if (!v.tableRead) chips.push_back({tr(S::StatusNotReadable), pal.caution});
    bool spanned = false;
    for (const auto& vol : d.volumes) spanned = spanned || vol.extentCount > 1;
    if (spanned || (v.layout && isManagedByWindows(*v.layout))) chips.push_back({tr(S::ChipManaged), pal.caution});
    int cx = card.right - pad;
    SelectObject(dc, s.caption);
    for (const auto& c : chips) {
        SIZE sz{};
        GetTextExtentPoint32W(dc, c.first.c_str(), static_cast<int>(c.first.size()), &sz);
        const int w = sz.cx + scale(18, dpi);
        RECT r{cx - w, midY - scale(11, dpi), cx, midY + scale(11, dpi)};
        if (r.left < x + scale(200, dpi)) break;  // never cover the title
        drawPill(dc, r, blend(pal.card, c.second, pal.dark ? 28 : 14), c.second, c.first.c_str(), s.caption, dpi);
        cx = r.left - scale(6, dpi);
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
    const UINT dpi = dpiForWindow(h);
    ensureFonts(s, dpi);
    HGDIOBJ oldFont = SelectObject(dc, s.body);
    fillRect(dc, rc, pal.window);
    RECT card = rc;
    card.right -= 1;
    card.bottom -= 1;
    fillRounded(dc, card, scale(8, dpi), pal.card, pal.border);
    SetBkMode(dc, TRANSPARENT);
    const int radius = scale(6, dpi);
    const int bar = scale(4, dpi);

    if (s.view && s.view->disk) {
        drawHeader(dc, s, card, dpi);
        const auto& rows = s.view->rows;
        for (int pass = 0; pass < 2; ++pass) {  // extended containers first
            for (std::size_t i = 0; i < rows.size(); ++i) {
                const Row& row = rows[i];
                if ((pass == 0) != row.isExtended) continue;
                RECT box = s.rects[i];
                if (box.right - box.left < 3) continue;
                const bool hot = static_cast<int>(i) == s.hot;
                if (row.kind == Segment::Kind::Unallocated) {
                    if (s.view->tableRead) {
                        fillRounded(dc, box, radius, blend(pal.card, pal.unallocated, pal.dark ? 35 : 45),
                                    hot ? pal.accent : pal.border);
                    } else {
                        fillRounded(dc, box, radius, pal.card, pal.border);
                        HBRUSH hatch = CreateHatchBrush(HS_BDIAGONAL, pal.border);
                        RECT in = box;
                        InflateRect(&in, -2, -2);
                        SetBkColor(dc, pal.card);
                        SetBkMode(dc, OPAQUE);
                        FillRect(dc, &in, hatch);
                        SetBkMode(dc, TRANSPARENT);
                        DeleteObject(hatch);
                    }
                } else if (row.isExtended) {
                    HPEN pen = CreatePen(PS_DOT, 1, hot ? pal.accent : pal.mutedText);
                    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
                    RoundRect(dc, box.left, box.top, box.right, box.bottom, radius, radius);
                    SelectObject(dc, op);
                    SelectObject(dc, ob);
                    DeleteObject(pen);
                    continue;  // children carry the text
                } else {
                    const auto& p = s.view->layout->partitions[static_cast<std::size_t>(row.partitionIndex)];
                    const COLORREF fc = fsColor(p.fs.type);
                    fillRounded(dc, box, radius, blend(pal.card, fc, pal.dark ? 18 : 10), hot ? pal.accent : pal.border);
                    // Usage: filled part in the file system colour.
                    const auto* vol = viewVolume(*s.view, p);
                    if (vol && vol->hasUsage && vol->totalBytes) {
                        RECT used = box;
                        InflateRect(&used, -2, -2);
                        used.top += bar;
                        const double frac = static_cast<double>(vol->totalBytes - vol->freeBytes) / vol->totalBytes;
                        used.right = used.left + static_cast<int>((used.right - used.left) * frac);
                        fillRect(dc, used, blend(pal.card, fc, pal.dark ? 34 : 24));
                    }
                    RECT top = box;
                    InflateRect(&top, -1, -1);
                    top.bottom = top.top + bar;
                    fillRounded(dc, top, radius / 2, fc, fc);
                }
                if (box.right - box.left < scale(48, dpi)) continue;
                RECT t1 = box;
                InflateRect(&t1, -scale(8, dpi), 0);
                t1.top += bar + scale(4, dpi);
                t1.bottom = (box.top + box.bottom) / 2 + scale(3, dpi);
                SelectObject(dc, s.body);
                SetTextColor(dc, pal.text);
                const std::wstring title = rowTitle(*s.view, row);
                DrawTextW(dc, title.c_str(), -1, &t1, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                RECT t2 = t1;
                t2.top = t1.bottom + scale(1, dpi);
                t2.bottom = box.bottom - scale(4, dpi);
                std::wstring sub = win::widen(formatSize(row.sectorCount * s.view->layout->sectorSize));
                if (row.kind == Segment::Kind::Partition) {
                    const auto& p = s.view->layout->partitions[static_cast<std::size_t>(row.partitionIndex)];
                    sub += L"  ·  " + win::widen(fsTypeName(p.fs.type));
                }
                SelectObject(dc, s.caption);
                SetTextColor(dc, pal.mutedText);
                DrawTextW(dc, sub.c_str(), -1, &t2, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
        }
        if (s.selected >= 0 && s.selected < static_cast<int>(s.rects.size())) {
            RECT r = s.rects[static_cast<std::size_t>(s.selected)];
            HPEN pen = CreatePen(PS_SOLID, scale(2, dpi), pal.accent);
            HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(pen);
            if (s.focused) {
                InflateRect(&r, -scale(4, dpi), -scale(4, dpi));
                DrawFocusRect(dc, &r);
            }
        }
    }
    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
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
            if (s) {
                for (HFONT f : {s->title, s->body, s->caption, s->icon})
                    if (f) DeleteObject(f);
            }
            delete s;
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            break;
        case DM_SETVIEW:
            s->view = reinterpret_cast<const DiskView*>(lp);
            s->pal = reinterpret_cast<const Palette*>(wp);
            s->selected = -1;
            s->hot = -1;
            layoutRects(h, *s);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case DM_SETSELECTION:
            select(h, *s, static_cast<int>(wp), false);
            return 0;
        case WM_SIZE:
        case WM_DPICHANGED_AFTERPARENT:
            layoutRects(h, *s);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            paint(h, *s);
            return 0;
        case WM_MOUSEMOVE: {
            if (!s->tracking) {
                TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, h, 0};
                TrackMouseEvent(&tme);
                s->tracking = true;
            }
            const int hot = hitTest(*s, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (hot != s->hot) {
                s->hot = hot;
                InvalidateRect(h, nullptr, FALSE);
            }
            break;
        }
        case WM_MOUSELEAVE:
            s->tracking = false;
            s->hot = -1;
            InvalidateRect(h, nullptr, FALSE);
            break;
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
