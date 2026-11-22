#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Willi Ye
# SPDX-License-Identifier: GPL-2.0-or-later

"""texdump.py DIR: the dumptex pairs (azahar-texN-ADDR-WxH-fF-sS-{guest,host}.bin) as PNGs.

The guest bytes are decoded from the PICA's 8x8 Morton tiles (upstream's texture_decode.cpp:
the 4-bit formats a nibble a texel, the low one first; ETC1 and ETC1A4 are left black); the
host copy is U8888 rows of the width, shown as its four bytes. Each pair goes side by side into
ADDR-WxH-fF.png, the guest picture on the left, both with row 0 at the top as stored.
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

BITS = {0: 32, 1: 24, 2: 16, 3: 16, 4: 16, 5: 16, 6: 16, 7: 8, 8: 8, 9: 8, 10: 4, 11: 4}


def morton(x, y):
    m = 0
    for b in range(3):
        m |= ((x >> b) & 1) << (2 * b) | ((y >> b) & 1) << (2 * b + 1)
    return m


def texel_index(w, h):
    """The texel number in the guest bytes of each (row, column), row 0 the first tile row."""
    ys, xs = np.mgrid[0:h, 0:w]
    tile = (ys // 8) * (w // 8) + xs // 8
    m = np.vectorize(morton)(xs % 8, ys % 8)
    return tile * 64 + m


def decode(raw, w, h, fmt):
    idx = texel_index(w, h)
    bits = BITS[fmt]
    if bits == 4:
        b = np.frombuffer(raw, np.uint8)
        nib = np.stack([b & 15, b >> 4], 1).reshape(-1)
        v = nib[idx].astype(np.uint8) * 17
        return np.stack([v, v, v, np.full_like(v, 255)], -1)
    px = np.frombuffer(raw[: w * h * bits // 8], np.uint8).reshape(-1, bits // 8)[idx]
    if fmt == 0:  # RGBA8: bytes A B G R
        return px[..., ::-1]
    if fmt == 1:  # RGB8: B G R
        return np.concatenate([px[..., ::-1], np.full(px.shape[:2] + (1,), 255, np.uint8)], -1)
    if bits == 16:
        v = px[..., 0].astype(np.uint16) | px[..., 1].astype(np.uint16) << 8
        if fmt == 2:  # RGB5A1
            f = [(v >> 11) & 31, (v >> 6) & 31, (v >> 1) & 31]
            rgb = [c * 255 // 31 for c in f]
            a = (v & 1) * 255
        elif fmt == 3:  # RGB565
            rgb = [((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31]
            a = np.full_like(v, 255)
        elif fmt == 4:  # RGBA4
            rgb = [((v >> s) & 15) * 17 for s in (12, 8, 4)]
            a = (v & 15) * 17
        else:  # IA8 / RG8: show both bytes
            rgb = [px[..., 1], px[..., 0], np.zeros_like(px[..., 0])]
            a = np.full_like(v, 255)
        return np.stack([np.asarray(c, np.uint8) for c in rgb] + [np.asarray(a, np.uint8)], -1)
    v = px[..., 0]
    if fmt == 9:  # IA4: intensity high nibble
        i = (v >> 4) * 17
        return np.stack([i, i, i, (v & 15) * 17], -1).astype(np.uint8)
    return np.stack([v, v, v, np.full_like(v, 255)], -1)


def main():
    d = Path(sys.argv[1])
    for g in sorted(d.glob('azahar-tex*-guest.bin')):
        parts = g.name.split('-')
        addr, size, fmt = parts[2], parts[3], int(parts[4][1:])
        w, h = map(int, size.split('x'))
        host = g.with_name(g.name.replace('-guest.bin', '-host.bin'))
        if not host.exists():
            continue
        hb = np.frombuffer(host.read_bytes(), np.uint8)[: w * h * 4].reshape(h, w, 4)
        # ETC1 and ETC1A4 black.
        gp = decode(g.read_bytes(), w, h, fmt) if fmt in BITS else np.zeros_like(hb)
        # The host copy's bytes as RGB, and its fourth byte as a grey strip beside it.
        hp = hb.copy()
        hp[..., 3] = 255
        ha = np.repeat(hb[..., 3:4], 4, -1)
        ha[..., 3] = 255
        gp = gp.copy()
        gp[..., 3] = 255
        out = np.concatenate([gp, hp, ha], 1)
        Image.fromarray(out, 'RGBA').save(d / f'{addr}-{size}-f{fmt}.png')
        print(f'{addr} {size} f{fmt}')


if __name__ == '__main__':
    main()
