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

#include "sgxd.h"

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

// A copy between two strided surfaces of the same format: `rows` whole rows of `width` pixels,
typedef struct SgxdPtlaCopy {
    uint32_t src, dst;
    uint32_t format;
    uint32_t stride;
    uint32_t width, rows, tail;
    uint32_t dst_stride;
} SgxdPtlaCopy;

// Without events: runs the fill and waits for it: the SLC flushed and invalidated, each
// blit's stream ending in FLUSH, TWOD_COMPLETE polled and cleared, the SLC flushed and
// invalidated again so the bytes are in memory. Returns 0, or -1. The time and, on a failure,
// the op that stopped the list are the diagnostics build's (elapsed_us 0, failed untouched
// otherwise).
int sgxd_ptla_fill(const SgxdPtlaFill *fill, uint32_t *elapsed_us, char *failed,
                   unsigned failed_size);

// The copy, as sgxd_ptla_fill (queued with events). An overlap is only safe within one
// blit: the PTLA reads a blit's whole source before it writes, but the whole rows and the tail
// are two blits.
int sgxd_ptla_copy(const SgxdPtlaCopy *copy, uint32_t *elapsed_us, char *failed,
                   unsigned failed_size);

// Asynchronous PTLA ops (sgxdEvents). Once events, the app's zeroed page, is registered, a fill
// or copy is a list in the async queue (sgxd_chain_submit) and returns at once: it runs once
// the lists before it have, the kernel's ISR takes each TWOD_COMPLETE, writes the SLC back and
// empties it, then counts it in events->twod. sgxd_ptla_fence() is the count the last op
// completes at; the list's own end is a chain fence like a render's. Synchronous submissions
// (the maps, the resets) wait for both first through sgxd_gpu_idle(). Returns 0, or
// sgxdEvents's error (the ops stay synchronous). NULL waits for the last op and gives the
// interrupt back.
int sgxd_ptla_events(SgxdEvents *events);
uint32_t sgxd_ptla_fence(void);
// Whether the op that returned `fence` has completed (always, without events). Any thread.
int sgxd_ptla_reached(uint32_t fence);
// Spins until every PTLA op submitted has completed. Returns 0, or -1 after a second.
int sgxd_ptla_idle(void);
const volatile SgxdEvents *sgxd_ptla_page(void);

// Async lists (sgxdSubmitAsync), queued in the kernel and run in order from IRQ 52: each ends
// in one SGXD_SIGNAL_DONE (and one SGXD_SIGNAL_TA before it). The fence is the count of lists
// sent; chain_ta_reached is for the same fence, the list's vertices read. A full queue spins
// until the GPU has made room (a second at most).
int sgxd_chain_submit(const SgxdOp *ops, unsigned count);
// The queued op at the kernel's queue position pos (31 bits, as sgxdSubmitAsync's stop returns
// it less one), and how many ops from the queue's end it is; NULL once the queue has moved past
// it by a whole ring.
const SgxdOp *sgxd_chain_op(uint32_t pos, uint32_t *behind);
uint32_t sgxd_chain_fence(void);
int sgxd_chain_reached(uint32_t fence);
int sgxd_chain_ta_reached(uint32_t fence);
// Spins until the last list completed. Returns 0, or -1 on its fault or after a second.
int sgxd_chain_idle(void);
// sgxd_ptla_idle and sgxd_chain_idle: nothing of ours runs on the GPU.
int sgxd_gpu_idle(void);

#ifdef VITA_DIAGNOSTICS
// The time the app's threads spun waiting for the GPU: the idle waits above and a synchronous
// render's TA and 3D waits, in the diagnostics build only. Take returns it and starts again.
void sgxd_wait_us_add(uint32_t us);
uint32_t sgxd_wait_us_take(void);
// The same, since the start, never taken.
uint64_t sgxd_wait_us_total(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
