// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// sgxd_ptla: the SGX's 2D unit (the PTLA), driven from the host with no microkernel
// (docs/sgxd/ptla.md). The stream goes word by word into MASTER_PTLA_SLAVE_CMD_0; FLUSH ends it
// and raises MASTER_EVENT_STATUS TWOD_COMPLETE. Block layouts carry sgxptladefs.h's names.
// The PTLA reads its page directory through BIF_BANK0's PTLA index, which sgxd's take leaves at
// 0: the directory sgxdMap fills. Vita only, like sgxd_session.

#ifndef SGXD_PTLA_H
#define SGXD_PTLA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// PTLA surface formats (the ones measured).
#define SGXD_PTLA_U8 0u
#define SGXD_PTLA_4444 1u
#define SGXD_PTLA_1555 2u
#define SGXD_PTLA_565 3u
#define SGXD_PTLA_U88 4u
#define SGXD_PTLA_888 5u
#define SGXD_PTLA_8888 6u
#define SGXD_PTLA_RAW32 17u

// The largest width or height a blit takes: its coordinates are 13 bits.
#define SGXD_PTLA_MAX_EXTENT 8191u

// A fill of a strided surface's top rows: `rows` whole rows of `width` pixels, then `tail`
// pixels at the start of the next row, all `colour`. Pixels are `format`; stride is in bytes,
// below 64 KiB. The fill colour is written as the format stores an 8888 word: in 8888, the
// word's four bytes, low first.
typedef struct SgxdPtlaFill {
    uint32_t va;
    uint32_t format;
    uint32_t stride;
    uint32_t width, rows, tail;
    uint32_t colour;
} SgxdPtlaFill;

// Runs the fill and waits for it: the SLC flushed and invalidated (the GPU must be idle), each
// blit's stream ending in FLUSH, TWOD_COMPLETE polled and cleared, the SLC flushed and
// invalidated again so the bytes are in memory. Returns 0, or -1 with the op that stopped the
// list in failed.
int sgxd_ptla_fill(const SgxdPtlaFill *fill, uint32_t *elapsed_us, char *failed,
                   unsigned failed_size);

#ifdef __cplusplus
}
#endif

#endif
