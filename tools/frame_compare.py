#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Willi Ye
# SPDX-License-Identifier: GPL-2.0-or-later

"""frame_compare.py A B [--tolerance N] [--diff OUT.png]: two 960x544 display frames (PPM, PNG,
or raw A8B8G8R8 words as the sgxd tests write them) compared per channel.

Prints the pixels whose largest channel difference is 0, 1..N (within the tolerance) and above
it, the bounding box of the ones above, and a coarse 32x32-tile map of them ('.' none, a digit
the tile's count in tenths of its 1024 pixels, '#' all). --diff writes the pixels above the
tolerance in white over a dimmed copy of A. Exit status 1 when any pixel is above the tolerance."""
import argparse
import sys

from PIL import Image, ImageChops

W, H = 960, 544


def load(path):
    if path.endswith('.bin'):
        data = open(path, 'rb').read()[:W * H * 4]
        return Image.frombytes('RGBA', (W, H), data).convert('RGB')
    return Image.open(path).convert('RGB')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('a')
    p.add_argument('b')
    p.add_argument('--tolerance', type=int, default=1)
    p.add_argument('--diff')
    args = p.parse_args()
    a, b = load(args.a), load(args.b)
    if a.size != b.size:
        sys.exit(f'sizes differ: {a.size} {b.size}')
    diff = ImageChops.difference(a, b)
    # The largest channel difference per pixel.
    r, g, bl = diff.split()
    worst = ImageChops.lighter(ImageChops.lighter(r, g), bl)
    values = list(worst.tobytes())
    w, h = a.size
    same = sum(1 for v in values if v == 0)
    near = sum(1 for v in values if 0 < v <= args.tolerance)
    far = [(i % w, i // w) for i, v in enumerate(values) if v > args.tolerance]
    print(f'{args.a} vs {args.b}: {same} same, {near} within {args.tolerance}, {len(far)} above')
    if far:
        xs = [x for x, _ in far]
        ys = [y for _, y in far]
        print(f'above the tolerance: x {min(xs)}..{max(xs)}, y {min(ys)}..{max(ys)}, '
              f'largest difference {max(values)}')
        tiles = {}
        for x, y in far:
            tiles[(x // 32, y // 32)] = tiles.get((x // 32, y // 32), 0) + 1
        for ty in range((h + 31) // 32):
            row = ''
            for tx in range((w + 31) // 32):
                n = tiles.get((tx, ty), 0)
                row += '.' if n == 0 else '#' if n >= 1024 else str(min(9, n * 10 // 1024))
            print(row)
    if args.diff:
        out = a.point(lambda v: v // 3)
        mask = worst.point(lambda v: 255 if v > args.tolerance else 0)
        out.paste((255, 255, 255), mask=mask)
        out.save(args.diff)
    return 1 if far else 0


if __name__ == '__main__':
    sys.exit(main())
