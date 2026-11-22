// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// Reads a 3DS ROM's headers the way azahar's loaders do, checks that azahar can compress it and
// that it is decrypted, and gathers the metadata azahar stores with a compressed ROM.
//
// A source is { name, size, read(offset, length) -> Promise<Uint8Array> }.

import { DEFAULT_CIA_FRAME_SIZE, DEFAULT_FRAME_SIZE, isZ3DS } from "./z3ds.js";

const NCSD_BLOCK = 0x200;
const SMDH_SIZE = 0x36c0;

export class RomError extends Error {
    constructor(message, checks) {
        super(message);
        this.checks = checks;
    }
}

const DECRYPT_HINT =
    "Decrypt it on a 3DS first (GodMode9 can), then choose the decrypted file here.";

function view(bytes) {
    return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
}

function ascii(bytes, offset, length) {
    let s = "";
    for (let i = offset; i < offset + length && bytes[i]; i++) {
        s += String.fromCharCode(bytes[i]);
    }
    return s;
}

function utf16(bytes, offset, length) {
    const s = new TextDecoder("utf-16le").decode(bytes.subarray(offset, offset + length));
    const end = s.indexOf("\0");
    return (end < 0 ? s : s.slice(0, end)).trim();
}

function hex64(value) {
    return value.toString(16).toUpperCase().padStart(16, "0");
}

async function sha256(bytes) {
    return new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
}

function sameBytes(a, b) {
    return a.length === b.length && a.every((x, i) => x === b[i]);
}

async function readExact(source, offset, length) {
    if (offset + length > source.size) {
        return null;
    }
    const bytes = await source.read(offset, length);
    return bytes.length === length ? bytes : null;
}

// Service::AM::TitleInfo: tid, size, version, unused, type.
function titleInfo(tid, size, version, type) {
    const out = new Uint8Array(0x18);
    const v = view(out);
    v.setBigUint64(0, BigInt.asUintN(64, BigInt(tid)), true);
    v.setBigUint64(8, BigInt(size), true);
    v.setUint16(16, version, true);
    v.setUint32(20, type, true);
    return out;
}

// --- SMDH ----------------------------------------------------------------------------------

const LANGUAGES = ["Japanese", "English", "French", "German", "Italian", "Spanish",
    "Chinese (Simplified)", "Korean", "Dutch", "Portuguese", "Russian", "Chinese (Traditional)"];
const REGIONS = ["Japan", "North America", "Europe", "Australia", "China", "Korea", "Taiwan"];

// The large icon: 48x48 RGB565 in 8x8 tiles, each tile in Morton order.
function decodeIcon(data, size) {
    const rgba = new Uint8ClampedArray(size * size * 4);
    let i = 0;
    for (let ty = 0; ty < size; ty += 8) {
        for (let tx = 0; tx < size; tx += 8) {
            for (let k = 0; k < 64; k++, i++) {
                const x = tx + ((k & 1) | ((k >> 1) & 2) | ((k >> 2) & 4));
                const y = ty + (((k >> 1) & 1) | ((k >> 2) & 2) | ((k >> 3) & 4));
                const px = data[i * 2] | (data[i * 2 + 1] << 8);
                const r = (px >> 11) & 0x1f;
                const g = (px >> 5) & 0x3f;
                const b = px & 0x1f;
                const o = (y * size + x) * 4;
                rgba[o] = (r << 3) | (r >> 2);
                rgba[o + 1] = (g << 2) | (g >> 4);
                rgba[o + 2] = (b << 3) | (b >> 2);
                rgba[o + 3] = 255;
            }
        }
    }
    return { width: size, height: size, rgba };
}

function parseSmdh(bytes) {
    if (!bytes || bytes.length < SMDH_SIZE || ascii(bytes, 0, 4) !== "SMDH") {
        return null;
    }
    const titles = [];
    for (let i = 0; i < 16; i++) {
        const o = 8 + i * 0x200;
        titles.push({
            short: utf16(bytes, o, 0x80),
            long: utf16(bytes, o + 0x80, 0x100),
            publisher: utf16(bytes, o + 0x180, 0x80),
        });
    }
    // English first, as azahar's game list does, then the first language that has a title.
    const title = titles[1].short ? titles[1] : titles.find((t) => t.short) ?? titles[1];
    const lockout = view(bytes).getUint32(0x2018, true);
    let region;
    if ((lockout & 0x7f) === 0x7f) {
        region = "Region free";
    } else {
        region = REGIONS.filter((_, i) => lockout & (1 << i)).join(", ") || "None";
    }
    const languages = titles.slice(0, LANGUAGES.length)
        .map((t, i) => (t.short ? LANGUAGES[i] : null)).filter(Boolean);
    return {
        bytes: bytes.subarray(0, SMDH_SIZE),
        title: title.short,
        longTitle: title.long.replace(/\n/g, " "),
        publisher: title.publisher,
        region,
        languages,
        icon: decodeIcon(bytes.subarray(0x24c0, 0x24c0 + 0x1200), 48),
    };
}

// --- NCCH ----------------------------------------------------------------------------------

// The NCCH at base, length bytes long (its NCSD partition, or the whole file).
async function readNcch(source, base, length) {
    const h = await readExact(source, base, 0x200);
    if (!h || ascii(h, 0x100, 4) !== "NCCH") {
        return null;
    }
    const v = view(h);
    const flags = h.subarray(0x188, 0x190);
    const contentSize = v.getUint32(0x104, true);
    // A prototype NCCH counts its sizes in bytes (NCCHContainer::Load).
    const unit = contentSize === length ? 1 : 0x200 * 2 ** flags[6];
    return {
        base,
        header: h,
        contentSize,
        contentUnitSize: 0x200 * 2 ** flags[6],
        makerCode: ascii(h, 0x110, 2),
        version: v.getUint16(0x112, true),
        programId: v.getBigUint64(0x118, true),
        productCode: ascii(h, 0x150, 0x10),
        exheaderSize: v.getUint32(0x180, true),
        noCrypto: (flags[7] & 0x4) !== 0,
        noRomfs: (flags[7] & 0x2) !== 0,
        exefs: {
            offset: v.getUint32(0x1a0, true) * unit,
            size: v.getUint32(0x1a4, true) * unit,
            hashed: v.getUint32(0x1a8, true) * unit,
        },
        romfs: {
            offset: v.getUint32(0x1b0, true) * unit,
            size: v.getUint32(0x1b4, true) * unit,
            hashed: v.getUint32(0x1b8, true) * unit,
        },
        end: base + contentSize * unit,
    };
}

async function readExefs(source, ncch) {
    if (!ncch.exefs.size) {
        return [];
    }
    const base = ncch.base + ncch.exefs.offset;
    const h = await readExact(source, base, 0x200);
    if (!h) {
        return null;
    }
    const v = view(h);
    const files = [];
    for (let i = 0; i < 10; i++) {
        const size = v.getUint32(i * 0x10 + 12, true);
        if (size) {
            files.push({
                name: ascii(h, i * 0x10, 8),
                offset: base + 0x200 + v.getUint32(i * 0x10 + 8, true),
                size,
                hash: h.subarray(0xc0 + (9 - i) * 0x20, 0xe0 + (9 - i) * 0x20),
            });
        }
    }
    return files;
}

// Hashes in the NCCH header and the ExeFS cover the decrypted data, so they show whether the
// decryption was right. The RomFS's level hashes would need the whole RomFS: its header's hash
// is checked here, its data stays as it is.
async function verifyNcch(source, ncch) {
    const h = ncch.header;
    if (ncch.exheaderSize) {
        const exheader = await readExact(source, ncch.base + 0x200, 0x400);
        if (!exheader || !sameBytes(await sha256(exheader), h.subarray(0x160, 0x180))) {
            return "its extended header";
        }
    }
    if (ncch.exefs.size) {
        const region = await readExact(source, ncch.base + ncch.exefs.offset, ncch.exefs.hashed);
        if (!region || !sameBytes(await sha256(region), h.subarray(0x1c0, 0x1e0))) {
            return "its ExeFS header";
        }
        const files = await readExefs(source, ncch);
        for (const file of files) {
            const data = await readExact(source, file.offset, file.size);
            if (!data || !sameBytes(await sha256(data), file.hash)) {
                return `its ExeFS file ${file.name}`;
            }
        }
    }
    if (ncch.romfs.size && !ncch.noRomfs) {
        const region = await readExact(source, ncch.base + ncch.romfs.offset, ncch.romfs.hashed);
        if (!region || !sameBytes(await sha256(region), h.subarray(0x1e0, 0x200))) {
            return "its RomFS header";
        }
    }
    return null;
}

async function ncchSmdh(source, ncch) {
    const files = await readExefs(source, ncch);
    const icon = files?.find((f) => f.name === "icon" && f.size >= SMDH_SIZE);
    return icon ? parseSmdh(await readExact(source, icon.offset, SMDH_SIZE)) : null;
}

function ncchInfo(ncch) {
    return {
        titleId: hex64(ncch.programId),
        productCode: ncch.productCode,
        makerCode: ncch.makerCode,
        version: `${ncch.version}`,
        new3ds: ncch.productCode.startsWith("KTR-"),
    };
}

const PARTITIONS = { 0: "game", 1: "manual", 2: "download play", 6: "New 3DS update data",
    7: "update data" };

// --- inspect -------------------------------------------------------------------------------

export async function inspect(source) {
    const checks = [];
    const pass = (label, detail) => checks.push({ state: "ok", label, detail });
    const fail = (label, message) => {
        checks.push({ state: "fail", label, detail: message });
        throw new RomError(message, checks);
    };

    if (source.size < 0x200) {
        fail("A 3DS ROM", "This file is too small to be a 3DS ROM. Choose a .3ds, .cci, .cxi, " +
            ".3dsx or .cia file.");
    }
    const first = await source.read(0, 0x200);
    if (isZ3DS(first)) {
        fail("Not compressed yet", "This file is already compressed: azahar opens it as it is.");
    }
    if (ascii(first, 0, 4) === "3DSX") {
        return inspect3dsx(source, first, checks, pass, fail);
    }
    if (first[0] === 0x7f && ascii(first, 1, 3) === "ELF") {
        fail("A 3DS ROM", "azahar runs ELF files as they are and can't compress them.");
    }
    const magic = ascii(first, 0x100, 4);
    if (magic === "NCSD") {
        return inspectNcsd(source, first, checks, pass, fail);
    }
    if (magic === "NCCH") {
        return inspectCxi(source, checks, pass, fail);
    }
    if (view(first).getUint32(0, true) === 0x2020) {
        return inspectCia(source, checks, pass, fail);
    }
    fail("A 3DS ROM", "This isn't a 3DS ROM azahar can compress. Choose a .3ds, .cci, .cxi, " +
        ".3dsx or .cia file.");
}

function baseName(name) {
    return name.replace(/\.[^./\\]*$/, "");
}

async function checkNcch(source, ncch, label, fail) {
    if (!ncch.noCrypto) {
        fail("Decrypted", `The ${label} is encrypted. ${DECRYPT_HINT}`);
    }
    const bad = await verifyNcch(source, ncch);
    if (bad) {
        fail("Contents match their hashes",
            `The ${label}'s data doesn't match ${bad}'s hash: the file is damaged or wasn't ` +
            "decrypted correctly. Dump and decrypt it again.");
    }
}

async function inspectNcsd(source, first, checks, pass, fail) {
    const v = view(first);
    const partitions = [];
    for (let i = 0; i < 8; i++) {
        const offset = v.getUint32(0x120 + i * 8, true) * NCSD_BLOCK;
        const size = v.getUint32(0x124 + i * 8, true) * NCSD_BLOCK;
        if (size) {
            partitions.push({ index: i, offset, size, name: PARTITIONS[i] ?? `partition ${i}` });
        }
    }
    if (!partitions.length || partitions[0].index !== 0) {
        fail("A 3DS ROM", "This cartridge image has no game partition. Dump it again.");
    }
    const end = Math.max(...partitions.map((p) => p.offset + p.size));
    if (end > source.size) {
        fail("Complete", "The file ends before its last partition does: it was cut short. Copy " +
            "or dump it again.");
    }
    pass("Cartridge image (NCSD)",
        `${partitions.length} partition${partitions.length > 1 ? "s" : ""}: ` +
        partitions.map((p) => p.name).join(", "));

    const ncchs = [];
    for (const p of partitions) {
        const ncch = await readNcch(source, p.offset, p.size);
        if (!ncch) {
            fail("A 3DS ROM", `The ${p.name} partition has no NCCH header. Dump the ROM again.`);
        }
        ncchs.push(ncch);
    }
    for (let i = 0; i < ncchs.length; i++) {
        if (!ncchs[i].noCrypto) {
            fail("Decrypted", `The ${partitions[i].name} partition is encrypted. ${DECRYPT_HINT}`);
        }
    }
    pass("Decrypted", "Every partition says so in its header");
    for (let i = 0; i < ncchs.length; i++) {
        await checkNcch(source, ncchs[i], `${partitions[i].name} partition`, fail);
    }
    pass("Contents match their hashes", "Extended header, ExeFS and RomFS headers");

    const game = ncchs[0];
    const smdh = await ncchSmdh(source, game);
    return {
        format: "NCSD",
        kind: "Cartridge image",
        outputName: `${baseName(source.name)}.zcci`,
        underlyingMagic: new Uint8Array([0x4e, 0x43, 0x53, 0x44]),
        frameSize: DEFAULT_FRAME_SIZE,
        metadata: [["titleinfo", titleInfo(game.programId,
            game.contentSize * game.contentUnitSize, game.version, 0)]],
        info: { ...ncchInfo(game), smdh },
        checks,
    };
}

async function inspectCxi(source, checks, pass, fail) {
    const ncch = await readNcch(source, 0, source.size);
    if (ncch.end > source.size) {
        fail("Complete", "The file ends before its contents do: it was cut short. Copy or dump " +
            "it again.");
    }
    pass("Executable content (NCCH)", ncch.exheaderSize ? "With an extended header" : "Data only");
    await checkNcch(source, ncch, "ROM", fail);
    pass("Decrypted", "Its header says so");
    pass("Contents match their hashes", "Extended header, ExeFS and RomFS headers");
    return {
        format: "NCCH",
        kind: "Executable (CXI)",
        outputName: `${baseName(source.name)}.zcxi`,
        underlyingMagic: new Uint8Array([0x4e, 0x43, 0x43, 0x48]),
        frameSize: DEFAULT_FRAME_SIZE,
        metadata: [["titleinfo", titleInfo(ncch.programId,
            ncch.contentSize * ncch.contentUnitSize, ncch.version, 0)]],
        info: { ...ncchInfo(ncch), smdh: await ncchSmdh(source, ncch) },
        checks,
    };
}

async function inspect3dsx(source, first, checks, pass, fail) {
    const v = view(first);
    const headerSize = v.getUint16(4, true);
    if (headerSize < 0x20) {
        fail("A 3DS ROM", "This 3DSX file's header is broken. Get the homebrew again.");
    }
    pass("Homebrew (3DSX)", "Homebrew is never encrypted");
    let smdh = null;
    if (headerSize >= 0x2c) {
        const offset = v.getUint32(0x20, true);
        const size = v.getUint32(0x24, true);
        if (size >= SMDH_SIZE && offset + SMDH_SIZE <= source.size) {
            smdh = parseSmdh(await source.read(offset, SMDH_SIZE));
        }
    }
    return {
        format: "3DSX",
        kind: "Homebrew",
        outputName: `${baseName(source.name)}.z3dsx`,
        underlyingMagic: new Uint8Array([0x33, 0x44, 0x53, 0x58]),
        frameSize: DEFAULT_FRAME_SIZE,
        metadata: [],
        info: { smdh },
        checks,
    };
}

const SIGNATURE_SIZES = { 0x10000: 0x200, 0x10001: 0x100, 0x10002: 0x3c, 0x10003: 0x200,
    0x10004: 0x100, 0x10005: 0x3c };

async function inspectCia(source, checks, pass, fail) {
    const h = await readExact(source, 0, 0x2020);
    if (!h) {
        fail("A 3DS ROM", "This CIA file is cut short. Copy or dump it again.");
    }
    const v = view(h);
    const align = (x) => Math.ceil(x / 0x40) * 0x40;
    const certSize = v.getUint32(8, true);
    const ticketSize = v.getUint32(12, true);
    const tmdSize = v.getUint32(16, true);
    const metaSize = v.getUint32(20, true);
    const contentSize = Number(v.getBigUint64(24, true));
    const present = (index) => (h[0x20 + (index >> 3)] & (0x80 >> (index & 7))) !== 0;
    const ticketOffset = align(align(0x2020) + certSize);
    const tmdOffset = align(ticketOffset + ticketSize);
    const contentOffset = align(tmdOffset + tmdSize);
    const metaOffset = align(contentOffset + contentSize);
    if (contentOffset + contentSize > source.size) {
        fail("Complete", "The CIA ends before its contents do: it was cut short. Copy or dump " +
            "it again.");
    }

    const tmd = await readExact(source, tmdOffset, tmdSize);
    const signatureSize = tmd ? SIGNATURE_SIZES[view(tmd).getUint32(0, false)] : undefined;
    if (!signatureSize) {
        fail("A 3DS ROM", "This CIA's title metadata is broken. Dump it again.");
    }
    const body = align(signatureSize + 4);
    const t = view(tmd);
    if (tmd.length < body + 0x9c4) {
        fail("A 3DS ROM", "This CIA's title metadata is broken. Dump it again.");
    }
    const titleId = t.getBigUint64(body + 0x4c, false);
    const titleType = t.getUint32(body + 0x54, false);
    const titleVersion = t.getUint16(body + 0x9c, false);
    const count = t.getUint16(body + 0x9e, false);
    if (tmd.length < body + 0x9c4 + count * 0x30) {
        fail("A 3DS ROM", "This CIA's title metadata is broken. Dump it again.");
    }
    const chunks = [];
    for (let i = 0; i < count; i++) {
        const o = body + 0x9c4 + i * 0x30;
        chunks.push({
            index: t.getUint16(o + 4, false),
            type: t.getUint16(o + 6, false),
            size: Number(t.getBigUint64(o + 8, false)),
        });
    }
    pass("Installable title (CIA)", `${chunks.filter((c) => present(c.index)).length} of ` +
        `${count} contents included`);

    if (chunks.some((c) => (c.type & 1) && present(c.index))) {
        fail("Decrypted", `This CIA's contents are encrypted. ${DECRYPT_HINT}`);
    }
    // The included contents follow each other in the TMD's order (CIAContainer::GetContentOffset).
    const ncchs = [];
    let offset = contentOffset;
    for (const [i, chunk] of chunks.entries()) {
        if (!present(chunk.index)) {
            continue;
        }
        const ncch = await readNcch(source, offset, chunk.size);
        if (!ncch) {
            fail("A 3DS ROM", `Content ${i} of this CIA isn't an NCCH. Dump it again.`);
        }
        if (!ncch.noCrypto) {
            fail("Decrypted", `Content ${i} of this CIA is encrypted. ${DECRYPT_HINT}`);
        }
        ncchs.push({ i, ncch });
        offset += chunk.size;
    }
    pass("Decrypted", "No content is encrypted");
    for (const { i, ncch } of ncchs) {
        await checkNcch(source, ncch, `content ${i}`, fail);
    }
    pass("Contents match their hashes", "Extended header, ExeFS and RomFS headers");

    const metadata = [["titleinfo", titleInfo(titleId,
        chunks.filter((c) => present(c.index)).reduce((n, c) => n + c.size, 0),
        titleVersion, titleType)]];
    let smdh = null;
    if (metaSize >= 0x400 + SMDH_SIZE) {
        smdh = parseSmdh(await readExact(source, metaOffset + 0x400, SMDH_SIZE));
        if (smdh) {
            metadata.push(["smdh", smdh.bytes]);
        }
    }
    const game = ncchs[0]?.ncch;
    return {
        format: "CIA",
        kind: "Installable title (CIA)",
        outputName: `${baseName(source.name)}.zcia`,
        underlyingMagic: new Uint8Array([0x43, 0x49, 0x41, 0x00]),
        frameSize: DEFAULT_CIA_FRAME_SIZE,
        metadata,
        info: {
            ...(game ? ncchInfo(game) : {}),
            titleId: hex64(titleId),
            version: `${titleVersion >> 10}.${(titleVersion >> 4) & 0x3f}.${titleVersion & 0xf}`,
            smdh: smdh ?? (game ? await ncchSmdh(source, game) : null),
        },
        checks,
    };
}
