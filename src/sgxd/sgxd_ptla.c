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

#define SRC_SURF 0x40000000u
#define DST_SURF 0x50000000u
#define BLIT 0x60000000u
#define FLUSH 0x80000000u
#define BLIT_FILL (1u << 4)
#define SURF_TWIDDLED (2u << 22)

// A blit's wait: the hardware completes at the first read after FLUSH.
#define TWOD_POLL_US 1000000u

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

static unsigned blit(SgxdOp *ops, const SgxdPtlaFill *f, uint32_t x, uint32_t y, uint32_t w,
                     uint32_t h) {
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
    return sizeof(list) / sizeof(list[0]);
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
    SgxdOp ops[64];
    unsigned n = slc(ops);
    if (f->rows) {
        n += blit(ops + n, f, 0, 0, f->width, f->rows);
    }
    if (f->tail) {
        n += blit(ops + n, f, 0, f->rows, f->tail, 1);
    }
    n += slc(ops + n);

    const SceUInt64 start = sceKernelGetProcessTimeWide();
    const int done = sgxdSubmit(ops, n);
    if (elapsed_us) {
        *elapsed_us = (uint32_t)(sceKernelGetProcessTimeWide() - start);
    }
    if (done != (int)n) {
        const SgxdOp *at = done >= 0 && done < (int)n ? &ops[done] : NULL;
        snprintf(failed, failed_size,
                 "PTLA fill op %d of %u: kind %u reg 0x%05x value 0x%08x mask 0x%08x", done, n,
                 at ? at->op : 0, at ? at->reg : 0, at ? at->value : 0, at ? at->mask : 0);
        return -1;
    }
    return 0;
}
