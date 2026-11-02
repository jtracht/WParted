#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 The WParted contributors
#
# Test fixture only, for sparse image files: splits the $DATA of the $MFT of a
# volume made by mkntfs into three segments behind an attribute list, as Windows
# does for a fragmented $MFT. The later clusters move to scattered free clusters
# near the end of the volume. Two reserved records hold the extension segments.
#
# Usage: ntfs-fragment-mft.py <image>
import struct
import sys


def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def u64(b, o): return struct.unpack_from('<Q', b, o)[0]


def unfix(rec):
    usa, cnt = u16(rec, 4), u16(rec, 6)
    for i in range(1, cnt):
        rec[i * 512 - 2:i * 512] = rec[usa + 2 * i:usa + 2 * i + 2]


def fix(rec):
    usa, cnt = u16(rec, 4), u16(rec, 6)
    usn = (u16(rec, usa) % 0xFFFE) + 1
    struct.pack_into('<H', rec, usa, usn)
    for i in range(1, cnt):
        rec[usa + 2 * i:usa + 2 * i + 2] = rec[i * 512 - 2:i * 512]
        struct.pack_into('<H', rec, i * 512 - 2, usn)


def decode(b):
    runs, i, lcn = [], 0, 0
    while b[i]:
        lb, ob = b[i] & 15, b[i] >> 4
        length = int.from_bytes(b[i + 1:i + 1 + lb], 'little')
        lcn += int.from_bytes(b[i + 1 + lb:i + 1 + lb + ob], 'little', signed=True)
        runs.append((length, lcn))
        i += 1 + lb + ob
    return runs


def encode(runs):
    out, prev = bytearray(), 0
    for length, lcn in runs:
        lb = (length.bit_length() + 8) // 8
        delta, ob = lcn - prev, 1
        while not -(1 << (8 * ob - 1)) <= delta < (1 << (8 * ob - 1)):
            ob += 1
        out.append(lb | ob << 4)
        out += length.to_bytes(lb, 'little') + delta.to_bytes(ob, 'little', signed=True)
        prev = lcn
    return bytes(out) + b'\0'


def attrs(rec):
    a = u16(rec, 0x14)
    while u32(rec, a) != 0xFFFFFFFF:
        yield a, u32(rec, a), u32(rec, a + 4)
        a += u32(rec, a + 4)


def segment_attr(runs, low, instance):
    enc = encode(runs)
    size = (0x40 + len(enc) + 7) // 8 * 8
    at = bytearray(size)
    high = low + sum(r[0] for r in runs) - 1
    struct.pack_into('<IIBBHHH', at, 0, 0x80, size, 1, 0, 0x40, 0, instance)
    struct.pack_into('<QQHH', at, 0x10, low, high, 0x40, 0)
    at[0x40:0x40 + len(enc)] = enc
    return bytes(at)


def list_entry(type_, low, record, seq, instance):
    return struct.pack('<IHBBQQH', type_, 0x20, 0, 0x1A, low, record | seq << 48, instance) + bytes(6)


def main(path):
    with open(path, 'r+b') as f:
        boot = f.read(512)
        cs = u16(boot, 11) * boot[13]
        clusters = u64(boot, 0x28) // boot[13]
        raw = struct.unpack_from('<b', boot, 0x40)[0]
        rs = raw * cs if raw > 0 else 1 << -raw
        mft_lcn, mirr_lcn = u64(boot, 0x30), u64(boot, 0x38)

        def read(off, n):
            f.seek(off)
            return bytearray(f.read(n))

        def write(off, data):
            f.seek(off)
            f.write(data)

        rec0 = read(mft_lcn * cs, rs)
        unfix(rec0)
        da = next(a for a, t, _ in attrs(rec0) if t == 0x80)
        base = decode(rec0[da + u16(rec0, da + 0x20):da + u32(rec0, da + 4)])
        size = u64(rec0, da + 0x30)
        mft_clusters = [lcn + i for length, lcn in base for i in range(length)]
        keep = 16 * rs // cs + 1  # the records 0-15 and the extension records stay in one run
        if len(mft_clusters) < keep + 8:
            sys.exit(f'$MFT too small for the fixture: {len(mft_clusters)} clusters')

        bm_rec = read(mft_lcn * cs + 6 * rs, rs)
        unfix(bm_rec)
        ba = next(a for a, t, _ in attrs(bm_rec) if t == 0x80)
        bm_runs = decode(bm_rec[ba + u16(bm_rec, ba + 0x20):ba + u32(bm_rec, ba + 4)])
        bits = bytearray()
        for length, lcn in bm_runs:
            bits += read(lcn * cs, length * cs)

        def used(c): return bits[c // 8] >> (c % 8) & 1
        def mark(c, on):
            bits[c // 8] = bits[c // 8] | 1 << (c % 8) if on else bits[c // 8] & ~(1 << (c % 8))

        # Every moved cluster becomes its own run, one free cluster apart, from the end.
        moved, spot = [], clusters - 2
        for old in mft_clusters[keep:]:
            while used(spot) or used(spot + 1):
                spot -= 1
            write(spot * cs, read(old * cs, cs))
            mark(spot, True)
            mark(old, False)
            moved.append(spot)
            spot -= 2
        mid = len(moved) // 2
        seg1 = [(1, c) for c in moved[:mid]]
        seg2 = [(1, c) for c in moved[mid:]]

        s0 = []
        for c in mft_clusters[:keep]:
            if s0 and s0[-1][1] + s0[-1][0] == c:
                s0[-1] = (s0[-1][0] + 1, s0[-1][1])
            else:
                s0.append((1, c))

        # Extension records: reserved records without attributes in the first run.
        # mkntfs marks them in use in the MFT bitmap, so nothing else can take them.
        ma = next(a for a, t, _ in attrs(rec0) if t == 0xB0)
        if rec0[ma + 8] == 0:
            mft_bits = rec0[ma + u16(rec0, ma + 0x14):ma + u16(rec0, ma + 0x14) + u32(rec0, ma + 0x10)]
        else:
            mb_runs = decode(rec0[ma + u16(rec0, ma + 0x20):ma + u32(rec0, ma + 4)])
            mft_bits = read(mb_runs[0][1] * cs, cs)
        seq0 = u16(rec0, 0x10)
        ext = []
        for n in range(16, keep * cs // rs):
            r = read(mft_lcn * cs + n * rs, rs)
            if r[:4] == b'FILE' and mft_bits[n // 8] >> (n % 8) & 1 and u32(r, u16(r, 0x14)) == 0xFFFFFFFF:
                ext.append((n, r))
            if len(ext) == 2:
                break
        if len(ext) != 2:
            sys.exit('no reserved records without attributes')
        vcn = keep
        entries = []
        for (n, r), runs in zip(ext, (seg1, seg2)):
            unfix(r)
            at = segment_attr(runs, vcn, 0)
            start = u16(r, 0x14)
            if start + len(at) + 8 > u32(r, 0x1C):
                sys.exit('a segment does not fit into its record')
            r[start:start + len(at)] = at
            r[start + len(at):start + len(at) + 8] = struct.pack('<II', 0xFFFFFFFF, 0)
            struct.pack_into('<H', r, 0x16, 1)
            struct.pack_into('<I', r, 0x18, start + len(at) + 8)
            struct.pack_into('<Q', r, 0x20, seq0 << 48)
            struct.pack_into('<H', r, 0x28, 1)
            fix(r)
            write(mft_lcn * cs + n * rs, r)
            entries.append(list_entry(0x80, vcn, n, u16(r, 0x10), 0))
            vcn += len(runs)

        # Record 0: shorter runs, highest VCN, and the attribute list after $STANDARD_INFORMATION.
        enc = encode(s0)
        mp = u16(rec0, da + 0x20)
        old_len = u32(rec0, da + 4)
        new_len = (mp + len(enc) + 7) // 8 * 8
        new_attr = bytearray(rec0[da:da + mp]) + enc + bytes(new_len - mp - len(enc))
        struct.pack_into('<I', new_attr, 4, new_len)
        struct.pack_into('<Q', new_attr, 0x18, keep - 1)
        rec0 = rec0[:da] + new_attr + rec0[da + old_len:]
        listing = []
        for a, t, ln in attrs(rec0):
            listing.append(list_entry(t, 0, 0, seq0, u16(rec0, a + 0x0E)))
            if t == 0x80:
                listing += entries
        body = b''.join(listing)
        at_len = (0x18 + len(body) + 7) // 8 * 8
        at = bytearray(at_len)
        next_id = u16(rec0, 0x28)
        struct.pack_into('<IIBBHHH', at, 0, 0x20, at_len, 0, 0, 0x18, 0, next_id)
        struct.pack_into('<IHH', at, 0x10, len(body), 0x18, 0)
        at[0x18:0x18 + len(body)] = body
        struct.pack_into('<H', rec0, 0x28, next_id + 1)
        pos = next(a for a, t, _ in attrs(rec0) if t > 0x20)
        a = u16(rec0, 0x14)
        while u32(rec0, a) != 0xFFFFFFFF:
            a += u32(rec0, a + 4)
        used_bytes = a + 8
        if used_bytes + at_len > rs:
            sys.exit('the attribute list does not fit into record 0')
        rec0 = bytes(rec0[:pos]) + bytes(at) + bytes(rec0[pos:used_bytes])
        rec0 = bytearray(rec0.ljust(rs, b'\0'))
        struct.pack_into('<I', rec0, 0x18, used_bytes + at_len)
        fix(rec0)
        write(mft_lcn * cs, rec0)

        # $MFTMirr and $Bitmap.
        write(mirr_lcn * cs, rec0)
        done = 0
        for length, lcn in bm_runs:
            write(lcn * cs, bits[done:done + length * cs])
            done += length * cs
        print(f'fragmented: {len(mft_clusters)} clusters, {size} bytes, {len(seg1)} + {len(seg2)} moved runs')


if __name__ == '__main__':
    main(sys.argv[1])
