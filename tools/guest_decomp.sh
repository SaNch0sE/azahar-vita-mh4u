#!/bin/bash
# SPDX-FileCopyrightText: 2026 Willi Ye
# SPDX-License-Identifier: GPL-2.0-or-later
#
# guest_decomp.sh DUMP BASE OUT.c DEPTH ADDR...: Ghidra's decompile of a 3DS title's functions
# from a raw dump of its code (the diagnostics command `vdump VADDR SIZE` of a running title;
# BASE the dump's first address, hexadecimal). ADDR: "=HEX" a function's first instruction, or
# HEX inside one (its start: the nearest push {..., lr} before); callees DEPTH calls down too.
set -e
here=$(cd "$(dirname "$0")" && pwd)
GHIDRA=${GHIDRA:?the Ghidra installation}
dump=$1; base=$2; out=$3; depth=$4; shift 4
proj=$(mktemp -d)
"$GHIDRA/support/analyzeHeadless" "$proj" guest -import "$dump" -overwrite -noanalysis \
    -loader BinaryLoader -loader-baseAddr "0x$base" -processor "ARM:LE:32:v6" -cspec default \
    -scriptPath "$here/guest_decomp" -postScript DecompAt.java "$out" "$depth" "$@" \
    -deleteProject 2>&1 | grep -E "ERROR|wrote|no function" | head -20
rm -rf "$proj"
