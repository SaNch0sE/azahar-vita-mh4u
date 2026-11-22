# SPDX-FileCopyrightText: 2026 Willi Ye
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The RomFS file fd_test reads: 32 MiB, word i = i ^ 0x5a5a5a5a (little endian). Run before make.
import os, struct
os.makedirs('romfs', exist_ok=True)
with open('romfs/data.bin', 'wb') as f:
    for base in range(0, 8 << 20, 1 << 16):
        f.write(b''.join(struct.pack('<I', i ^ 0x5a5a5a5a) for i in range(base, base + (1 << 16))))
