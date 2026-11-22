#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Willi Ye
# SPDX-License-Identifier: GPL-2.0-or-later

"""picdump_view.py DIR: the picdump files in DIR (sgxd_flags picdump, RasterizerSgxd::DumpPictures)
as PNGs beside them, at their own sizes:
- scene-ADDR-STRIDExROWS-qQ-mM-bB: a scaled picture, the colour buffer's transpose (a row per
  buffer column, STRIDE texels along the buffer's rows), U8888 R the lowest byte: written
  rows x stride as stored (x along the buffer's rows, the screen's long side);
- guest-ADDR-WxH: the buffer's guest bytes, PICA-tiled RGBA8 (8x8 tiles, Morton inside, bytes A B
  G R), as the buffer's H x W columns transposed the same way, at 1x;
- screen-OUT-WxH-qQ-bB-vV: a screen picture, H rows of W texels (the output's transpose), U8888;
- lcd-OUT-WxH-rR: the output's guest bytes, H rows (the screen's columns) of W pixels, RGB8 as
  B G R, transposed to the screen's 400 x 240;
- texf-ADDR-WxH-sSTRIDE-fF-lL-uU-KIND (sgxd_flags texdump): a texture a draw sampled, its host
  copy: rgba and scaled U8888 rows STRIDE texels apart (R the lowest byte), u8 a byte a texel;
  etc1 and cube are left as they are.
A texture's PNG keeps the copy's row order (the PICA's v from the bottom: upside down)."""
import os, re, sys
import numpy as np
from PIL import Image

d = sys.argv[1]
for name in sorted(os.listdir(d)):
    p = os.path.join(d, name)
    raw = np.fromfile(p, np.uint8) if name.endswith('.bin') else None
    if raw is None:
        continue
    if m := re.match(r'azahar-pic(\d+)-scene-([0-9a-f]+)-(\d+)x(\d+)-q(\d+)-m(\d+)-b(\d+)\.bin', name):
        stride, rows, bpp = int(m[3]), int(m[4]), int(m[7])
        a = raw[:stride * rows * bpp].reshape(rows, stride, bpp)
        Image.fromarray(a[:, :, :3]).save(p[:-4] + '.png')
    elif m := re.match(r'azahar-pic(\d+)-guest-([0-9a-f]+)-(\d+)x(\d+)\.bin', name):
        w, h = int(m[3]), int(m[4])
        a = raw[:w * h * 4].reshape(-1, 64, 4)  # tiles of 8x8, Morton inside
        img = np.zeros((h, w, 4), np.uint8)
        tiles_x = w // 8
        for t in range(a.shape[0]):
            ty, tx = divmod(t, tiles_x)
            for i in range(64):
                x = (i & 1) | ((i >> 1) & 2) | ((i >> 2) & 4)
                y = ((i >> 1) & 1) | ((i >> 2) & 2) | ((i >> 3) & 4)
                img[ty * 8 + y, tx * 8 + x] = a[t, i]
        img = img[::-1]  # the PICA's rows bottom up
        rgb = img[:, :, [3, 2, 1]]  # bytes A B G R
        Image.fromarray(np.ascontiguousarray(rgb.transpose(1, 0, 2))).save(p[:-4] + '.png')
    elif m := re.match(r'azahar-pic(\d+)-screen-([0-9a-f]+)-(\d+)x(\d+)-q(\d+)-b(\d+)-v(\d)\.bin', name):
        w, h, bpp = int(m[3]), int(m[4]), int(m[6])
        a = raw[:w * h * bpp].reshape(h, w, bpp)
        Image.fromarray(np.ascontiguousarray(a[:, :, :3])).save(p[:-4] + '.png')
    elif m := re.match(r'azahar-pic(\d+)-lcd-([0-9a-f]+)-(\d+)x(\d+)-r(\d+)\.bin', name):
        w, h, rb = int(m[3]), int(m[4]), int(m[5])
        a = raw[:h * rb].reshape(h, rb)[:, :w * 3].reshape(h, w, 3)[:, :, ::-1]
        Image.fromarray(np.ascontiguousarray(a.transpose(1, 0, 2)[::-1])).save(p[:-4] + '.png')
    elif m := re.match(r'azahar-texf-([0-9a-f]+)-(\d+)x(\d+)-s(\d+)-f(\d+)-l(\d+)-u(\d)-(\w+)\.bin', name):
        w, h, stride, kind = int(m[2]), int(m[3]), int(m[4]), m[8]
        if kind in ('rgba', 'scaled'):
            a = raw[:stride * h * 4]
            if a.size < stride * h * 4:
                continue
            Image.fromarray(np.ascontiguousarray(a.reshape(h, stride, 4)[:, :w, :])).save(p[:-4] + '.png')
        elif kind == 'u8':
            a = raw[:stride * h]
            if a.size < stride * h:
                continue
            Image.fromarray(np.ascontiguousarray(a.reshape(h, stride)[:, :w])).save(p[:-4] + '.png')
        else:
            continue
    else:
        continue
    print(name, '->', os.path.basename(p[:-4] + '.png'))
