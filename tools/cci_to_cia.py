#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Willi Ye
# SPDX-License-Identifier: GPL-2.0-or-later

"""Decrypted CCI -> unsigned CIA, as test input for azahar's CIA install.

Contents are the CCI's NCCH partitions (0 = game, 1 = manual, 2 = download play) in order;
signatures and the certificate chain are zeros (azahar's install checks neither).
"""
import hashlib
import struct
import sys

MEDIA = 0x200
SIG_RSA2048 = 0x00010004
SIG_BLOCK = 4 + 0x100 + 0x3C  # type, signature, padding to 0x40


def align(value, to):
    return (value + to - 1) // to * to


def signed(body):
    return struct.pack(">I", SIG_RSA2048) + bytes(0x100 + 0x3C) + body


def ticket(title_id, version):
    body = bytearray(0x164)
    body[0:0x40] = b"Root-CA00000003-XS0000000c".ljust(0x40, b"\0")
    body[0x7C] = 1  # version
    struct.pack_into(">Q", body, 0x90, 0)  # ticket id
    struct.pack_into(">Q", body, 0x9C, title_id)
    struct.pack_into(">H", body, 0xA6, version)
    # The content index every CIA tool writes: one rights field, all 1024 contents allowed.
    index = bytearray(0xAC)
    struct.pack_into(">HHIIHHI", index, 0, 1, 0x14, 0xAC, 0x14, 1, 0x14, 0)
    struct.pack_into(">IIIIHH", index, 0x14, 0x28, 1, 0x84, 0x84, 3, 0)
    struct.pack_into(">HH", index, 0x28, 0, 0)
    index[0x2C:0xAC] = b"\xff" * 0x80
    return signed(bytes(body) + bytes(index))


def tmd(title_id, version, save_size, chunks):
    records = b"".join(
        struct.pack(">IHHQ", cid, index, 0, size) + digest for cid, index, size, digest in chunks)
    info = bytearray(0x24 * 64)
    struct.pack_into(">HH", info, 0, 0, len(chunks))
    info[4:0x24] = hashlib.sha256(records).digest()
    body = bytearray(0xC4)
    body[0:0x40] = b"Root-CA00000003-CP0000000b".ljust(0x40, b"\0")
    body[0x40] = 1  # version
    struct.pack_into(">QQIHI", body, 0x44, 0, title_id, 0x40, 0, save_size)
    struct.pack_into(">IHHH", body, 0x98, 0, version, len(chunks), 0)
    body[0xA4:0xC4] = hashlib.sha256(info).digest()
    return signed(bytes(body) + bytes(info) + records)


def exefs_file(ncch, name):
    exefs_offset, = struct.unpack_from("<I", ncch, 0x1A0)
    base = exefs_offset * MEDIA
    for slot in range(10):
        entry_name, offset, size = struct.unpack_from("<8sII", ncch, base + slot * 16)
        if entry_name.rstrip(b"\0") == name:
            return ncch[base + 0x200 + offset:base + 0x200 + offset + size]
    return None


def main(cci_path, cia_path):
    with open(cci_path, "rb") as f:
        cci = f.read()
    if cci[0x100:0x104] != b"NCSD":
        sys.exit(f"{cci_path}: no NCSD header")
    # Partitions 0-2 (game, manual, download play) become contents of the same index; 6 and 7
    # hold system updates, which a CIA leaves out.
    partitions = []
    for index in range(3):
        offset, size = struct.unpack_from("<II", cci, 0x120 + index * 8)
        if size == 0:
            continue
        data = cci[offset * MEDIA:(offset + size) * MEDIA]
        if data[0x100:0x104] != b"NCCH":
            sys.exit(f"partition {index}: no NCCH header")
        if not data[0x18F] & 0x4:
            sys.exit(f"partition {index} is encrypted")
        partitions.append((index, data))

    if not partitions or partitions[0][0] != 0:
        sys.exit("no game partition")
    game = partitions[0][1]
    title_id, = struct.unpack_from("<Q", game, 0x118)
    exheader = game[0x200:0x600]
    # The game's revision (the remaster version); the NCCH header's 0x112 is its format's version.
    version, = struct.unpack_from("<H", exheader, 0xE)
    save_size, = struct.unpack_from("<Q", exheader, 0x1C0)
    chunks = [(i, i, len(p), hashlib.sha256(p).digest()) for i, p in partitions]

    icon = exefs_file(game, b"icon")
    meta = bytearray(0x400)
    meta[0:0x180] = exheader[0x40:0x1C0]  # dependency list
    struct.pack_into("<I", meta, 0x300, struct.unpack_from("<I", exheader, 0x208)[0])
    meta = bytes(meta) + (icon if icon is not None else b"")

    cert = bytes(0xA00)
    tik = ticket(title_id, version)
    tm = tmd(title_id, version, min(save_size, 0xFFFFFFFF), chunks)
    content_size = sum(len(p) for _, p in partitions)

    header = bytearray(0x2020)
    struct.pack_into("<IHHIIIIQ", header, 0, 0x2020, 0, 0, len(cert), len(tik), len(tm),
                     len(meta), content_size)
    for i, _ in partitions:
        header[0x20 + i // 8] |= 0x80 >> (i % 8)

    with open(cia_path, "wb") as out:
        for section in (bytes(header), cert, tik, tm):
            out.write(section)
            out.write(bytes(align(len(section), 0x40) - len(section)))
        for _, p in partitions:
            out.write(p)
        out.write(bytes(align(content_size, 0x40) - content_size))
        out.write(meta)
    print(f"{cia_path}: title {title_id:016X} v{version}, contents {[i for i, _ in partitions]}, "
          f"{content_size >> 20} MiB, icon {'yes' if icon else 'no'}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: cci_to_cia.py IN.cci OUT.cia")
    main(sys.argv[1], sys.argv[2])
