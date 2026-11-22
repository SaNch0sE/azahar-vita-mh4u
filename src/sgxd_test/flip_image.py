#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Willi Ye
# SPDX-License-Identifier: GPL-2.0-or-later

"""Test 04's image of a frame (sgxd_test.c flip_layout), and captures compared with it.

    flip_image.py [--frame F] [--black] [--e1 | --e2 | --e3 | --e4 | --e5 [--opaque] | --e6 | --e7 | --f1] [--write OUT.ppm] [CAPTURE ...]

--black is test 04's black=1; --e1 gives test 05's per-draw colours, --e2 test 06's vertex
colours (R, G and B compared; the app itself compares all four bytes), --e3 test 07's planes:
red, green and u in R, G and B, a byte more than 1 from the pixel centre's value differing.
--e4 test 08's texture: the first rectangle's texels exactly, the second's bilinear values to
within 1 (R, G and B). --e5 test 09's blend: each rectangle over what is under it, source alpha,
within 1 (R, G and B); --opaque is its opaque=1, the first rectangle removed under the second.
--e6 test 10's 2000 rectangles, the even ones only inside the frame's clip window, exactly.
--e7 test 11's: test 05's image with every pixel's R and G xored with the frame's immediate.
--f1 test 12's: test 08's texture times test 06's colours, blended as --e5, within 1.

The hold frame is F = -1 (the default). A CAPTURE is a binary PPM (P6, 960x544), a raw 960x544
RGBA file (04-frame.bin, a VCP raw capture) or a PNG. Each prints the pixels that differ; the exit
status is 1 if any do.
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path

W, H = 960, 544
BG = (0x08, 0x10, 0x20)   # FLIP_BG 0xff201008, A8B8G8R8: R, G, B in bytes 0-2
RECT = (0xff, 0x00, 0x00)  # SGXD_RECIPE_TRI 0xff0000ff
# Test 05's palette (sgxd_test.c flip_palette, A8B8G8R8) as R, G, B: frame f's rectangle n takes
# entry (f + n) % 4, the hold frame as frame 0.
PALETTE = [(0x00, 0xa0, 0x00), (0xc0, 0x60, 0x00), (0x60, 0x00, 0xa0), (0xa0, 0xa0, 0x60)]
# Test 06's (flip_palette_e2: 0xc0804020, 0x1f5a96d2, 0xfe01807f, 0x40c0e010).
PALETTE_E2 = [(0x20, 0x40, 0x80), (0xd2, 0x96, 0x5a), (0x7f, 0x80, 0x01), (0x10, 0xe0, 0xc0)]
ALPHA_E2 = [0xc0, 0x1f, 0xfe, 0x40]


def layout(f):
    d = 0 if f < 0 else (f * 4) % 260   # sgxd_test.c flip_layout
    return [(96 + d, 96, 352 + d, 256), (560, 160 + d, 752, 256 + d)]


def layout_e4(f):
    d = 0 if f < 0 else (f * 4) % 260   # sgxd_test.c flip_layout, flip_e4
    return [(96 + d, 96, 224 + d, 224), (560, 16 + d, 816, 272 + d)]


def e6_colour(f, n):
    """sgxd_test.c e6_colour as R, G, B."""
    v = (n * 2654435761 + max(f, 0) * 40503) & 0xffffffff
    v ^= v >> 15
    v = (v * 0x2c1b3c6d) & 0xffffffff
    v ^= v >> 12
    return (v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff)


def e7_imm(f):
    return 0xa5c3 if f < 0 else 1 + f % 0x7fff   # sgxd_test.c e7_imm


def image_e7(f, bg=BG):
    imm = e7_imm(f)
    out = bytearray(image(f, bg, e1=True))
    out[0::3] = bytes(v ^ (imm & 0xff) for v in out[0::3])
    out[1::3] = bytes(v ^ (imm >> 8) for v in out[1::3])
    return bytes(out)


def image_e6(f, bg=BG):
    """Test 10 (sgxd_test.c e6_check): the 50x40 grid, even draws clipped to the window."""
    k = max(f, 0)
    tx0, ty0 = k % 21, (k // 21) % 10
    rows = []
    for y in range(H):
        row = bytearray()
        for x in range(W):
            pixel = bg
            if x >= 5 and y >= 12:
                c, r = (x - 5) // 19, (y - 12) // 13
                n = 1 + r * 50 + c
                clipped = n % 2 == 0 and not (tx0 <= x // 32 <= tx0 + 9 and ty0 <= y // 32 <= ty0 + 7)
                if c < 50 and r < 40 and (x - 5) % 19 < 18 and (y - 12) % 13 < 12 and not clipped:
                    pixel = e6_colour(f, n)
            row += bytes(pixel)
        rows.append(bytes(row))
    return b''.join(rows)


def layout_e5(f):
    d = 0 if f < 0 else (f * 4) % 260   # sgxd_test.c flip_layout, flip_e5
    return [(96 + d, 96, 352 + d, 256), (320, 128 + d // 4, 704, 320 + d // 4)]


def e5_values(f, bg=BG, opaque=False):
    """Test 09 (sgxd_test.c flip_e5_value): {pixel: (r, g, b)}, the background and then each
    rectangle over the pixel blended with its alpha, the layers between rounded to nearest."""
    rects = layout_e5(f)
    colours = [(PALETTE_E2[(max(f, 0) + n) % 4], ALPHA_E2[(max(f, 0) + n) % 4])
               for n in range(len(rects))]
    inside = lambda r, x, y: r[0] <= x < r[2] and r[1] <= y < r[3]
    values = {}
    for y in range(min(r[1] for r in rects), max(r[3] for r in rects)):
        for x in range(min(r[0] for r in rects), max(r[2] for r in rects)):
            covered = [n for n, r in enumerate(rects) if inside(r, x, y)]
            if not covered:
                continue
            if opaque:
                covered = covered[-1:]
            value = bg
            for n in covered:
                (src, a), under = colours[n], [int(c + 0.5) for c in value]
                value = tuple((s * a + d * (255 - a)) / 255 for s, d in zip(src, under))
            values[y * W + x] = value
    return values


TEX = 128


def texel(x, y):
    """sgxd_test.c flip_texel as R, G, B."""
    return (2 * x, 2 * y, (x * 5 + y * 3) & 0xff)


def f1_values(f, bg=BG):
    """Test 12 (sgxd_test.c flip_f1_value): each rectangle's texel times its colour, per byte
    rounded to nearest, then blended as e5_values."""
    d = 0 if f < 0 else (f * 4) % 260   # sgxd_test.c flip_layout, flip_f1
    rects = [(96 + d, 96, 224 + d, 224), (160, 128 + d // 4, 288, 256 + d // 4)]
    k = max(f, 0)
    values = {}
    for y in range(min(r[1] for r in rects), max(r[3] for r in rects)):
        for x in range(min(r[0] for r in rects), max(r[2] for r in rects)):
            value = bg
            covered = False
            for n, r in enumerate(rects):
                if not (r[0] <= x < r[2] and r[1] <= y < r[3]):
                    continue
                covered = True
                tx, ty = x - r[0], y - r[1]
                t = texel(tx, ty) + (0x80 | ((tx ^ ty) & 0x7f),)
                c = PALETTE_E2[(k + n) % 4] + (ALPHA_E2[(k + n) % 4],)
                src = [(tc * cc + 127) // 255 for tc, cc in zip(t, c)]
                a, under = src[3], [int(v + 0.5) for v in value]
                value = tuple((sv * a + dv * (255 - a)) / 255 for sv, dv in zip(src[:3], under))
            if covered:
                values[y * W + x] = value
    return values


def e4_values(f):
    """Test 08 (sgxd_test.c flip_check, flip_e4_value): {pixel: ((r, g, b), tolerance)}."""
    values = {}
    (ax0, ay0, ax1, ay1), (bx0, by0, bx1, by1) = layout_e4(f)
    for y in range(ay0, ay1):
        for x in range(ax0, ax1):
            values[y * W + x] = (texel(x - ax0, y - ay0), 0)
    clamp = lambda v: min(max(v, 0), TEX - 1)
    for y in range(by0, by1):
        ty = (y + 0.5 - by0) / 2 - 0.5
        v0 = int(ty + 1) - 1
        fy = ty - v0
        for x in range(bx0, bx1):
            tx = (x + 0.5 - bx0) / 2 - 0.5
            u0 = int(tx + 1) - 1
            fx = tx - u0
            t = [texel(clamp(u0 + (k & 1)), clamp(v0 + (k >> 1))) for k in range(4)]
            values[y * W + x] = (tuple((t[0][c] * (1 - fx) + t[1][c] * fx) * (1 - fy) +
                                       (t[2][c] * (1 - fx) + t[3][c] * fx) * fy
                                       for c in range(3)), 1.5)
    return values


def image(f, bg=BG, e1=False, e2=False):
    rows = []
    rects = layout(f)
    palette = PALETTE_E2 if e2 else PALETTE
    colours = [palette[(max(f, 0) + n) % 4] if e1 or e2 else RECT for n in range(len(rects))]
    for y in range(H):
        row = bytearray()
        for x in range(W):
            pixel = bg
            for (x0, y0, x1, y1), colour in zip(rects, colours):
                if x0 <= x < x1 and y0 <= y < y1:
                    pixel = colour   # the later draw wins
            row += bytes(pixel)
        rows.append(bytes(row))
    return b''.join(rows)


def e3_values(f):
    """Test 07 (sgxd_test.c flip_e3_value): {pixel: (r, g, u)} inside the rectangles, the later
    rectangle winning, as unrounded unorm values at the pixel centre."""
    values = {}
    for x0, y0, x1, y1 in layout(f):
        for y in range(y0, y1):
            fy = (y + 0.5 - y0) / (y1 - y0)
            for x in range(x0, x1):
                fx = (x + 0.5 - x0) / (x1 - x0)
                values[y * W + x] = (fx * 255, fy * 255, (1 - fx) * 255)
    return values


def read_png(data):
    chunks, at = [], 8
    while at < len(data):
        length, kind = struct.unpack('>I4s', data[at:at + 8])
        chunks.append((kind, data[at + 8:at + 8 + length]))
        at += 12 + length
    header = dict(chunks)[b'IHDR']
    width, height, depth, colour = struct.unpack('>IIBB', header[:10])
    if depth != 8 or colour not in (2, 6):
        raise ValueError(f'PNG depth {depth} colour type {colour}')
    step = 3 if colour == 2 else 4
    raw = zlib.decompress(b''.join(body for kind, body in chunks if kind == b'IDAT'))
    stride, out, previous = width * step, bytearray(), bytearray(width * step)
    for y in range(height):
        kind, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - step] if i >= step else 0
            b, c = previous[i], previous[i - step] if i >= step else 0
            p = a + b - c
            pred = [0, a, b, (a + b) // 2,
                    a if abs(p - a) <= abs(p - b) and abs(p - a) <= abs(p - c) else
                    b if abs(p - b) <= abs(p - c) else c][kind]
            line[i] = (line[i] + pred) & 0xff
        out += b''.join(bytes(line[x * step:x * step + 3]) for x in range(width))
        previous = line
    return width, height, bytes(out)


def read_capture(path):
    data = Path(path).read_bytes()
    if data.startswith(b'P6'):
        fields, at = [], 2
        while len(fields) < 3:
            while data[at:at + 1].isspace():
                at += 1
            end = at
            while not data[end:end + 1].isspace():
                end += 1
            fields.append(int(data[at:end]))
            at = end
        return fields[0], fields[1], data[at + 1:at + 1 + fields[0] * fields[1] * 3]
    if data.startswith(b'\x89PNG'):
        return read_png(data)
    if len(data) == W * H * 4:
        return W, H, b''.join(data[n:n + 3] for n in range(0, len(data), 4))
    raise ValueError(f'{path}: not a P6 PPM, a PNG or a {W}x{H} RGBA raw file')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--frame', type=int, default=-1)
    parser.add_argument('--write', type=Path)
    parser.add_argument('--black', action='store_true', help="test 04's black=1")
    parser.add_argument('--e1', action='store_true', help="test 05's colours")
    parser.add_argument('--e2', action='store_true', help="test 06's colours")
    parser.add_argument('--e3', action='store_true', help="test 07's planes, within 1")
    parser.add_argument('--e4', action='store_true', help="test 08's texture")
    parser.add_argument('--e5', action='store_true', help="test 09's blend")
    parser.add_argument('--opaque', action='store_true', help="test 09's opaque=1")
    parser.add_argument('--e6', action='store_true', help="test 10's 2000 draws")
    parser.add_argument('--e7', action='store_true', help="test 11's xored image")
    parser.add_argument('--f1', action='store_true', help="test 12's modulate and blend")
    parser.add_argument('captures', nargs='*')
    args = parser.parse_args()
    want = image(args.frame, (0, 0, 0) if args.black else BG, args.e1, args.e2)
    if args.e6:
        want = image_e6(args.frame, (0, 0, 0) if args.black else BG)
    elif args.e7:
        want = image_e7(args.frame, (0, 0, 0) if args.black else BG)
    values = None
    if args.e3:
        values = {n: (v, 1.5) for n, v in e3_values(args.frame).items()}
    elif args.e4:
        values = e4_values(args.frame)
    elif args.f1:
        values = {n: (v, 1.5) for n, v in
                  f1_values(args.frame, (0, 0, 0) if args.black else BG).items()}
    elif args.e5:
        values = {n: (v, 1.5) for n, v in
                  e5_values(args.frame, (0, 0, 0) if args.black else BG, args.opaque).items()}
    if values is not None:
        # The background, and the rectangles as their rounded values, for --write.
        want = bytearray(bytes((0, 0, 0) if args.black else BG) * (W * H))
        for n, (v, _) in values.items():
            want[n * 3:n * 3 + 3] = bytes(min(255, int(c + 0.5)) for c in v)
        want = bytes(want)
    if args.write:
        args.write.write_bytes(b'P6\n%d %d\n255\n' % (W, H) + want)
    bad = 0
    for path in args.captures:
        width, height, got = read_capture(path)
        if (width, height) != (W, H):
            print(f'{path}: {width}x{height}, not {W}x{H}')
            bad += 1
            continue
        if values is not None:
            differ = [n for n in range(W * H)
                      if (n in values and any(abs(got[n * 3 + c] - values[n][0][c]) >
                                              max(values[n][1], 0.5) for c in range(3))) or
                      (n not in values and got[n * 3:n * 3 + 3] != want[n * 3:n * 3 + 3])]
        else:
            differ = [n for n in range(W * H) if got[n * 3:n * 3 + 3] != want[n * 3:n * 3 + 3]]
        first = f', the first at ({differ[0] % W}, {differ[0] // W}) = ' \
                f'{got[differ[0] * 3:differ[0] * 3 + 3].hex()}' if differ else ''
        print(f'{path}: {len(differ)} of {W * H} pixels differ{first}')
        bad += bool(differ)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
