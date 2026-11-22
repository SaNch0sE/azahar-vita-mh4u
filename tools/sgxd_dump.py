#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Willi Ye
# SPDX-License-Identifier: GPL-2.0-or-later

"""Decode an sgxdDump register dump (azahar vita_native/src/sgxd.h): SceGpuEs4's own register
lists, read by sgxd. Layout: [0] magic "SGXD", [1] version, [10] cores, [11] bank registers, [12]
per-core registers, [14 + 2i] offset and value of bank register i, [546 + 5i] offset and cores 0..3
of per-core register i. A fault dump (sgxdFaultDump, version 3) has [2] "FULT", [3] the poll's op
index, [4] register, [5] wanted value, [6] mask, [7] last read, [8] us waited, [9] the system time.
With --hwdefs, registers are named from the SGX's hardware definition headers (sgx543defs.h,
sgxmpdefs.h) in that directory (EUR_CR_* offsets; the per-core bank is the core's own window);
without it they print by offset.

    sgxd_dump.py DUMP [--hwdefs DIR] [--all] [--grep REGEX]

Without --all only non-zero registers are printed."""
import argparse
import os
import re
import struct

MAGIC = 0x44584753
DEBUG_AT = 2541   # sgxd.h SGXD_DUMP_DEBUG_AT
DEBUG_IDS = 109
FAULT = 0x544c5546


def names(hwdefs, header):
    out = {}
    if not hwdefs:
        return out
    path = os.path.join(hwdefs, header)
    pattern = re.compile(r'#define\s+(EUR_CR_[A-Z0-9_]+)\s+0x([0-9A-Fa-f]+)U?\s*$')
    for line in open(path, errors='replace'):
        m = pattern.match(line)
        if m and not m.group(1).endswith(('_MASK', '_SHIFT', '_SIGNED')):
            out.setdefault(int(m.group(2), 16), m.group(1))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dump')
    ap.add_argument('--hwdefs',
                    help='the directory of sgx543defs.h and sgxmpdefs.h, for register names')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--grep')
    a = ap.parse_args()
    data = open(a.dump, 'rb').read()
    w = struct.unpack('<%dI' % (len(data) // 4), data)
    assert w[0] == MAGIC, 'not an sgxd dump (magic %08x)' % w[0]
    cores, nbank, ncore = w[10], w[11], w[12]
    core_names = names(a.hwdefs, 'sgx543defs.h')
    bank_names = names(a.hwdefs, 'sgxmpdefs.h')
    grep = re.compile(a.grep, re.I) if a.grep else None
    print(f'sgxd dump version {w[1]}, {cores} cores, {nbank} bank and {ncore} per-core registers')
    if w[2] == FAULT:
        print(f'fault: op {w[3]} polled {w[4]:#07x} for {w[5]:08x} under mask {w[6]:08x}, '
              f'last read {w[7]:08x}, after {w[8]} us (time {w[9]:#010x})')
    print('-- bank (master) registers')
    for i in range(nbank):
        off, val = w[14 + 2 * i], w[15 + 2 * i]
        name = bank_names.get(off, core_names.get(off, '?'))
        if (a.all or val) and (not grep or grep.search(name)):
            print(f'  {off:#06x} {name:40s} {val:08x}')
    print('-- per-core registers (cores 0..%d)' % (cores - 1))
    base = 14 + 2 * 266
    for i in range(ncore):
        off = w[base + 5 * i]
        vals = w[base + 5 * i + 1: base + 5 * i + 1 + cores]
        name = core_names.get(off, '?')
        if (a.all or any(vals)) and (not grep or grep.search(name)):
            print(f'  {off:#06x} {name:40s} ' + ' '.join(f'{v:08x}' for v in vals))
    # Version 15 on: the cores' debug-bus words (SGXDumpDebugState's third list, 109 IDs).
    debug = {}
    if len(w) >= DEBUG_AT + DEBUG_IDS * 5:
        print('-- debug bus (ID: cores 0..%d)' % (cores - 1))
        for i in range(DEBUG_IDS):
            ident = w[DEBUG_AT + 5 * i]
            vals = w[DEBUG_AT + 5 * i + 1: DEBUG_AT + 5 * i + 1 + cores]
            debug[ident] = vals
            if a.all or any(vals):
                print(f'  {ident:#04x} ' + ' '.join(f'{v:08x}' for v in vals))
    # Sony's lockup classifier (SceGpuEs4's): with no BIF fault in the master's or
    # any core's BIF_INT_STAT (0x4c04, 0x0c04; bits 16-18), a core whose debug word 0x25 has
    # bit 4 set is class 4, which SceGpuEs4 reports on a tool as "GPU may have been halted by
    # a known clipper hardware bug" (technote 274) and on a retail unit answers with a GPU
    # restart (classes 3 and 4).
    bif = [v for i in range(nbank) if w[14 + 2 * i] == 0x4c04 for v in [w[15 + 2 * i]]]
    for i in range(ncore):
        if w[base + 5 * i] == 0x0c04:
            bif += list(w[base + 5 * i + 1: base + 5 * i + 1 + cores])
    faulted = any((v & 0x7ffff) >> 16 for v in bif)
    if debug:
        clip = [c for c, v in enumerate(debug.get(0x25, [])) if v & 0x10]
        verdict = ('a BIF fault (classes 1-3 by the fault code)' if faulted else
                   f'class 4, the clipper lockup, on core {clip}' if clip else
                   'no class (no BIF fault, debug word 0x25 bit 4 clear on every core)')
        print(f'Sony classifier: {verdict}')
        # The TA's own state (sgx543_debug_defs.h): word 0x25 PERF_TA_2's clipper
        # stages (CLIP_BARY bit 4, CLIP_STALL_BARY bit 7) and word 0x27 PERF_TA_4's MTE units
        # (CLIP_EXEC bit 5, idle 0x12412). A core stopped in the clipper without bit 4 (CLIP_EXEC
        # with CTRL and TE stalled) escapes Sony's test; the clip_test homebrew's lockups set
        # bit 4.
        stuck = [c for c, v in enumerate(debug.get(0x27, [])) if v != 0x12412]
        clipper = [c for c in stuck
                   if debug.get(0x27)[c] & 0x20 or debug.get(0x25, [0] * cores)[c] & 0x90]
        if stuck:
            print(f'TA units busy on cores {stuck}; in the clipper on cores {clipper}')


if __name__ == '__main__':
    main()
