// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// sgxd_session: the init script, the SLC and a render's phases around the recipe
// (sgxd_session.h).

#include "sgxd_session.h"
#include "sgxd_ptla.h"

#include <stdio.h>
#include <string.h>

#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>

#define W(r, v) {SGXD_OP_WRITE, (r), (v), 0, 0}
#define P(r, m, v, t) {SGXD_OP_POLL, (r), (v), (m), (t)}
#define R(r) {SGXD_OP_READ, (r), 0, 0, 0}

// The ops through sgxdSubmit, after any PTLA op still running (sgxd_ptla_idle); on a stop,
// the op it stopped at goes into failed.
static int submit(SgxdOp *ops, unsigned count, int overflow, const char *what, char *failed,
                  unsigned failed_size) {
    if (sgxd_gpu_idle() < 0) {
        snprintf(failed, failed_size, "%s: a PTLA op before it never completed", what);
        return -1;
    }
#ifdef VITA_DIAGNOSTICS
    sgxd_sync_submits++;
#endif
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

static uint32_t renders;

uint32_t sgxd_session_renders(void) {
    return renders;
}

#ifdef VITA_DIAGNOSTICS
// The diagnostics build times the TA and the 3D: it spins on core 0's EVENT_STATUS until bit
// is set (or a second passed) and returns the time of that read; the kernel's poll sleeps
// ~220 us after 100 us, which would time every small pass the same.
static uint64_t spin_until(uint32_t bit) {
    const uint64_t t0 = sceKernelGetProcessTimeWide();
    SgxdOp status = R(0x812c);
    uint64_t t;
    do {
        t = sceKernelGetProcessTimeWide();
        if (sgxdSubmit(&status, 1) != 1) {
            break;
        }
    } while (!(status.value & bit) && t - t0 < SGXD_SESSION_POLL_US);
    return t;
}
#endif

int sgxd_session_render(const SgxdRecipe *r, SgxdSessionRender *render, char *failed,
                        unsigned failed_size) {
    if (sgxd_session_phase(r, sgxd_recipe_ta_kick, "ta kick", failed, failed_size) < 0) {
        return -1;
    }
#ifdef VITA_DIAGNOSTICS
    const uint64_t t0 = sceKernelGetProcessTimeWide();
    const uint64_t t1 = spin_until(0x2000);
#endif
    if (sgxd_session_phase(r, sgxd_session_ta_wait, "ta wait", failed, failed_size) < 0) {
        return -1;
    }
    if (render->read_pages) {
        // The pages the pass took, counted since the free-list load (PAGE_STATUS's high half).
        SgxdOp page_status = R(0x0724);
        render->pages = sgxd_gpu_idle() == 0 && sgxdSubmit(&page_status, 1) == 1
                            ? page_status.value >> 16
                            : 0;
    }
    if (sgxd_session_phase(r, sgxd_recipe_ta_done, "ta done", failed, failed_size) < 0 ||
        sgxd_session_phase(r, sgxd_recipe_3d_kick, "3d kick", failed, failed_size) < 0) {
        return -1;
    }
#ifdef VITA_DIAGNOSTICS
    const uint64_t t2 = sceKernelGetProcessTimeWide();
    const uint64_t t3 = spin_until(0x40000);
#endif
    if (sgxd_session_phase(r, sgxd_session_3d_wait, "3d wait", failed, failed_size) < 0) {
        return -1;
    }
    if (sgxd_session_phase(r, sgxd_recipe_3d_done, "3d done", failed, failed_size) < 0) {
        return -1;
    }
    renders++;
    // The waits' times are the diagnostics build's; 0 otherwise.
#ifdef VITA_DIAGNOSTICS
    render->ta_us = (uint32_t)(t1 - t0);
    render->d3_us = (uint32_t)(t3 - t2);
    sgxd_wait_us_add(render->ta_us + render->d3_us);
#else
    render->ta_us = 0;
    render->d3_us = 0;
#endif
    return 0;
}

// ---------------------------------------------------------------------------- async

static SgxdOp chain_ops[SGXD_MAX_CHAIN_OPS];

static void add(SgxdOpList *l, uint32_t kind, uint32_t reg, uint32_t value, uint32_t mask) {
    if (l->count >= l->max) {
        l->overflow = 1;
        return;
    }
    l->ops[l->count++] = (SgxdOp){kind, reg, value, mask, 0};
}

#define TA_FINISHED 0x00002000u
#define PIXELBE_END_RENDER 0x00040000u
// The core-0 waits after the master's event: already met.
#define AFTER_EVENT_POLL_US 1000u

void sgxd_session_render_list(const SgxdRecipe *r, int read_pages, uint32_t ptla_fence,
                              SgxdOpList *l) {
    add(l, SGXD_OP_WAIT_TWOD, 0, ptla_fence, 0);
    // The TA kick makes Sony's SLC requests (sgxd_recipe_events_owned).
    sgxd_recipe_ta_kick(r, l);
    add(l, SGXD_OP_WAIT_EVENT, 0, 0, TA_FINISHED);
    sgxd_recipe_ta_wait(l, AFTER_EVENT_POLL_US);
    if (read_pages) {
        add(l, SGXD_OP_READ, 0x0724, 0, 0);   // PAGE_STATUS
    }
    add(l, SGXD_OP_SIGNAL, 0, SGXD_SIGNAL_TA, 0);
    sgxd_recipe_ta_done(r, l);
    sgxd_recipe_3d_kick(r, l);
    add(l, SGXD_OP_WAIT_EVENT, 0, 0, PIXELBE_END_RENDER);
    sgxd_recipe_3d_wait(l, AFTER_EVENT_POLL_US);
    sgxd_recipe_3d_done(r, l);
    add(l, SGXD_OP_SIGNAL, 0, SGXD_SIGNAL_DONE, 0);
}

int sgxd_session_render_async(const SgxdRecipe *r, int read_pages, char *failed,
                              unsigned failed_size) {
    // Queued behind the lists before it, which the kernel runs first.
    SgxdOpList l = {chain_ops, 0, SGXD_MAX_CHAIN_OPS, 0};
    sgxd_session_render_list(r, read_pages, sgxd_ptla_fence(), &l);
    if (l.overflow) {
        snprintf(failed, failed_size, "async render: more than %u ops", SGXD_MAX_CHAIN_OPS);
        return -1;
    }
    const int ret = sgxd_chain_submit(chain_ops, l.count);
    if (ret < 0) {
        snprintf(failed, failed_size, "async render: sgxd_chain_submit %08x", (unsigned)ret);
        return -1;
    }
    renders++;
    return 0;
}

// ------------------------------------------------------------------ overlapped lists
//
// The TA of pass N runs while the 3D of pass N - 1 does. A list ends right after its own 3D kick; the next
// list, or sgxd_session_finish_pending, waits for that 3D and runs its done phase.

// The pending 3D: its recipe, what it touches, whether a fence waits on it.
static const SgxdRecipe *pending;
static uint32_t pending_touches[6];
static int pending_fenced;

const SgxdRecipe *sgxd_session_pending(void) {
    return pending;
}

int sgxd_session_pending_touches(uint32_t lo, uint32_t hi) {
    if (!pending) {
        return 0;
    }
    for (unsigned n = 0; n < 6; n += 2) {
        if (lo < pending_touches[n + 1] && pending_touches[n] < hi) {
            return 1;
        }
    }
    return 0;
}

void sgxd_session_pending_fence(void) {
    if (pending) {
        pending_fenced = 1;
    }
}

int sgxd_session_pending_fenced(void) {
    return pending && pending_fenced;
}

int sgxd_session_ta_over_3d = 1;

// The pending 3D's wait and done.
static void finish_phase(SgxdOpList *l) {
    add(l, SGXD_OP_WAIT_EVENT, 0, 0, PIXELBE_END_RENDER);
    sgxd_recipe_3d_wait(l, AFTER_EVENT_POLL_US);
    sgxd_recipe_3d_done(pending, l);
}

int sgxd_session_render_overlap(const SgxdRecipe *r, const uint32_t touches[6], char *failed,
                                unsigned failed_size) {
    const int finish_previous = pending != NULL;
    SgxdOpList l = {chain_ops, 0, SGXD_MAX_CHAIN_OPS, 0};
    add(&l, SGXD_OP_WAIT_TWOD, 0, sgxd_ptla_fence(), 0);
    if (finish_previous && !sgxd_session_ta_over_3d) {
        finish_phase(&l);
    }
    sgxd_recipe_slc(&l, 1);
    sgxd_recipe_ta_kick(r, &l);
    add(&l, SGXD_OP_STAMP, 0, 0, 0);
    add(&l, SGXD_OP_WAIT_EVENT, 0, 0, TA_FINISHED);
    add(&l, SGXD_OP_STAMP, 0, 1, 0);
    sgxd_recipe_ta_wait(&l, AFTER_EVENT_POLL_US);
    add(&l, SGXD_OP_SIGNAL, 0, SGXD_SIGNAL_TA, 0);
    sgxd_recipe_ta_done(r, &l);
    if (finish_previous && sgxd_session_ta_over_3d) {
        finish_phase(&l);
    }
    sgxd_recipe_3d_kick(r, &l);
    add(&l, SGXD_OP_STAMP, 0, 2, 0);
    add(&l, SGXD_OP_SIGNAL, 0, SGXD_SIGNAL_DONE, 0);
    if (l.overflow) {
        snprintf(failed, failed_size, "overlapped render: more than %u ops", SGXD_MAX_CHAIN_OPS);
        return -1;
    }
    const int ret = sgxd_chain_submit(chain_ops, l.count);
    if (ret < 0) {
        snprintf(failed, failed_size, "overlapped render: sgxd_chain_submit %08x", (unsigned)ret);
        return -1;
    }
    pending = r;
    pending_fenced = 0;
    for (unsigned n = 0; n < 6; n++) {
        pending_touches[n] = touches ? touches[n] : 0;
    }
    renders++;
    return 0;
}

int sgxd_session_finish_pending(char *failed, unsigned failed_size) {
    if (!pending) {
        return 0;
    }
    SgxdOpList l = {chain_ops, 0, SGXD_MAX_CHAIN_OPS, 0};
    // One TA signal and one DONE a list, so both counts stay the lists' count.
    add(&l, SGXD_OP_SIGNAL, 0, SGXD_SIGNAL_TA, 0);
    add(&l, SGXD_OP_WAIT_EVENT, 0, 0, PIXELBE_END_RENDER);
    sgxd_recipe_3d_wait(&l, AFTER_EVENT_POLL_US);
    sgxd_recipe_3d_done(pending, &l);
    add(&l, SGXD_OP_SIGNAL, 0, SGXD_SIGNAL_DONE, 0);
#ifdef VITA_DIAGNOSTICS
    sgxd_mkq_kind_next = SGXD_MKQ_KIND_FINISH;
    sgxd_session_finishes++;
#endif
    const int ret = sgxd_chain_submit(chain_ops, l.count);
    if (ret < 0) {
        snprintf(failed, failed_size, "render finish: sgxd_chain_submit %08x", (unsigned)ret);
        return -1;
    }
    pending = NULL;
    pending_fenced = 0;
    return 0;
}

void sgxd_session_render_overlap_result(SgxdSessionRender *render) {
    render->ta_us = 0;
    render->d3_us = 0;
    render->pages = 0;
}

void sgxd_session_render_async_result(SgxdSessionRender *render) {
    render->ta_us = 0;
    render->d3_us = 0;
    render->pages = render->read_pages ? sgxd_mkq_state()[SGXD_MK_VALUE] >> 16 : 0;
}

void sgxd_session_render_async_failed(char *failed, unsigned failed_size) {
    const volatile uint32_t *mk = sgxd_mkq_state();
    if (!mk) {
        snprintf(failed, failed_size, "async lists: no microkernel");
        return;
    }
    const volatile SgxdEvents *ev = sgxd_ptla_page();
    const uint32_t fault = ev ? ev->chain_fault : 0;
    const int stopped = sgxdSubmitAsync(NULL, 0);
    if (stopped <= 0 && !fault) {
        snprintf(failed, failed_size, "async lists: none stopped, %u of %u done",
                 ev ? ev->chains : 0, sgxd_chain_fence());
        return;
    }
    const uint32_t at = (stopped > 0 ? (uint32_t)stopped : fault) - 1u;
    uint32_t behind = 0;
    const SgxdOp *op = sgxd_chain_op(at & 0x7fffffffu, &behind);
    snprintf(failed, failed_size,
             "async queue op %u, %u from the end (%s, %u of %u lists done): kind %u reg 0x%05x "
             "value 0x%08x mask 0x%08x, last read 0x%08x",
             (unsigned)(at & 0x7fffffffu), (unsigned)behind,
             fault ? "a poll timed out" : "no interrupt", ev ? ev->chains : 0,
             sgxd_chain_fence(), op ? op->op : 0, op ? op->reg : 0, op ? op->value : 0,
             op ? op->mask : 0, ev ? ev->chain_value : 0);
}
