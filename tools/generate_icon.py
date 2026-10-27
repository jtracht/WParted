#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generates apps/wparted-gui/res/wparted.ico without third-party libraries.

Design: a rounded slate tile with a horizontal "disk map" bar split into
three partitions (teal, blue, grey = unallocated). Original WParted artwork.
Usage: python tools/generate_icon.py [output.ico]
"""
import struct
import sys
from pathlib import Path

SIZES = [16, 20, 24, 32, 40, 48, 64, 256]

TILE = (0x26, 0x32, 0x38)
SEGMENTS = [(0.0, 0.46, (0x2E, 0xC4, 0x9A)), (0.46, 0.78, (0x3B, 0x82, 0xF6)), (0.78, 1.0, (0x9C, 0xA3, 0xAF))]


def coverage_rounded_rect(px, py, x0, y0, x1, y1, r, ss=4):
    """Anti-aliased coverage of pixel (px,py) by a rounded rectangle."""
    hit = 0
    for sy in range(ss):
        for sx in range(ss):
            x = px + (sx + 0.5) / ss
            y = py + (sy + 0.5) / ss
            if x < x0 or x > x1 or y < y0 or y > y1:
                continue
            cx = min(max(x, x0 + r), x1 - r)
            cy = min(max(y, y0 + r), y1 - r)
            if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                hit += 1
    return hit / (ss * ss)


def render(n):
    px = [[(0, 0, 0, 0)] * n for _ in range(n)]
    m = n * 0.06
    tile = (m, m, n - m, n - m, n * 0.2)
    bar_y0, bar_y1 = n * 0.36, n * 0.64
    bar_x0, bar_x1 = n * 0.16, n * 0.84
    gap = max(1.0, n * 0.03)
    for y in range(n):
        for x in range(n):
            a = coverage_rounded_rect(x, y, *tile)
            if a <= 0:
                continue
            col = TILE
            for (f0, f1, c) in SEGMENTS:
                sx0 = bar_x0 + (bar_x1 - bar_x0) * f0 + (gap / 2 if f0 > 0 else 0)
                sx1 = bar_x0 + (bar_x1 - bar_x0) * f1 - (gap / 2 if f1 < 1 else 0)
                cov = coverage_rounded_rect(x, y, sx0, bar_y0, sx1, bar_y1, n * 0.04)
                if cov > 0:
                    col = tuple(int(col[i] * (1 - cov) + c[i] * cov) for i in range(3))
            # usage strip under the bar
            cov = coverage_rounded_rect(x, y, bar_x0, n * 0.70, bar_x0 + (bar_x1 - bar_x0) * 0.6, n * 0.75, n * 0.02)
            if cov > 0:
                col = tuple(int(col[i] * (1 - cov) + 0xF8 * cov) for i in range(3))
            px[y][x] = (col[0], col[1], col[2], int(255 * a))
    return px


def png_chunk(tag, data):
    import zlib
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def encode_png(px):
    import zlib
    n = len(px)
    raw = b"".join(b"\x00" + bytes(c for p in row for c in p) for row in px)
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 6, 0, 0, 0)) +
            png_chunk(b"IDAT", zlib.compress(raw, 9)) + png_chunk(b"IEND", b""))


def encode_bmp(px):
    """32-bit BGRA DIB with AND mask, as required for small icon entries on Windows 7."""
    n = len(px)
    header = struct.pack("<IiiHHIIiiII", 40, n, n * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    xor = b"".join(bytes((p[2], p[1], p[0], p[3])) for row in reversed(px) for p in row)
    row_bytes = ((n + 31) // 32) * 4
    andmask = b"\x00" * (row_bytes * n)
    return header + xor + andmask


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent / "apps/wparted-gui/res/wparted.ico"
    images = []
    for n in SIZES:
        px = render(n)
        images.append((n, encode_png(px) if n >= 256 else encode_bmp(px)))
    data = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries, blobs = b"", b""
    for n, blob in images:
        entries += struct.pack("<BBBBHHII", n % 256, n % 256, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
        blobs += blob
    out.write_bytes(data + entries + blobs)
    print(f"wrote {out} ({len(images)} sizes)")


if __name__ == "__main__":
    main()
