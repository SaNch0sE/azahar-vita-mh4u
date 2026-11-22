#!/bin/sh
# SPDX-FileCopyrightText: 2026 Willi Ye
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds site/zstd.wasm from azahar's zstd (externals/zstd) with clang's wasm32 target.
# Needs clang and wasm-ld (lld); no emscripten or wasi sysroot.
set -e
here=$(cd "$(dirname "$0")" && pwd)
zstd=$here/../../externals/zstd/lib
cc=${CC:-clang}

$cc --target=wasm32 -O3 -flto -nostdlib -ffreestanding -fno-builtin-malloc \
    -msimd128 -mbulk-memory -mnontrapping-fptoint -msign-ext \
    -DNDEBUG -DZSTD_DISABLE_ASM -DZSTD_NO_INTRINSICS \
    -DXXH_NO_STDLIB -DZSTD_LEGACY_SUPPORT=0 \
    -isystem "$here/include" -I"$zstd" -I"$zstd/common" \
    "$here/libc.c" "$here/z3ds_zstd.c" \
    "$zstd"/common/entropy_common.c "$zstd"/common/error_private.c \
    "$zstd"/common/fse_decompress.c "$zstd"/common/xxhash.c "$zstd"/common/zstd_common.c \
    "$zstd"/compress/fse_compress.c "$zstd"/compress/hist.c "$zstd"/compress/huf_compress.c \
    "$zstd"/compress/zstd_compress.c "$zstd"/compress/zstd_compress_literals.c \
    "$zstd"/compress/zstd_compress_sequences.c "$zstd"/compress/zstd_compress_superblock.c \
    "$zstd"/compress/zstd_double_fast.c "$zstd"/compress/zstd_fast.c "$zstd"/compress/zstd_lazy.c \
    "$zstd"/compress/zstd_ldm.c "$zstd"/compress/zstd_opt.c "$zstd"/compress/zstd_preSplit.c \
    -Wl,--no-entry -Wl,--strip-all -Wl,--initial-memory=4194304 -Wl,--max-memory=1073741824 \
    -Wl,--export=malloc -Wl,--export=free -Wl,--export=z3ds_init -Wl,--export=z3ds_bound \
    -Wl,--export=z3ds_frame -Wl,--export=z3ds_is_error -Wl,--export=z3ds_error_name \
    -Wl,--export=z3ds_version \
    -o "$here/site/zstd.wasm"
