#!/bin/bash
# SPDX-FileCopyrightText: 2026 Willi Ye
# SPDX-License-Identifier: GPL-2.0-or-later
#
# build-profile-rt.sh LLVM_SRC OUT.a: LLVM's profile runtime (compiler-rt/lib/profile) for the Vita,
# for clang -fprofile-instr-generate builds of azahar (AZAHAR_PGO=GENERATE). LLVM_SRC is an
# llvm-project tree of the same release as the clang that compiles azahar (21.1.8 for clang-21
# 21.1.8): the raw profile's format is that release's. Its Linux platform file is replaced by
# InstrProfilingPlatformVita.c, the POSIX pieces newlib lacks by vita_profile_shims.c.
set -e
src=$1
out=$2
here=$(cd "$(dirname "$0")" && pwd)
[ -f "$src/compiler-rt/lib/profile/InstrProfiling.c" ] || { echo "no compiler-rt in $src"; exit 1; }
sys=${VITASDK:-/usr/local/vitasdk}/arm-vita-eabi
cc=${CC:-clang-21}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
flags=(--target=armv7a-none-eabihf --sysroot="$sys" -mcpu=cortex-a9 -mfpu=neon -mthumb -fno-pic
       -O2 -D__vita__=1 -DCOMPILER_RT_HAS_ATOMICS=1 -I"$src/compiler-rt/include"
       -I"$src/compiler-rt/lib/profile")
for f in InstrProfiling InstrProfilingBuffer InstrProfilingFile InstrProfilingInternal \
         InstrProfilingMerge InstrProfilingMergeFile InstrProfilingNameVar InstrProfilingUtil \
         InstrProfilingValue InstrProfilingVersionVar InstrProfilingWriter; do
    "$cc" "${flags[@]}" -c "$src/compiler-rt/lib/profile/$f.c" -o "$tmp/$f.o"
done
"$cc" "${flags[@]}" -c "$here/InstrProfilingPlatformVita.c" -o "$tmp/platform.o"
"$cc" "${flags[@]}" -c "$here/vita_profile_shims.c" -o "$tmp/shims.o"
rm -f "$out"
llvm-ar-21 rcs "$out" "$tmp"/*.o
echo "$out: $(ls "$tmp"/*.o | wc -l) objects"
