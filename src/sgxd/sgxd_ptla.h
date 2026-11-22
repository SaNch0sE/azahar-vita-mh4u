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
#include "sgxd_mkq.h"

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
// then `tail` pixels at the start of the next row, from src to dst, at (src_x, 0) and (dst_x, 0)
// of their surfaces. Both surfaces must be one the PTLA reads through its directory. dst_stride
// 0 is stride; another takes the destination surface word's own.
typedef struct SgxdPtlaCopy {
    uint32_t src, dst;
    uint32_t format;
    uint32_t stride;
    uint32_t width, rows, tail;
    uint32_t dst_stride;
    uint32_t src_x, dst_x;
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

// The async path: events, the app's zeroed page, registered (sgxdEvents) and sgxd's microkernel
// (sgxd_mkq.h) started at GPU address mk_va (SGXD_MKQ_SPACE), which runs the async lists from
// then on: the recipes' kicks leave core 0's event program alone (sgxd_recipe_events_owned) until
// the page goes. A fill or copy is then a list in the async queue (sgxd_chain_submit) and returns
// at once: it runs once the lists before it have, and the microkernel counts its completion.
// sgxd_ptla_fence() is the count the last op completes at; the list's own end is a chain fence
// like a render's. Synchronous submissions (the maps, the resets) wait for both first through
// sgxd_gpu_idle(). Returns 0, or -1 with failed filled (the ops stay synchronous). NULL waits for
// the last op, stops the microkernel and gives the interrupt back.
int sgxd_ptla_events(SgxdEvents *events, uint32_t mk_va, char *failed, unsigned failed_size);
uint32_t sgxd_ptla_fence(void);
// Whether the op that returned `fence` has completed (always, without events). Any thread.
int sgxd_ptla_reached(uint32_t fence);
// Spins until every PTLA op submitted has completed. Returns 0, or -1 after a second.
int sgxd_ptla_idle(void);
const volatile SgxdEvents *sgxd_ptla_page(void);

// Async lists, run in order by the microkernel (sgxd_mkq_submit): each ends in one
// SGXD_SIGNAL_DONE (and one SGXD_SIGNAL_TA before it), at most SGXD_MAX_CHAIN_OPS ops. The fence
// is the count of lists sent; chain_ta_reached is for the same fence, the list's vertices read.
// A full job ring spins until the GPU has made room (a second at most). AZAHAR_ERR_STATE without
// sgxd_ptla_events.
int sgxd_chain_submit(const SgxdOp *ops, unsigned count);
// The same with the variable marks (sgxd_mk_compile: WRITEs whose values change from send to
// send) and the sender's kept code (sgxd_mkq_submit); either may be NULL.
int sgxd_chain_submit_vars(const SgxdOp *ops, unsigned count, const uint8_t *variable,
                           SgxdMkqCode *code);
// Remaps the GPU makes in its queue's order (azahar's page versions): behind every list and PTLA
// op sent before it, each PTE word pte[k] stored at pte_va[k] (sgxdPte), then the MMU invalidate
// in every bank, as the kernel's sgxdMap runs it. 1 to SGXD_REMAP_MAX at a time. The caller has
// had the pending 3D finished, so no unit runs while the MMU's caches go. A list like any other
// (its fence).
#define SGXD_REMAP_MAX 31u
int sgxd_remap_submit(const uint32_t* pte_va, const uint32_t* pte, unsigned count, char* failed,
                      unsigned failed_size);
// The lists completed (the SGXD_SIGNAL_DONE count), the PTLA ops completed and the queue's
// fault (0, or 1 + the op it stopped at): the microkernel's state words.
uint32_t sgxd_chain_done(void);
uint32_t sgxd_ptla_done(void);
uint32_t sgxd_chain_faulted(void);
// The waker (sgxdWaker): sets bits of the app's event flag once the GPU reaches the counts
// sgxd_wake_arm gave (a render list fence, a PTLA fence): the microkernel's SW_EVENT. Arm before
// a wait on the flag: 0 when the counts are reached already (no wait), 1 when armed. Disarm
// after it, woken or not; a flag set after the disarm only ends the next wait early. wakes
// counts the flags set.
int sgxd_ptla_waker(int32_t flag, uint32_t bits);
int sgxd_wake_arm(uint32_t chains, uint32_t twod);
void sgxd_wake_disarm(void);
uint32_t sgxd_wake_count(void);
#ifdef VITA_DIAGNOSTICS
// The app's sgxd syscalls counted (perflog; diagnostics builds only): synchronous lists
// (sgxdSubmit) and sgxdKick.
extern uint32_t sgxd_sync_submits, sgxd_kicks;
#endif
uint32_t sgxd_chain_fence(void);
int sgxd_chain_reached(uint32_t fence);
int sgxd_chain_ta_reached(uint32_t fence);
// Spins until the list that returned fence (the count after it) has completed. Returns 0, or -1
// on a fault or after sgxd_chain_stall_us in which no list completed. The spins time out on the
// calling thread's cycle counter (azaharCycles): azaharCycleCounter must have run for the process.
int sgxd_chain_wait(uint32_t fence);
// The same, asleep: arms the waker (sgxd_wake_arm) for the fence and waits on the app's event
// flag `flag` (bits, the ones sgxdWaker was given) until the microkernel's SW_EVENT, a tenth of
// a second at a time. Returns 0, -1 on a fault or a stall as sgxd_chain_wait, or the event
// flag's error. One waiter on the flag at a time (it is SCE_EVENT_WAITSINGLE).
int sgxd_chain_sleep(uint32_t fence, int32_t flag, uint32_t bits);
// The same until the process time `deadline` (us, sceKernelGetProcessTimeWide): 1 when that
// comes first.
int sgxd_chain_sleep_until(uint32_t fence, int32_t flag, uint32_t bits, uint64_t deadline);
extern const uint32_t sgxd_chain_stall_us;
// Spins until the last list queued completed, as sgxd_chain_wait.
int sgxd_chain_idle(void);
// The render drain, then sgxd_ptla_idle and sgxd_chain_idle: nothing of ours runs on the GPU.
int sgxd_gpu_idle(void);
// Who runs overlapped lists registers how to finish their last 3D (0, or -1 on a fault):
// sgxd_gpu_idle calls it first. PTLA fills and copies do not: their caller queues that finish
// first when the op touches the 3D's memory.
void sgxd_set_render_drain(int (*drain)(void));

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
