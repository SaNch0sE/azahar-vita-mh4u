// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// The Z3DS container, laid out as azahar writes it (Z3DSWriteIOFile):
//
//   0x00  header (0x20 bytes): "Z3DS", the underlying file's magic, version 1, reserved,
//         header size, metadata size, compressed size (u64), uncompressed size (u64)
//   0x20  metadata: version 1, then {type 1, name length, data length, name, data} items and
//         an end item, zero padded to 0x10
//   ...   a zstd seekable stream: one zstd frame per frame-size bytes of the ROM, then the seek
//         table in a skippable frame. azahar reads it from the end of the file, so the stream
//         ends the file.

export const HEADER_SIZE = 0x20;
export const DEFAULT_FRAME_SIZE = 256 * 1024;
export const DEFAULT_CIA_FRAME_SIZE = 32 * 1024 * 1024;
export const DEFAULT_LEVEL = 3;

const SKIPPABLE_SEEK_TABLE_MAGIC = 0x184d2a5e; // ZSTD_MAGIC_SKIPPABLE_START | 0xE
const SEEKABLE_MAGIC = 0x8f92eab1;

export function header(underlyingMagic, metadataSize, compressedSize, uncompressedSize) {
    const out = new Uint8Array(HEADER_SIZE);
    const view = new DataView(out.buffer);
    out.set([0x5a, 0x33, 0x44, 0x53]); // "Z3DS"
    out.set(underlyingMagic, 4);
    out[8] = 1; // version
    view.setUint16(10, HEADER_SIZE, true);
    view.setUint32(12, metadataSize, true);
    view.setBigUint64(16, BigInt(compressedSize), true);
    view.setBigUint64(24, BigInt(uncompressedSize), true);
    return out;
}

// items: [name, Uint8Array | string] pairs.
export function metadata(items) {
    const encoder = new TextEncoder();
    const parts = [new Uint8Array([1])];
    for (const [name, value] of items) {
        const nameBytes = encoder.encode(name);
        const data = typeof value === "string" ? encoder.encode(value) : value;
        if (nameBytes.length > 0xff || data.length > 0xffff) {
            throw new Error(`metadata item ${name} is too large`);
        }
        const item = new Uint8Array(4 + nameBytes.length + data.length);
        item[0] = 1; // binary
        item[1] = nameBytes.length;
        new DataView(item.buffer).setUint16(2, data.length, true);
        item.set(nameBytes, 4);
        item.set(data, 4 + nameBytes.length);
        parts.push(item);
    }
    parts.push(new Uint8Array(4)); // end item
    const length = parts.reduce((n, p) => n + p.length, 0);
    const out = new Uint8Array((length + 0xf) & ~0xf);
    let at = 0;
    for (const p of parts) {
        out.set(p, at);
        at += p.length;
    }
    return out;
}

// The seek table frame for frames of the given compressed and decompressed sizes, without
// checksums (azahar starts its stream with the checksum flag clear).
export function seekTable(compressedSizes, decompressedSizes) {
    const count = compressedSizes.length;
    const out = new Uint8Array(8 + count * 8 + 9);
    const view = new DataView(out.buffer);
    view.setUint32(0, SKIPPABLE_SEEK_TABLE_MAGIC, true);
    view.setUint32(4, out.length - 8, true);
    for (let i = 0; i < count; i++) {
        view.setUint32(8 + i * 8, compressedSizes[i], true);
        view.setUint32(12 + i * 8, decompressedSizes[i], true);
    }
    view.setUint32(8 + count * 8, count, true);
    out[12 + count * 8] = 0; // descriptor: no checksums
    view.setUint32(13 + count * 8, SEEKABLE_MAGIC, true);
    return out;
}

// azahar closes a frame as it fills and closes the last one when the stream ends, so the last
// frame holds what is left: nothing when the ROM's size is a multiple of the frame size.
export function frameCount(size, frameSize) {
    return Math.floor(size / frameSize) + 1;
}

// strftime("%Y-%m-%dT%H:%M:%SZ") in UTC, as azahar stamps "date".
export function date(now = new Date()) {
    return now.toISOString().replace(/\.\d{3}Z$/, "Z");
}

export function isZ3DS(first4) {
    return first4[0] === 0x5a && first4[1] === 0x33 && first4[2] === 0x44 && first4[3] === 0x53;
}
