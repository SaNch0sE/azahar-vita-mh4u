// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// sgxd_session: the init script, the SLC and a render's phases around the recipe
// (sgxd_session.h).

#include "sgxd_session.h"

#include <stdio.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>

#define W(r, v) {SGXD_OP_WRITE, (r), (v), 0, 0}
#define P(r, m, v, t) {SGXD_OP_POLL, (r), (v), (m), (t)}
#define R(r) {SGXD_OP_READ, (r), 0, 0, 0}

// The ops through sgxdSubmit; on a stop, the op it stopped at goes into failed.
static int submit(SgxdOp *ops, unsigned count, int overflow, const char *what, char *failed,
                  unsigned failed_size) {
    const int done = overflow ? -1 : sgxdSubmit(ops, count);
    if (done != (int)count) {
        const SgxdOp *at = done >= 0 && done < (int)count ? &ops[done] : NULL;
        snprintf(failed, failed_size, "%s op %d of %u: kind %u reg 0x%05x value 0x%08x mask 0x%08x",
                 what, done, count, at ? at->op : 0, at ? at->reg : 0, at ? at->value : 0,
                 at ? at->mask : 0);
        return -1;
    }
    return 0;
}

void sgxd_session_recipe_addresses(const SgxdRecipe *r, uint32_t kicker_va,
                                   SgxdSessionAddresses *a) {
    memset(a, 0, sizeof(*a));
    a->kicker = kicker_va;
    a->bank0_pds = a->core0_pds = sgxd_recipe_va(r, SGXD_R_IDLE);
    a->bank0_units = a->core0_units = 1;
    for (unsigned n = 0; n < 16; n++) {
        a->code[n] = sgxd_recipe_va(r, n == 1 ? SGXD_R_CF : SGXD_R_CV);
    }
}

int sgxd_session_init_script(const SgxdSessionAddresses *a, char *failed, unsigned failed_size) {
    SgxdOp ops[64];
    unsigned n = 0;
    const SgxdOp head[] = {
        W(0x001c, 0), W(0x0130, 0x4000), W(0x0110, 0xc00), W(0x013c, 0), W(0x0a7c, 0),
        W(0x0a80, 0), W(0x0a74, 0x208200), W(0x0abc, 0x40), W(0x0ab8, 0), W(0x0800, 0x10),
        W(0x0818, 0), W(0x0804, 0x5e0), W(0x0814, 0xffff), W(0x0a00, 0x7c000),
    };
    memcpy(ops, head, sizeof(head));
    n = sizeof(head) / sizeof(head[0]);
    for (unsigned base = 0; base < 16; base++) {
        ops[n++] = (SgxdOp)W(0x0a0c + base * 4, a->code[base] >> 6);
    }
    const SgxdOp tail[] = {
        W(0x0b30, 0x100), W(0x0ac4, a->kicker),
        W(0x0a68, a->bank0_pds), W(0x0a6c, a->bank0_units), W(0x0a70, 0x12001),
        W(0x8a68, a->core0_pds), W(0x8a6c, a->core0_units), W(0x8a70, 0x12001),
        W(0x8a58, 0x2200c000), W(0x4a58, 0x804300f), W(0x4808, 4), W(0x4144, 0x103e8),
        W(0x414c, 0), W(0x0bb4, 0),
    };
    memcpy(ops + n, tail, sizeof(tail));
    n += sizeof(tail) / sizeof(tail[0]);
    return submit(ops, n, 0, "init script", failed, failed_size);
}

int sgxd_session_slc(int inval, char *failed, unsigned failed_size) {
    SgxdOp ops[6] = {
        W(0x4d2c, 0x10), P(0x4d10, 2, 2, 1000), W(0x4d14, 2),
        W(0x4d28, 1), P(0x4d10, 1, 1, 1000), W(0x4d14, 1),
    };
    return submit(ops, inval ? 6 : 3, 0, inval ? "SLC flush and invalidate" : "SLC flush",
                  failed, failed_size);
}

int sgxd_session_init(const SgxdRecipe *r, uint32_t kicker_va, char *failed,
                      unsigned failed_size) {
    SgxdSessionAddresses a;
    sgxd_session_recipe_addresses(r, kicker_va, &a);
    if (sgxd_session_slc(1, failed, failed_size) < 0) {
        return -1;
    }
    return sgxd_session_init_script(&a, failed, failed_size);
}

static const SgxdRecipe *reinit_recipe;
static uint32_t reinit_kicker;

void sgxd_session_set_reinit(const SgxdRecipe *r, uint32_t kicker_va) {
    reinit_recipe = r;
    reinit_kicker = kicker_va;
}

int sgxd_session_reinit(char *failed, unsigned failed_size) {
    if (!reinit_recipe) {
        snprintf(failed, failed_size, "no session to set up again after the GPU's reset");
        return -1;
    }
    return sgxd_session_resume(reinit_recipe, reinit_kicker, failed, failed_size);
}

int sgxd_session_phase(const SgxdRecipe *r, SgxdSessionBuild build, const char *what,
                       char *failed, unsigned failed_size) {
    static SgxdOp ops[SGXD_MAX_OPS];
    SgxdOpList list = {ops, 0, SGXD_MAX_OPS, 0};
    build(r, &list);
    return submit(ops, list.count, list.overflow, what, failed, failed_size);
}

void sgxd_session_ta_wait(const SgxdRecipe *r, SgxdOpList *list) {
    (void)r;
    sgxd_recipe_ta_wait(list, SGXD_SESSION_POLL_US);
}

void sgxd_session_3d_wait(const SgxdRecipe *r, SgxdOpList *list) {
    (void)r;
    sgxd_recipe_3d_wait(list, SGXD_SESSION_POLL_US);
}

void sgxd_session_slc_inval(const SgxdRecipe *r, SgxdOpList *list) {
    (void)r;
    sgxd_recipe_slc(list, 1);
}

int sgxd_session_render(const SgxdRecipe *r, SgxdSessionRender *render, char *failed,
                        unsigned failed_size) {
    if (sgxd_session_phase(r, sgxd_recipe_ta_kick, "ta kick", failed, failed_size) < 0) {
        return -1;
    }
    const uint64_t t0 = sceKernelGetProcessTimeWide();
    if (sgxd_session_phase(r, sgxd_session_ta_wait, "ta wait", failed, failed_size) < 0) {
        return -1;
    }
    const uint64_t t1 = sceKernelGetProcessTimeWide();
    if (render->read_pages) {
        // The pages the pass took, counted since the free-list load (PAGE_STATUS's high half).
        SgxdOp page_status = R(0x0724);
        render->pages = sgxdSubmit(&page_status, 1) == 1 ? page_status.value >> 16 : 0;
    }
    if (sgxd_session_phase(r, sgxd_recipe_ta_done, "ta done", failed, failed_size) < 0 ||
        sgxd_session_phase(r, sgxd_recipe_3d_kick, "3d kick", failed, failed_size) < 0) {
        return -1;
    }
    const uint64_t t2 = sceKernelGetProcessTimeWide();
    if (sgxd_session_phase(r, sgxd_session_3d_wait, "3d wait", failed, failed_size) < 0) {
        return -1;
    }
    const uint64_t t3 = sceKernelGetProcessTimeWide();
    if (sgxd_session_phase(r, sgxd_recipe_3d_done, "3d done", failed, failed_size) < 0) {
        return -1;
    }
    render->ta_us = (uint32_t)(t1 - t0);
    render->d3_us = (uint32_t)(t3 - t2);
    return 0;
}
