#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Willi Ye
# SPDX-License-Identifier: GPL-2.0-or-later

"""Checks azahar's GPU texture untile against a CPU decode of the guest's bytes.

azahar's sgxd_flags `dumptex` writes, for each of the first untiles, the guest's tiled bytes and
the host copy the GPU made (SgxdBackend::Untile):
    azahar-texN-ADDR-WxH-fFORMAT-sSTRIDE-guest.bin
    azahar-texN-ADDR-WxH-fFORMAT-sSTRIDE-host.bin
The host copy is A8B8G8R8 (R the lowest byte), `stride` texels a row, rows in the PICA's memory
order (row 0 is t = 1); a one-channel format's byte copy (-host8.bin) is the texel's one value a
byte, A8 and A4's alpha, I8 and I4's intensity. Stride 0 is a cube face: a SWIZZLED square (Morton, y in bit 0). The guest's bytes are 8x8 tiles, Morton order inside a tile (x in the
even bits), tiles row by row (azahar video_core/utils.h MortonInterleave).

Usage: tex_untile_check.py DIR [--png OUTDIR]
Prints, per texture, how many texels differ and the first few with where the host texel's
value sits in the decode (to show a transposition or mirror). With --png, writes the decode and
the host copy side by side.
"""
import argparse
import pathlib
import re
import sys

NAME = re.compile(r"azahar-tex(\d+)-([0-9a-f]+)-(\d+)x(\d+)-f(\d+)-s(\d+)-guest\.bin$")


def morton(x, y):
    m = 0
    for b in range(3):
        m |= ((x >> b) & 1) << (2 * b) | ((y >> b) & 1) << (2 * b + 1)
    return m


def host_offset(x, y, w, stride):
    """Texel (x, y)'s byte offset in the host copy."""
    if stride:
        return (y * stride + x) * 4
    m = 0
    for b in range(w.bit_length() - 1):
        m |= ((y >> b) & 1) << (2 * b) | ((x >> b) & 1) << (2 * b + 1)
    return m * 4


def decode_texel(fmt, raw):
    """PICA texel bytes -> (r, g, b, a), as upstream's texture_decode.cpp."""
    if fmt == 0:  # RGBA8: A, B, G, R
        return raw[3], raw[2], raw[1], raw[0]
    if fmt == 1:  # RGB8: B, G, R
        return raw[2], raw[1], raw[0], 255
    v = raw[0] | raw[1] << 8 if len(raw) == 2 else raw[0]
    c5 = lambda n: (n << 3) | (n >> 2)
    c6 = lambda n: (n << 2) | (n >> 4)
    c4 = lambda n: n * 17
    if fmt == 2:
        return c5(v >> 11 & 31), c5(v >> 6 & 31), c5(v >> 1 & 31), 255 * (v & 1)
    if fmt == 3:
        return c5(v >> 11 & 31), c6(v >> 5 & 63), c5(v & 31), 255
    if fmt == 4:
        return c4(v >> 12 & 15), c4(v >> 8 & 15), c4(v >> 4 & 15), c4(v & 15)
    if fmt == 5:  # IA8: byte 0 alpha, byte 1 intensity
        return raw[1], raw[1], raw[1], raw[0]
    if fmt == 7:
        return v, v, v, 255
    if fmt == 8:
        return 0, 0, 0, v
    raise ValueError(f"format {fmt} not decoded here")


BYTES = {0: 4, 1: 3, 2: 2, 3: 2, 4: 2, 5: 2, 7: 1, 8: 1}

ETC1_MODIFIERS = [(2, 8), (5, 17), (9, 29), (13, 42), (18, 60), (24, 80), (33, 106), (47, 183)]


def etc1_texel(block, x, y):
    """One texel of a PICA ETC1 block (a little-endian u64), as upstream's ETC1Tile."""
    v = int.from_bytes(block, "little")
    bits = lambda lo, n: v >> lo & ((1 << n) - 1)
    texel = 4 * x + y
    if bits(32, 1):
        x, y = y, x
    if bits(33, 1):
        signed = lambda n: n - 8 if n & 4 else n
        c = [bits(59, 5), bits(51, 5), bits(43, 5)]
        if x >= 2:
            c = [c[0] + signed(bits(56, 3)), c[1] + signed(bits(48, 3)), c[2] + signed(bits(40, 3))]
        c = [(n << 3) | (n >> 2) for n in c]
    else:
        c = [bits(60, 4), bits(52, 4), bits(44, 4)] if x < 2 else [bits(56, 4), bits(48, 4), bits(40, 4)]
        c = [n * 17 for n in c]
    table = bits(37, 3) if x < 2 else bits(34, 3)
    m = ETC1_MODIFIERS[table][bits(texel, 1)]
    if bits(16 + texel, 1):
        m = -m
    return tuple(min(255, max(0, n + m)) for n in c) + (255,)


def decode(guest, w, h, fmt):
    """Texel (x, y) in memory row order -> rgba."""
    if fmt in (12, 13):  # ETC1 (ETC1A4): 8x8 tiles of four 4x4 blocks, x first, 8 (16) bytes
        block = 8 if fmt == 12 else 16  # ETC1A4: the alpha u64 first, 4 bits at 4 * (4x + y)
        out = [[None] * w for _ in range(h)]
        for y in range(h):
            for x in range(w):
                tile = (y // 8) * (w // 8) + x // 8
                sub = (x % 8) // 4 + 2 * ((y % 8) // 4)
                off = (tile * 4 + sub) * block
                rgba = etc1_texel(guest[off + block - 8:off + block], x % 4, y % 4)
                if fmt == 13:
                    a = int.from_bytes(guest[off:off + 8], "little") >> (4 * (4 * (x % 4) + y % 4)) & 15
                    rgba = rgba[:3] + (a * 17,)
                out[y][x] = rgba
        return out
    if fmt in (9, 10, 11):  # IA4 (a byte: I high, A low), I4, A4 (a nibble, the low first)
        out = [[None] * w for _ in range(h)]
        for y in range(h):
            for x in range(w):
                m = ((y // 8) * (w // 8) + x // 8) * 64 + morton(x % 8, y % 8)
                if fmt == 9:
                    b = guest[m]
                    out[y][x] = ((b >> 4) * 17,) * 3 + ((b & 15) * 17,)
                    continue
                n = guest[m // 2] >> (4 * (m & 1)) & 15
                out[y][x] = (n * 17,) * 3 + (255,) if fmt == 10 else (0, 0, 0, n * 17)
        return out
    bpp = BYTES[fmt]
    out = [[None] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            tile = (y // 8) * (w // 8) + x // 8
            off = (tile * 64 + morton(x % 8, y % 8)) * bpp
            out[y][x] = decode_texel(fmt, guest[off:off + bpp])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--png")
    args = ap.parse_args()
    found = False
    for g in sorted(pathlib.Path(args.dir).glob("azahar-tex*-guest.bin")):
        m = NAME.search(g.name)
        if not m:
            continue
        found = True
        n, addr, w, h, fmt, stride = m.group(1), m.group(2), *map(int, m.groups()[2:])
        host_path = g.with_name(g.name.replace("-guest.bin", "-host.bin"))
        byte_path = g.with_name(g.name.replace("-guest.bin", "-host8.bin"))
        if byte_path.exists():
            host = byte_path.read_bytes()
            want = decode(g.read_bytes(), w, h, fmt)
            # A8 and A4 keep the alpha, I8 and I4 the intensity (R).
            lane = 3 if fmt in (8, 11) else 0
            bad = [(x, y, host[y * w + x], want[y][x][lane]) for y in range(h) for x in range(w)
                   if host[y * w + x] != want[y][x][lane]]
            print(f"tex{n} {addr} {w}x{h} format {fmt} byte copy: {len(bad)} of {w * h} texels "
                  f"differ")
            for x, y, got, exp in bad[:8]:
                print(f"  ({x},{y}) host {got} want {exp}")
            continue
        if not host_path.exists():
            # ETC1 levels (and split ETC1A4 ones) are dumped as the GPU's SWIZZLED blocks
            # ("-swizzled", "-alpha"), not RGBA rows: nothing here decodes those.
            print(f"tex{n} {addr} {w}x{h} format {fmt}: no host copy as RGBA rows "
                  f"(compressed blocks), not checked")
            continue
        host = host_path.read_bytes()
        want = decode(g.read_bytes(), w, h, fmt)
        where = {}
        for y in range(h):
            for x in range(w):
                where.setdefault(want[y][x], (x, y))
        bad = []
        for y in range(h):
            for x in range(w):
                o = host_offset(x, y, w, stride)
                got = tuple(host[o:o + 4])
                if got != want[y][x]:
                    bad.append((x, y, got, want[y][x]))
        print(f"tex{n} {addr} {w}x{h} format {fmt} stride {stride}: {len(bad)} of {w * h} "
              f"texels differ")
        for x, y, got, exp in bad[:8]:
            print(f"  ({x},{y}) host {got} want {exp}; host value first at {where.get(got)}")
        if args.png:
            from PIL import Image
            img = Image.new("RGBA", (2 * w + 4, h), (255, 0, 255, 255))
            for y in range(h):
                for x in range(w):
                    img.putpixel((x, y), want[y][x])
                    o = host_offset(x, y, w, stride)
                    img.putpixel((w + 4 + x, y), tuple(host[o:o + 4]))
            out = pathlib.Path(args.png) / f"tex{n}-{addr}.png"
            img.save(out)
            print(f"  decode | host: {out}")
    if not found:
        sys.exit(f"no azahar-tex*-guest.bin in {args.dir}")


if __name__ == "__main__":
    main()
