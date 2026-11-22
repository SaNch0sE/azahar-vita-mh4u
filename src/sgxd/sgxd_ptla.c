// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// sgxd_ptla: the PTLA driven from the host (sgxd_ptla.h).

#include "sgxd_ptla.h"

#include <stdio.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>

#include "sgxd.h"
#include "sgxd_mkq.h"
#include "sgxd_recipe.h"

#define W(r, v) {SGXD_OP_WRITE, (r), (v), 0, 0}
#define P(r, m, v, t) {SGXD_OP_POLL, (r), (v), (m), (t)}

#define PTLA_CMD 0x4180u             // MASTER_PTLA_SLAVE_CMD_0
#define EVENT_STATUS 0x412cu         // MASTER_EVENT_STATUS
#define EVENT_HOST_CLEAR 0x4134u     // MASTER_EVENT_HOST_CLEAR
#define TWOD_COMPLETE 0x08000000u
#define SLC_EVENT_STATUS 0x4d10u
#define SLC_EVENT_CLEAR 0x4d14u
#define SLC_CTRL_INVAL 0x4d28u
#define SLC_CTRL_FLUSH 0x4d2cu
// MASTER_CLKGATECTL2: the start sequence's 0x2a9 with the 2D clock (bits 8-9) on, 1, as Sony's
// PTLA kick sets it before the command words; its completion puts it back to auto, 2 (sgxd's
// microkernel runtime). Nothing else writes it.
#define CLKGATECTL2 0x4020u
#define CLKGATECTL2_2D_ON 0x1a9u

#define SRCOFF 0x10000000u
#define SRC_SURF 0x40000000u
#define DST_SURF 0x50000000u
#define BLIT 0x60000000u
#define FLUSH 0x80000000u
#define BLIT_FILL (1u << 4)
#define SURF_TWIDDLED (2u << 22)

// A blit's wait: the hardware completes at the first read after FLUSH.
#define TWOD_POLL_US 1000000u

// sgxd_chain_wait's limit: no list completed in this long is a stopped GPU.
const uint32_t sgxd_chain_stall_us = TWOD_POLL_US;

// The spins count the calling thread's own cycles (azaharCycles; no syscall a turn). A thread's
// cycles stop while it is not running, so a timeout can only come later than the wall clock's.
// The ARM clock in MHz, read when the events page is registered.
static uint32_t cycles_per_us;

// sgxd_ptla_events: the ISR's page, and the completions asked for so far (one a FLUSH).
static const volatile SgxdEvents *events;
static uint32_t twod_sent;
// sgxd_chain_submit: the async lists sent, each ending in one SGXD_SIGNAL_DONE; the kernel
// queue's copy, for a fault's description (sgxd_chain_op), and the position past its last op.
static uint32_t chains_sent;
static SgxdOp chain_mirror[SGXD_CHAIN_RING];
static uint32_t chain_mirror_end;
// The queue's ring (sgxdQueue): lists are written here and published through the events page's
// queue_end, which only this side writes; the kernel copies each op out as it runs it, and a
// list costs a syscall only when the queue had gone idle (sgxdKick).
static SgxdOp *ring;
static volatile uint32_t *queue_end;
// The waker's fields of the events page (sgxd_wake_arm), which this side writes.
static volatile SgxdEvents *wake_page;
// sgxd_chain_microkernel: the microkernel's state block, whose counts, fault and waker words
// stand for the events page's; NULL while IRQ 52's handler runs the lists.
static volatile uint32_t *mk;

static inline uint32_t twod_done(void) {
    return mk[SGXD_MK_TWOD];
}
static inline uint32_t chains_done(void) {
    return mk[SGXD_MK_CHAINS];
}
static inline uint32_t chain_fault(void) {
    return mk[SGXD_MK_FAULT];
}

#ifdef VITA_DIAGNOSTICS
// The app's sgxd syscalls counted (perflog): synchronous lists and kicks.
uint32_t sgxd_sync_submits, sgxd_kicks;
#endif

int sgxd_ptla_idle(void);

// The caller's time spun waiting for the GPU (sgxd_wait_us_take); counted in the diagnostics
// build only.
#ifdef VITA_DIAGNOSTICS
static uint32_t wait_us;
static uint64_t wait_us_total;

void sgxd_wait_us_add(uint32_t us) {
    wait_us += us;
    wait_us_total += us;
}

uint64_t sgxd_wait_us_total(void) {
    return wait_us_total;
}

uint32_t sgxd_wait_us_take(void) {
    const uint32_t us = wait_us;
    wait_us = 0;
    return us;
}
#endif

int sgxd_ptla_events(SgxdEvents *page, uint32_t mk_va, char *failed, unsigned failed_size) {
    if (!page) {
        sgxd_gpu_idle();
        if (mk) {
            sgxd_mkq_stop();
        }
        events = NULL;
        mk = NULL;
        sgxd_recipe_events_owned = 0;
        return sgxdEvents(NULL);
    }
    // The spins' clock must run: a stopped cycle counter would make a stopped GPU a hang
    // rather than a fault.
    const uint32_t c0 = azaharCyclesStart();
    for (volatile uint32_t i = 0; i < 1000; i++) {
    }
    if (azaharCycles() == c0) {
        snprintf(failed, failed_size, "this thread's cycle counter does not run");
        return -1;
    }
    if (events || sgxd_gpu_idle() < 0) {
        snprintf(failed, failed_size, events ? "the events page is registered already"
                                             : "the GPU did not go idle");
        return -1;
    }
    const int r = sgxdEvents(page);
    if (r < 0) {
        snprintf(failed, failed_size, "no GPU interrupt: sgxdEvents 0x%08x", (unsigned)r);
        return -1;
    }
    if (sgxd_mkq_start(mk_va, failed, failed_size) < 0) {
        sgxdEvents(NULL);
        return -1;
    }
    cycles_per_us = (uint32_t)scePowerGetArmClockFrequency();
    events = page;
    mk = sgxd_mkq_state();
    twod_sent = 0;
    chains_sent = 0;
    // The recipes' kicks leave core 0's event program on it, and their SLC requests are Sony's.
    sgxd_recipe_events_owned = 1;
    return 0;
}

const volatile SgxdEvents *sgxd_ptla_page(void) {
    return events;
}

uint32_t sgxd_ptla_fence(void) {
    return twod_sent;
}

int sgxd_ptla_reached(uint32_t fence) {
    return !events || (int32_t)(twod_done() - fence) >= 0;
}

uint32_t sgxd_chain_done(void) {
    return events ? chains_done() : chains_sent;
}

uint32_t sgxd_ptla_done(void) {
    return events ? twod_done() : twod_sent;
}

uint32_t sgxd_chain_faulted(void) {
    return events ? chain_fault() : 0;
}

int sgxd_ptla_idle(void) {
    if (!events || sgxd_ptla_reached(twod_sent)) {
        return 0;
    }
    const uint32_t start = azaharCyclesStart();
    const uint32_t limit = TWOD_POLL_US * cycles_per_us;
    while (!sgxd_ptla_reached(twod_sent)) {
        if (azaharCycles() - start > limit) {
            return -1;
        }
    }
#ifdef VITA_DIAGNOSTICS
    sgxd_wait_us_add((azaharCycles() - start) / cycles_per_us);
#endif
    return 0;
}

int sgxd_chain_submit(const SgxdOp *ops, unsigned count) {
    return sgxd_chain_submit_vars(ops, count, NULL, NULL);
}

int sgxd_chain_submit_vars(const SgxdOp *ops, unsigned count, const uint8_t *variable,
                           SgxdMkqCode *code) {
    if (count == 0 || count > SGXD_MAX_CHAIN_OPS) {
        return AZAHAR_ERR_ARG;
    }
    if (!events) {
        return AZAHAR_ERR_STATE;
    }
    const int r = sgxd_mkq_submit(ops, count, variable, code);
    if (r == 0) {
        chains_sent++;
    }
    // The ring's room comes back as the GPU runs the lists before: a full ring is the GPU a
    // whole ring behind.
    const uint32_t end = chain_end;
    if (end + count - events->queue_pos > SGXD_CHAIN_RING) {
        const uint32_t start = azaharCyclesStart();
        const uint32_t limit = TWOD_POLL_US * cycles_per_us;
        while (end + count - events->queue_pos > SGXD_CHAIN_RING) {
            if (events->chain_fault) {
                return AZAHAR_ERR_STATE;
            }
            if (azaharCycles() - start > limit) {
                return AZAHAR_ERR_FULL;
            }
        }
#ifdef VITA_DIAGNOSTICS
        sgxd_wait_us_add((azaharCycles() - start) / cycles_per_us);
#endif
    }
    for (unsigned i = 0; i < count; i++) {
        ring[(end + i) & (SGXD_CHAIN_RING - 1u)] = ops[i];
    }
    for (unsigned n = 0; n < count; n++) {
        chain_mirror[((uint32_t)r + n) & (SGXD_CHAIN_RING - 1u)] = ops[n];
    }
    chain_mirror_end = ((uint32_t)r + count) & 0x7fffffffu;
    chains_sent++;
    return 0;
}

int sgxd_ptla_waker(int32_t flag, uint32_t bits) {
    return sgxdWaker(flag, bits);
}

int sgxd_wake_arm(uint32_t chains, uint32_t twod) {
    // The targets before WAKE_ARMED, which the microkernel's check reads first (sgxd_mk.h);
    // WAKE_ARMED before the counts are read again (the GPU moves a count, then reads it).
    mk[SGXD_MK_WAKE_CHAINS] = chains;
    mk[SGXD_MK_WAKE_TWOD] = twod;
    __asm__ volatile("dmb" ::: "memory");
    mk[SGXD_MK_WAKE_ARMED] = 1;
    __asm__ volatile("dmb" ::: "memory");
    if ((int32_t)(mk[SGXD_MK_CHAINS] - chains) >= 0 && (int32_t)(mk[SGXD_MK_TWOD] - twod) >= 0) {
        mk[SGXD_MK_WAKE_ARMED] = 0;
        return 0;
    }
    return 1;
}

void sgxd_wake_disarm(void) {
    mk[SGXD_MK_WAKE_ARMED] = 0;
}

uint32_t sgxd_wake_count(void) {
    return mk ? mk[SGXD_MK_WAKES] : wake_page ? wake_page->wakes : 0;
}

const SgxdOp *sgxd_chain_op(uint32_t pos, uint32_t *behind) {
    const uint32_t back = (chain_mirror_end - pos) & 0x7fffffffu;
    if (back == 0 || back > SGXD_CHAIN_RING) {
        return NULL;
    }
    *behind = back;
    return &chain_mirror[pos & (SGXD_CHAIN_RING - 1u)];
}

uint32_t sgxd_chain_fence(void) {
    return chains_sent;
}

int sgxd_chain_reached(uint32_t fence) {
    return !events || (int32_t)(chains_done() - fence) >= 0;
}

int sgxd_chain_ta_reached(uint32_t fence) {
    return !events || (int32_t)(mk[SGXD_MK_CHAIN_TA] - fence) >= 0;
}

int sgxd_chain_wait(uint32_t fence) {
    if (!events || sgxd_chain_reached(fence)) {
        return 0;
    }
    // A second with no list completed (sgxd_chain_stall_us) is a stopped GPU; a deep queue takes
    // as long as it takes. The cycles since the last completion add up in 64 bits.
    const uint32_t start = azaharCyclesStart();
    const uint64_t stall = (uint64_t)sgxd_chain_stall_us * cycles_per_us;
    uint32_t last = start;
    uint64_t since_progress = 0;
    uint32_t done = chains_done();
    while (!sgxd_chain_reached(fence)) {
        const uint32_t now = azaharCycles();
        since_progress += now - last;
        last = now;
        if (chains_done() != done) {
            done = chains_done();
            since_progress = 0;
        }
        if (chain_fault() || since_progress > stall) {
            return -1;
        }
    }
#ifdef VITA_DIAGNOSTICS
    sgxd_wait_us_add((azaharCycles() - start) / cycles_per_us);
#endif
    return 0;
}

int sgxd_chain_idle(void) {
    return sgxd_chain_wait(chains_sent);
}

static int (*render_drain)(void);

void sgxd_set_render_drain(int (*drain)(void)) {
    render_drain = drain;
}

// A synchronous list's start: the ops before it and the last list done, the last list's 3D maybe
// not, and the microkernel's last run out of jobs (a synchronous list may reset the USE).
static int ptla_ready(void) {
    return sgxd_ptla_idle() == 0 && sgxd_chain_idle() == 0 && (!mk || sgxd_mkq_idle() == 0) ? 0
                                                                                           : -1;
}

int sgxd_gpu_idle(void) {
    if (render_drain && render_drain() < 0) {
        return -1;
    }
    return ptla_ready();
}

static uint32_t surf(uint32_t block, uint32_t format, uint32_t stride) {
    return block | format << 16 | stride;
}

static uint32_t xy(uint32_t x, uint32_t y) {
    return x << 13 | y;
}

static unsigned slc(SgxdOp *ops) {
    const SgxdOp list[] = {
        W(SLC_CTRL_FLUSH, 0x10), P(SLC_EVENT_STATUS, 2, 2, 1000), W(SLC_EVENT_CLEAR, 2),
        W(SLC_CTRL_INVAL, 1), P(SLC_EVENT_STATUS, 1, 1, 1000),
        W(SLC_EVENT_CLEAR, 1),
    };
    for (unsigned n = 0; n < sizeof(list) / sizeof(list[0]); n++) {
        ops[n] = list[n];
    }
    return sizeof(list) / sizeof(list[0]);
}

// A blit list's end, after FLUSH, with events: the microkernel clears TWOD_COMPLETE and counts
// it, so the leading clear and the poll go; a blit before the last waits for that count, the last
// returns. Returns the list's length.
static unsigned completion(SgxdOp *ops, unsigned n, int last) {
    if (!events) {
        return n;
    }
    for (unsigned i = 1; i < n - 2; i++) {
        ops[i - 1] = ops[i];
    }
    n -= 3;
    twod_sent++;
    if (!last) {
        const SgxdOp wait = {SGXD_OP_WAIT_TWOD, 0, twod_sent, 0, TWOD_POLL_US};
        ops[n++] = wait;
    }
    return n;
}

// A blit's list into ops: with the microkernel, the 2D clock on after its leading clear (Sony's
// PTLA kick). Returns its length.
static unsigned kick(SgxdOp *ops, const SgxdOp *list, unsigned count) {
    unsigned n = 0;
    for (unsigned k = 0; k < count; k++) {
        ops[n++] = list[k];
        if (k == 0 && events) {
            ops[n++] = (SgxdOp)W(CLKGATECTL2, CLKGATECTL2_2D_ON);
        }
    }
    return n;
}

// One fill blit of w x h pixels at (x, y) of the surface, then FLUSH and the completion.
static unsigned blit(SgxdOp *ops, const SgxdPtlaFill *f, uint32_t x, uint32_t y, uint32_t w,
                     uint32_t h, int last) {
    // Sony's own fill stream names the destination as the source too.
    const SgxdOp list[] = {
        W(EVENT_HOST_CLEAR, TWOD_COMPLETE),
        W(PTLA_CMD, surf(SRC_SURF, f->format, f->stride)),
        W(PTLA_CMD, f->va),
        W(PTLA_CMD, surf(DST_SURF, f->format, f->stride)),
        W(PTLA_CMD, f->va),
        W(PTLA_CMD, BLIT | BLIT_FILL),
        W(PTLA_CMD, f->colour),
        W(PTLA_CMD, xy(x, y)),
        W(PTLA_CMD, xy(w, h)),
        W(PTLA_CMD, FLUSH),
        P(EVENT_STATUS, TWOD_COMPLETE, TWOD_COMPLETE, TWOD_POLL_US),
        W(EVENT_HOST_CLEAR, TWOD_COMPLETE),
    };
    return completion(ops, kick(ops, list, sizeof(list) / sizeof(list[0])), last);
}

// One copy blit of w x h pixels from (x, y) of the source to (x, y) of the destination, then
// FLUSH and the completion.
static unsigned copy_blit(SgxdOp *ops, const SgxdPtlaCopy *c, uint32_t x, uint32_t y, uint32_t w,
                          uint32_t h, int last) {
    const SgxdOp list[] = {
        W(EVENT_HOST_CLEAR, TWOD_COMPLETE),
        W(PTLA_CMD, surf(SRC_SURF, c->format, c->stride)),
        W(PTLA_CMD, c->src),
        W(PTLA_CMD, SRCOFF | xy(c->src_x + x, y)),
        W(PTLA_CMD, xy(w, h)),
        W(PTLA_CMD, surf(DST_SURF, c->format, c->dst_stride ? c->dst_stride : c->stride)),
        W(PTLA_CMD, c->dst),
        W(PTLA_CMD, BLIT),
        W(PTLA_CMD, xy(c->dst_x + x, y)),
        W(PTLA_CMD, xy(w, h)),
        W(PTLA_CMD, FLUSH),
        P(EVENT_STATUS, TWOD_COMPLETE, TWOD_COMPLETE, TWOD_POLL_US),
        W(EVENT_HOST_CLEAR, TWOD_COMPLETE),
    };
    return completion(ops, kick(ops, list, sizeof(list) / sizeof(list[0])), last);
}

// With events: the ops as a list in the async queue, behind the lists before it, ending once
// its last blit has completed (the microkernel's TWOD count) with one TA signal and one DONE, so
// the lists' count stays the fence. Returns at once.
static int submit_queued(SgxdOp *ops, unsigned n, const char *what, char *failed,
                         unsigned failed_size) {
    ops[n++] = (SgxdOp){SGXD_OP_WAIT_TWOD, 0, twod_sent, 0, TWOD_POLL_US};
    const SgxdOp tail[] = {
        {SGXD_OP_SIGNAL, 0, SGXD_SIGNAL_TA, 0, 0},
        {SGXD_OP_SIGNAL, 0, SGXD_SIGNAL_DONE, 0, 0},
    };
    for (unsigned k = 0; k < sizeof(tail) / sizeof(tail[0]); k++) {
        ops[n++] = tail[k];
    }
    // The stream's words are the job's: one code a shape of fill or copy.
    uint8_t variable[64];
    for (unsigned k = 0; k < n; k++) {
        variable[k] = ops[k].op == SGXD_OP_WRITE && ops[k].reg == PTLA_CMD;
    }
#ifdef VITA_DIAGNOSTICS
    sgxd_mkq_kind_next = SGXD_MKQ_KIND_PTLA;
#endif
    const int r = sgxd_chain_submit_vars(ops, n, variable, NULL);
    if (r < 0) {
        snprintf(failed, failed_size, "PTLA %s: sgxd_chain_submit %08x", what, (unsigned)r);
        return -1;
    }
    return 0;
}

// Submits the list. The time and the failure's description are the diagnostics build's
// (elapsed_us 0 and failed untouched otherwise).
static int submit(SgxdOp *ops, unsigned n, const char *what, uint32_t *elapsed_us,
                  char *failed, unsigned failed_size) {
#ifdef VITA_DIAGNOSTICS
    sgxd_sync_submits++;
    const SceUInt64 start = sceKernelGetProcessTimeWide();
    const int done = sgxdSubmit(ops, n);
    if (elapsed_us) {
        *elapsed_us = (uint32_t)(sceKernelGetProcessTimeWide() - start);
    }
    if (done != (int)n) {
        const SgxdOp *at = done >= 0 && done < (int)n ? &ops[done] : NULL;
        snprintf(failed, failed_size,
                 "PTLA %s op %d of %u: kind %u reg 0x%05x value 0x%08x mask 0x%08x", what, done,
                 n, at ? at->op : 0, at ? at->reg : 0, at ? at->value : 0, at ? at->mask : 0);
        return -1;
    }
    return 0;
#else
    (void)what;
    (void)failed;
    (void)failed_size;
    if (elapsed_us) {
        *elapsed_us = 0;
    }
    return sgxdSubmit(ops, n) == (int)n ? 0 : -1;
#endif
}

int sgxd_ptla_fill(const SgxdPtlaFill *f, uint32_t *elapsed_us, char *failed,
                   unsigned failed_size) {
    if (f->width == 0 || f->width > SGXD_PTLA_MAX_EXTENT || f->rows > SGXD_PTLA_MAX_EXTENT ||
        f->tail >= f->width || f->stride == 0 || f->stride > 0xffffu ||
        (f->rows == 0 && f->tail == 0)) {
        snprintf(failed, failed_size,
                 "PTLA fill: %u rows of %u pixels and %u more, stride %u, is not one the "
                 "PTLA takes",
                 f->rows, f->width, f->tail, f->stride);
        return -1;
    }
    if (!events && ptla_ready() < 0) {
        snprintf(failed, failed_size, "PTLA fill: the GPU's previous op never completed");
        return -1;
    }
    // With events the tail goes first: its wait is one row's, and the list ends with the big
    // blit's completion; and no SLC request, as Sony's 2D makes none: the PTLA goes through the
    // SLC as the TA and the 3D do.
    SgxdOp ops[64];
    unsigned n = events ? 0 : slc(ops);
    if (f->tail && events) {
        n += blit(ops + n, f, 0, f->rows, f->tail, 1, !f->rows);
    }
    if (f->rows) {
        n += blit(ops + n, f, 0, 0, f->width, f->rows, !f->tail || events);
    }
    if (f->tail && !events) {
        n += blit(ops + n, f, 0, f->rows, f->tail, 1, 1);
    }
    if (events) {
        if (elapsed_us) {
            *elapsed_us = 0;
        }
        return submit_queued(ops, n, "fill", failed, failed_size);
    }
    n += slc(ops + n);
    return submit(ops, n, "fill", elapsed_us, failed, failed_size);
}

int sgxd_ptla_copy(const SgxdPtlaCopy *c, uint32_t *elapsed_us, char *failed,
                   unsigned failed_size) {
    if (c->width == 0 || c->width > SGXD_PTLA_MAX_EXTENT || c->rows > SGXD_PTLA_MAX_EXTENT ||
        c->src_x + c->width > SGXD_PTLA_MAX_EXTENT || c->dst_x + c->width > SGXD_PTLA_MAX_EXTENT ||
        c->tail >= c->width || c->stride == 0 || c->stride > 0xffffu || c->dst_stride > 0xffffu ||
        (c->rows == 0 && c->tail == 0)) {
        snprintf(failed, failed_size,
                 "PTLA copy: %u rows of %u pixels and %u more, stride %u, is not one the "
                 "PTLA takes",
                 c->rows, c->width, c->tail, c->stride);
        return -1;
    }
    if (!events && ptla_ready() < 0) {
        snprintf(failed, failed_size, "PTLA copy: the GPU's previous op never completed");
        return -1;
    }
    SgxdOp ops[64];
    unsigned n = events ? 0 : slc(ops);
    if (c->tail && events) {
        n += copy_blit(ops + n, c, 0, c->rows, c->tail, 1, !c->rows);
    }
    if (c->rows) {
        n += copy_blit(ops + n, c, 0, 0, c->width, c->rows, !c->tail || events);
    }
    if (c->tail && !events) {
        n += copy_blit(ops + n, c, 0, c->rows, c->tail, 1, 1);
    }
    if (events) {
        if (elapsed_us) {
            *elapsed_us = 0;
        }
        return submit_queued(ops, n, "copy", failed, failed_size);
    }
    n += slc(ops + n);
    return submit(ops, n, "copy", elapsed_us, failed, failed_size);
}
