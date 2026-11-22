// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// sgxd_ptla: the PTLA driven from the host (sgxd_ptla.h).

#include "sgxd_ptla.h"

#include <stdio.h>

#include <psp2/kernel/processmgr.h>

#include "sgxd.h"

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

#define SRCOFF 0x10000000u
#define SRC_SURF 0x40000000u
#define DST_SURF 0x50000000u
#define BLIT 0x60000000u
#define FLUSH 0x80000000u
#define BLIT_FILL (1u << 4)
#define SURF_TWIDDLED (2u << 22)

// A blit's wait: the hardware completes at the first read after FLUSH.
#define TWOD_POLL_US 1000000u

// sgxd_ptla_events: the ISR's page, and the completions asked for so far (one a FLUSH).
static const volatile SgxdEvents *events;
static uint32_t twod_sent;
// sgxd_chain_submit: the async lists sent, each ending in one SGXD_SIGNAL_DONE; the kernel
// queue's copy, for a fault's description (sgxd_chain_op), and the position past its last op.
static uint32_t chains_sent;
static SgxdOp chain_mirror[SGXD_CHAIN_RING];
static uint32_t chain_mirror_end;

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

int sgxd_ptla_events(SgxdEvents *page) {
    if (!page) {
        sgxd_gpu_idle();
        events = NULL;
        return sgxdEvents(NULL);
    }
    const int r = sgxdEvents(page);
    if (r == 0) {
        events = page;
        twod_sent = page->twod;
        chains_sent = page->chains;
    }
    return r;
}

const volatile SgxdEvents *sgxd_ptla_page(void) {
    return events;
}

uint32_t sgxd_ptla_fence(void) {
    return twod_sent;
}

int sgxd_ptla_reached(uint32_t fence) {
    return !events || (int32_t)(events->twod - fence) >= 0;
}

int sgxd_ptla_idle(void) {
    if (!events || sgxd_ptla_reached(twod_sent)) {
        return 0;
    }
    const SceUInt64 start = sceKernelGetProcessTimeWide();
    while (!sgxd_ptla_reached(twod_sent)) {
        if (sceKernelGetProcessTimeWide() - start > TWOD_POLL_US) {
            return -1;
        }
    }
#ifdef VITA_DIAGNOSTICS
    sgxd_wait_us_add((uint32_t)(sceKernelGetProcessTimeWide() - start));
#endif
    return 0;
}

int sgxd_chain_submit(const SgxdOp *ops, unsigned count) {
    // The queue's room comes back as the GPU runs the lists before: a full queue is the GPU a
    // whole queue behind.
    int r = sgxdSubmitAsync(ops, count);
    if (r == AZAHAR_ERR_FULL) {
        const SceUInt64 start = sceKernelGetProcessTimeWide();
        while (r == AZAHAR_ERR_FULL && sceKernelGetProcessTimeWide() - start <= TWOD_POLL_US) {
            r = sgxdSubmitAsync(ops, count);
        }
#ifdef VITA_DIAGNOSTICS
        sgxd_wait_us_add((uint32_t)(sceKernelGetProcessTimeWide() - start));
#endif
    }
    if (r < 0) {
        return r;
    }
    for (unsigned n = 0; n < count; n++) {
        chain_mirror[((uint32_t)r + n) & (SGXD_CHAIN_RING - 1u)] = ops[n];
    }
    chain_mirror_end = ((uint32_t)r + count) & 0x7fffffffu;
    chains_sent++;
    return 0;
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
    return !events || (int32_t)(events->chains - fence) >= 0;
}

int sgxd_chain_ta_reached(uint32_t fence) {
    return !events || (int32_t)(events->chain_ta - fence) >= 0;
}

int sgxd_chain_wait(uint32_t fence) {
    if (!events || sgxd_chain_reached(fence)) {
        return 0;
    }
    // A second with no list completed is a stopped GPU; a deep queue takes as long as it takes.
    const SceUInt64 start = sceKernelGetProcessTimeWide();
    SceUInt64 progress = start;
    uint32_t done = events->chains;
    while (!sgxd_chain_reached(fence)) {
        const SceUInt64 now = sceKernelGetProcessTimeWide();
        if (events->chains != done) {
            done = events->chains;
            progress = now;
        }
        if (events->chain_fault || now - progress > TWOD_POLL_US) {
            return -1;
        }
    }
#ifdef VITA_DIAGNOSTICS
    sgxd_wait_us_add((uint32_t)(sceKernelGetProcessTimeWide() - start));
#endif
    return 0;
}

int sgxd_chain_idle(void) {
    return sgxd_chain_wait(chains_sent);
}

int sgxd_gpu_idle(void) {
    return sgxd_ptla_idle() == 0 && sgxd_chain_idle() == 0 ? 0 : -1;
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
        W(SLC_CTRL_INVAL, 1),    P(SLC_EVENT_STATUS, 1, 1, 1000), W(SLC_EVENT_CLEAR, 1),
    };
    for (unsigned n = 0; n < sizeof(list) / sizeof(list[0]); n++) {
        ops[n] = list[n];
    }
    return sizeof(list) / sizeof(list[0]);
}

// A blit list's end, after FLUSH, with events: the ISR clears TWOD_COMPLETE and counts it, so
// the leading clear and the poll go; a blit before the last waits for the ISR's count, the last
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
    for (unsigned n = 0; n < sizeof(list) / sizeof(list[0]); n++) {
        ops[n] = list[n];
    }
    return completion(ops, sizeof(list) / sizeof(list[0]), last);
}

// One copy blit of w x h pixels from (x, y) of the source to (x, y) of the destination, then
// FLUSH and the completion.
static unsigned copy_blit(SgxdOp *ops, const SgxdPtlaCopy *c, uint32_t x, uint32_t y, uint32_t w,
                          uint32_t h, int last) {
    const SgxdOp list[] = {
        W(EVENT_HOST_CLEAR, TWOD_COMPLETE),
        W(PTLA_CMD, surf(SRC_SURF, c->format, c->stride)),
        W(PTLA_CMD, c->src),
        W(PTLA_CMD, SRCOFF | xy(x, y)),
        W(PTLA_CMD, xy(w, h)),
        W(PTLA_CMD, surf(DST_SURF, c->format, c->stride)),
        W(PTLA_CMD, c->dst),
        W(PTLA_CMD, BLIT),
        W(PTLA_CMD, xy(x, y)),
        W(PTLA_CMD, xy(w, h)),
        W(PTLA_CMD, FLUSH),
        P(EVENT_STATUS, TWOD_COMPLETE, TWOD_COMPLETE, TWOD_POLL_US),
        W(EVENT_HOST_CLEAR, TWOD_COMPLETE),
    };
    for (unsigned n = 0; n < sizeof(list) / sizeof(list[0]); n++) {
        ops[n] = list[n];
    }
    return completion(ops, sizeof(list) / sizeof(list[0]), last);
}

// With events: the ops as a list in the async queue, behind the lists before it, ending once
// its last blit has completed (the ISR's TWOD count) with one TA signal and one DONE, so the
// lists' count stays the fence. Returns at once.
static int submit_queued(SgxdOp *ops, unsigned n, const char *what, char *failed,
                         unsigned failed_size) {
    const SgxdOp tail[] = {
        {SGXD_OP_WAIT_TWOD, 0, twod_sent, 0, TWOD_POLL_US},
        {SGXD_OP_SIGNAL, 0, SGXD_SIGNAL_TA, 0, 0},
        {SGXD_OP_SIGNAL, 0, SGXD_SIGNAL_DONE, 0, 0},
    };
    for (unsigned k = 0; k < sizeof(tail) / sizeof(tail[0]); k++) {
        ops[n++] = tail[k];
    }
    const int r = sgxd_chain_submit(ops, n);
    if (r < 0) {
        snprintf(failed, failed_size, "PTLA %s: sgxdSubmitAsync %08x", what, (unsigned)r);
        return -1;
    }
    return 0;
}

// Submits the list. The time and the failure's description are the diagnostics build's
// (elapsed_us 0 and failed untouched otherwise).
static int submit(const SgxdOp *ops, unsigned n, const char *what, uint32_t *elapsed_us,
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
    if (sgxd_gpu_idle() < 0) {
        snprintf(failed, failed_size, "PTLA fill: the GPU's previous op never completed");
        return -1;
    }
    // With events the tail goes first: its wait is one row's, and the list ends with the big
    // blit's completion.
    SgxdOp ops[64];
    unsigned n = slc(ops);
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
        c->tail >= c->width || c->stride == 0 || c->stride > 0xffffu ||
        (c->rows == 0 && c->tail == 0)) {
        snprintf(failed, failed_size,
                 "PTLA copy: %u rows of %u pixels and %u more, stride %u, is not one the "
                 "PTLA takes",
                 c->rows, c->width, c->tail, c->stride);
        return -1;
    }
    if (sgxd_gpu_idle() < 0) {
        snprintf(failed, failed_size, "PTLA copy: the GPU's previous op never completed");
        return -1;
    }
    SgxdOp ops[64];
    unsigned n = slc(ops);
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
