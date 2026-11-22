#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Willi Ye
# SPDX-License-Identifier: GPL-2.0-or-later

"""azahar's host0:azahar-sgxd-gs-stage.bin: the PICA state of the geometry draw whose stream-out
render stopped (RasterizerSgxd::PrintStreamOut, diagnostics builds). Prints the geometry
registers, both programs' uniforms and a disassembly of each program from its entry.

Usage: sgxd_gs_stage.py DUMP                     azahar-sgxd-gs-stage.bin
       sgxd_gs_stage.py --programs FILE [HASH]   azahar-sgxd-programs.bin (drawring's programs):
                                                 the hashes, or HASH's disassembly
"""
import struct
import sys

CODE = SWIZZLE = 4096

NAMES = {
    0x00: "add", 0x01: "dp3", 0x02: "dp4", 0x03: "dph", 0x04: "dst", 0x05: "ex2", 0x06: "lg2",
    0x07: "litp", 0x08: "mul", 0x09: "sge", 0x0A: "slt", 0x0B: "flr", 0x0C: "max", 0x0D: "min",
    0x0E: "rcp", 0x0F: "rsq", 0x12: "mova", 0x13: "mov", 0x18: "dphi", 0x19: "dsti",
    0x1A: "sgei", 0x1B: "slti", 0x20: "break", 0x21: "nop", 0x22: "end", 0x23: "breakc",
    0x24: "call", 0x25: "callc", 0x26: "callu", 0x27: "ifu", 0x28: "ifc", 0x29: "loop",
    0x2A: "emit", 0x2B: "setemit", 0x2C: "jmpc", 0x2D: "jmpu",
}
CONDS = ["x || y", "x && y", "x", "y"]
CMPS = ["==", "!=", "<", "<=", ">", ">="]


def src(n):
    if n < 0x10:
        return f"v{n}"
    if n < 0x20:
        return f"r{n - 0x10}"
    return f"c{n - 0x20}"


def dst(n):
    return f"o{n}" if n < 0x10 else f"r{n - 0x10}"


def sel(desc, shift):
    return "".join("xyzw"[(desc >> (shift + 6 - 2 * k)) & 3] for k in range(4))


def operands(desc):
    mask = "".join(c for k, c in enumerate("xyzw") if desc & (8 >> k))
    n1 = "-" if desc & 0x10 else ""
    n2 = "-" if desc & 0x2000 else ""
    n3 = "-" if desc & 0x400000 else ""
    return mask, (n1, sel(desc, 5)), (n2, sel(desc, 14)), (n3, sel(desc, 23))


def disasm(w, swz):
    op = w >> 26
    flow_cond = f"{'!' if not (w >> 25) & 1 else ''}cc.x {CONDS[(w >> 22) & 3]} " \
                f"{'!' if not (w >> 24) & 1 else ''}cc.y"
    dest, num = (w >> 10) & 0xFFF, w & 0xFF
    if op >= 0x30:
        # mad (0x38..) / madi (0x30..): dest 24-28, src1 17-21, src2 10-16 or 10-14, src3 5-9 or 15-21
        desc = swz[w & 0x1F]
        mask, s1, s2, s3 = operands(desc)
        if op >= 0x38:
            a, b, c = (w >> 17) & 0x1F, (w >> 10) & 0x7F, (w >> 5) & 0x1F
        else:
            a, b, c = (w >> 17) & 0x1F, (w >> 10) & 0x1F, (w >> 5) & 0x7F
        name = "mad" if op >= 0x38 else "madi"
        return (f"{name} {dst((w >> 24) & 0x1F)}.{mask}, {s1[0]}{src(a)}.{s1[1]}, "
                f"{s2[0]}{src(b)}.{s2[1]}, {s3[0]}{src(c)}.{s3[1]}")
    if op in (0x2E, 0x2F):
        desc = swz[w & 0x7F]
        _, s1, s2, _ = operands(desc)
        a, b = (w >> 12) & 0x7F, (w >> 7) & 0x1F
        cx, cy = (w >> 24) & 7, (w >> 21) & 7
        cn = lambda c: CMPS[c] if c < 6 else f"?{c}"
        idx = (w >> 19) & 3
        return (f"cmp {s1[0]}{src(a)}{['', '[a0.x]', '[a0.y]', '[aL]'][idx]}.{s1[1]}, "
                f"{cn(cx)}, {cn(cy)}, {s2[0]}{src(b)}.{s2[1]}")
    name = NAMES.get(op, f"op{op:02x}")
    if op in (0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2C, 0x2D, 0x23):
        if op in (0x25, 0x28, 0x2C, 0x23):
            cond = flow_cond
        elif op in (0x26, 0x27, 0x2D):
            cond = f"b{(w >> 22) & 0xF}"
            if op == 0x2D and num & 1:
                cond = "!" + cond
        elif op == 0x29:
            cond = f"i{(w >> 22) & 3}"
        else:
            cond = ""
        if op == 0x23:
            return f"breakc {cond}"
        return f"{name} {cond} -> {dest:#05x}" + (f" (+{num})" if op not in (0x2C, 0x2D) else "")
    if op == 0x2B:
        return (f"setemit vtx {(w >> 24) & 3}, prim {(w >> 23) & 1}, "
                f"winding {(w >> 22) & 1}")
    if op in (0x20, 0x21, 0x22, 0x2A):
        return name
    desc = swz[w & 0x7F]
    mask, s1, s2, _ = operands(desc)
    idx = (w >> 19) & 3
    rel = ["", "[a0.x]", "[a0.y]", "[aL]"][idx]
    if op in (0x18, 0x19, 0x1A, 0x1B):  # inverted: src1 5 bits at 14, src2 7 bits at 7
        a, b = (w >> 14) & 0x1F, (w >> 7) & 0x7F
        return (f"{name} {dst((w >> 21) & 0x1F)}.{mask}, {s1[0]}{src(a)}.{s1[1]}, "
                f"{s2[0]}{src(b)}{rel}.{s2[1]}")
    a, b = (w >> 12) & 0x7F, (w >> 7) & 0x1F
    one = op in (0x05, 0x06, 0x07, 0x0B, 0x0E, 0x0F, 0x12, 0x13)
    d = "a0" if op == 0x12 else dst((w >> 21) & 0x1F)
    text = f"{name} {d}.{mask}, {s1[0]}{src(a)}{rel}.{s1[1]}"
    return text if one else text + f", {s2[0]}{src(b)}.{s2[1]}"


def listing(code, swz, entry):
    last = max((k for k, w in enumerate(code) if w), default=-1)
    print(f"  code: {last + 1} words")
    for k in range(last + 1):
        print(f"  {k:#05x}{'>' if k == entry else ' '} {code[k]:08x}  {disasm(code[k], swz)}")


def programs(path, want):
    data = open(path, "rb").read()
    magic, n = struct.unpack_from("<2I", data, 0)
    assert magic == 0x41434950, "not a PICA programs dump"
    off = 8
    for _ in range(n):
        (h,) = struct.unpack_from("<Q", data, off)
        entry = struct.unpack_from("<I", data, off + 8)[0]
        code = struct.unpack_from(f"<{CODE}I", data, off + 12)
        swz = struct.unpack_from(f"<{SWIZZLE}I", data, off + 12 + CODE * 4)
        off += 12 + (CODE + SWIZZLE) * 4
        if want is None:
            loops = sum(1 for w in code if w >> 26 == 0x29)
            back = sum(1 for k, w in enumerate(code)
                       if w >> 26 in (0x2C, 0x2D) and ((w >> 10) & 0xFFF) <= k)
            print(f"{h:016x} entry {entry:#x}: {loops} loops, {back} backward jumps")
        elif f"{h:016x}".startswith(want):
            print(f"{h:016x} entry {entry:#x}")
            listing(code, swz, entry)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    if sys.argv[1] == "--programs":
        programs(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)
        return
    data = open(sys.argv[1], "rb").read()
    magic, version, nregs = struct.unpack_from("<3I", data, 0)
    assert magic == 0x4F535347 and version == 1, "not a GSSO dump"
    off = 12
    regs = struct.unpack_from(f"<{nregs}I", data, off)
    off += nregs * 4
    print("geometry registers (0x280..0x2ba):")
    for r in range(0x280, 0x2BB):
        if regs[r]:
            print(f"  {r:#05x} {regs[r]:08x}")
    for r in (0x229, 0x252, 0x253, 0x254, 0x25e, 0x289):
        print(f"  reg {r:#05x} {regs[r]:08x}")
    for unit in ("VS", "GS"):
        (entry,) = struct.unpack_from("<I", data, off)
        off += 4
        code = struct.unpack_from(f"<{CODE}I", data, off)
        off += CODE * 4
        swz = struct.unpack_from(f"<{SWIZZLE}I", data, off)
        off += SWIZZLE * 4
        f = struct.unpack_from("<384f", data, off)
        off += 384 * 4
        b = struct.unpack_from("<16I", data, off)
        off += 64
        i = struct.unpack_from("<4I", data, off)
        off += 16
        print(f"\n{unit}: entry {entry:#x}")
        print("  ints: " + " / ".join(" ".join(str((v >> s) & 0xFF) for s in (0, 8, 16, 24))
                                     for v in i))
        print("  bools: " + "".join(str(v) for v in b))
        for k in range(96):
            vals = f[4 * k:4 * k + 4]
            if any(vals):
                print(f"  c{k}: " + " ".join(f"{v:g}" for v in vals))
        listing(code, swz, entry)


if __name__ == "__main__":
    main()
