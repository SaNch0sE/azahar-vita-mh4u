// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// zstd for the Z3DS web compressor.
//
// Each call compresses one frame of the seekable format exactly as azahar's Z3DSWriteIOFile does
// through ZSTD_seekable_compressStream: a stream at the given level with the frame size as its
// size hint, fed with ZSTD_compressStream, closed with ZSTD_endStream, then reset for the next
// frame. A frame never depends on the one before it, so the page compresses frames on several
// workers and the bytes still match azahar's own output. The seek table is written by the page.

#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"

static ZSTD_CStream* stream;
static unsigned frame_size;

// Returns 0, or a zstd error code.
size_t z3ds_init(int level, unsigned max_frame_size) {
    if (!stream) {
        stream = ZSTD_createCStream();
        if (!stream) {
            return (size_t)-ZSTD_error_memory_allocation;
        }
    }
    frame_size = max_frame_size;
    return ZSTD_initCStream(stream, level);
}

size_t z3ds_bound(size_t length) {
    // The frame header, the data's own bound, and the empty last block ZSTD_endStream adds when
    // the data ended on a block edge.
    return ZSTD_compressBound(length) + 64;
}

// Compresses length bytes (at most the frame size) at src into one frame at dst. Returns the
// frame's size, or a zstd error code (z3ds_is_error).
size_t z3ds_frame(const void* src, size_t length, void* dst, size_t capacity) {
    if (length > frame_size) {
        return (size_t)-ZSTD_error_srcSize_wrong;
    }
    ZSTD_CCtx_setParameter(stream, ZSTD_c_srcSizeHint, (int)frame_size);
    ZSTD_outBuffer out = {dst, capacity, 0};
    ZSTD_inBuffer in = {src, length, 0};
    while (in.pos < in.size) {
        const size_t ret = ZSTD_compressStream(stream, &out, &in);
        if (ZSTD_isError(ret)) {
            return ret;
        }
        if (out.pos == out.size && in.pos < in.size) {
            return (size_t)-ZSTD_error_dstSize_tooSmall;
        }
    }
    const size_t left = ZSTD_endStream(stream, &out);
    if (ZSTD_isError(left)) {
        return left;
    }
    if (left != 0) {
        return (size_t)-ZSTD_error_dstSize_tooSmall;
    }
    ZSTD_CCtx_reset(stream, ZSTD_reset_session_only);
    return out.pos;
}

unsigned z3ds_is_error(size_t code) {
    return ZSTD_isError(code);
}

const char* z3ds_error_name(size_t code) {
    return ZSTD_getErrorName(code);
}

unsigned z3ds_version(void) {
    return ZSTD_versionNumber();
}
