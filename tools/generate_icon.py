#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generates the WParted icon and mascot images without third-party libraries.

Artwork: Parti, the mascot of the WParted website (docs/images/parti.svg), a smiling
hard disk whose platter shows three partitions. Large sizes show all of it; the small
ones drop the details that would turn into noise, like the website's favicon.
Usage: python tools/generate_icon.py
Writes apps/wparted-gui/res/wparted.ico and docs/images/parti.png.
"""
import math
import struct
import zlib
from pathlib import Path

SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
LINE = (0x2B, 0x25, 0x47)
BODY = (0xC7, 0xB8, 0xFF)
MINT = (0x9F, 0xE3, 0xC8)
PINK = (0xFF, 0xB3, 0xCF)
BUTTER = (0xFF, 0xE0, 0x8A)
WHITE = (0xFF, 0xFF, 0xFF)


def rrect(x0, y0, x1, y1, r):
    def inside(x, y):
        if x < x0 or x > x1 or y < y0 or y > y1:
            return False
        cx = min(max(x, x0 + r), x1 - r)
        cy = min(max(y, y0 + r), y1 - r)
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r
    return inside


def ellipse(cx, cy, rx, ry=None):
    ry = rx if ry is None else ry
    return lambda x, y: ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1


def slice_of(cx, cy, r, start, end):
    """Pie slice; angles in degrees, clockwise from 12 o'clock."""
    def inside(x, y):
        if (x - cx) ** 2 + (y - cy) ** 2 > r * r:
            return False
        a = math.degrees(math.atan2(x - cx, cy - y)) % 360
        return start <= a < end
    return inside


def smile(cx, cy, half, depth, width):
    """The quadratic curve of the mouth, as a thick stroke."""
    points = []
    for i in range(25):
        t = i / 24
        points.append((cx - half + 2 * half * t, cy + 4 * depth * t * (1 - t) / 2))
    def inside(x, y):
        return any((x - px) ** 2 + (y - py) ** 2 <= (width / 2) ** 2 for px, py in points)
    return inside


# Slices as on the website: mint from 12 to about 4 o'clock, pink to 7, butter to 12.
def shapes(n):
    """(test, colour, opacity) from back to front, in the 120-unit space of the SVG."""
    # Below 32 px the outline stays heavy and the face is reduced to eyes and a smile.
    tiny = n < 32
    small = n < 64
    # The body fills more of the tile than in the SVG, so that the icon is not undersized.
    line = 7 if tiny else 5 if small else 3
    out = []
    if not small:
        out.append((ellipse(60, 112, 34, 5), LINE, 0.12))
    x0, y0, x1, y1 = (14, 4, 106, 116) if small else (18, 14, 102, 106)
    hl = line / 2
    out.append((rrect(x0 - hl, y0 - hl, x1 + hl, y1 + hl, 22 + hl), LINE, 1))
    out.append((rrect(x0 + hl, y0 + hl, x1 - hl, y1 - hl, 22 - hl), BODY, 1))
    cy, r = (46, 30) if small else (52, 24)
    out.append((ellipse(60, cy, r + hl), LINE, 1))
    out.append((ellipse(60, cy, r - hl), WHITE, 1))
    pie = r - hl
    out.append((slice_of(60, cy, pie, 0, 108), MINT, 1))
    out.append((slice_of(60, cy, pie, 108, 225), PINK, 1))
    out.append((slice_of(60, cy, pie, 225, 360), BUTTER, 1))
    out.append((ellipse(60, cy, 8 if tiny else 6.5), LINE, 1))
    out.append((ellipse(60, cy, 4 if tiny else 4), WHITE, 1))
    if small:
        out += [(ellipse(42, 94, 6 if tiny else 5), LINE, 1), (ellipse(78, 94, 6 if tiny else 5), LINE, 1)]
        out.append((smile(60, 98, 9, 8, 6 if tiny else 4.5), LINE, 1))
        return out
    out.append((rrect(26, 20, 48, 26, 3), WHITE, 0.55))
    out += [(ellipse(46, 86, 3.6), LINE, 1), (ellipse(74, 86, 3.6), LINE, 1)]
    out += [(ellipse(47.2, 84.8, 1.1), WHITE, 1), (ellipse(75.2, 84.8, 1.1), WHITE, 1)]
    out += [(ellipse(37, 93, 5, 3), PINK, 0.9), (ellipse(83, 93, 5, 3), PINK, 0.9)]
    out.append((smile(60, 92, 5, 5, 2.6), LINE, 1))
    out += [(ellipse(90, 98, 3.35), LINE, 1), (ellipse(90, 98, 1.85), MINT, 1)]
    return out


def render(n):
    ss = 16 if n <= 32 else 8 if n <= 64 else 4
    items = shapes(n)
    k = 120 / n
    px = []
    for y in range(n):
        row = []
        for x in range(n):
            ra = ga = ba = aa = 0.0
            for sy in range(ss):
                for sx in range(ss):
                    ux, uy = (x + (sx + 0.5) / ss) * k, (y + (sy + 0.5) / ss) * k
                    r, g, b, a = 0.0, 0.0, 0.0, 0.0  # premultiplied
                    for test, col, op in items:
                        if test(ux, uy):
                            r = col[0] * op + r * (1 - op)
                            g = col[1] * op + g * (1 - op)
                            b = col[2] * op + b * (1 - op)
                            a = op + a * (1 - op)
                    ra += r
                    ga += g
                    ba += b
                    aa += a
            if aa <= 0:
                row.append((0, 0, 0, 0))
            else:
                row.append((round(ra / aa), round(ga / aa), round(ba / aa), round(255 * aa / (ss * ss))))
        px.append(row)
    return px


def png_chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def encode_png(px):
    n = len(px)
    raw = b"".join(b"\x00" + bytes(c for p in row for c in p) for row in px)
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 6, 0, 0, 0)) +
            png_chunk(b"IDAT", zlib.compress(raw, 9)) + png_chunk(b"IEND", b""))


def encode_bmp(px):
    """32-bit BGRA DIB with AND mask; LoadImage returns these with per-pixel alpha."""
    n = len(px)
    header = struct.pack("<IiiHHIIiiII", 40, n, n * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    xor = b"".join(bytes((p[2], p[1], p[0], p[3])) for row in reversed(px) for p in row)
    row_bytes = ((n + 31) // 32) * 4
    return header + xor + b"\x00" * (row_bytes * n)


def main():
    root = Path(__file__).resolve().parent.parent
    images = []
    big = None
    for n in SIZES:
        px = render(n)
        if n == 256:
            big = px
        images.append((n, encode_png(px) if n >= 256 else encode_bmp(px)))
    data = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries, blobs = b"", b""
    for n, blob in images:
        entries += struct.pack("<BBBBHHII", n % 256, n % 256, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
        blobs += blob
    (root / "apps/wparted-gui/res/wparted.ico").write_bytes(data + entries + blobs)
    (root / "docs/images/parti.png").write_bytes(encode_png(big))
    print(f"wrote wparted.ico ({len(images)} sizes) and parti.png")


if __name__ == "__main__":
    main()
