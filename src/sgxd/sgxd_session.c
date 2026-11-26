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
// Set while sgxd_session_resume runs: the GPU just taken again is idle, and the drain
// sgxd_gpu_idle runs first is the render thread's own.
static int resuming;

static int submit(SgxdOp *ops, unsigned count, int overflow, const char *what, char *failed,
                  unsigned failed_size) {
    if (!resuming && sgxd_gpu_idle() < 0) {
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
    // EUR_CR_MASTER_MP_PRIMITIVE: Sony's 0x103e8 (TE split, runs of 1000 primitives) with
    // SPLIT_MODE (bit 18) set: Sony's value alone stops the TA in the clipper where one core's
    // run starts at a given primitive.
    const SgxdOp tail[] = {
        W(0x0b30, 0x100), W(0x0ac4, a->kicker),
        W(0x0a68, a->bank0_pds), W(0x0a6c, a->bank0_units), W(0x0a70, 0x12001),
        W(0x8a68, a->core0_pds), W(0x8a6c, a->core0_units), W(0x8a70, 0x12001),
        W(0x8a58, 0x2200c000), W(0x4a58, 0x804300f), W(0x4808, 4), W(0x4144, 0x503e8),
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

int sgxd_session_resume(const SgxdRecipe *r, uint32_t kicker_va, char *failed,
                        unsigned failed_size) {
    resuming = 1;
    const int done = sgxd_session_init(r, kicker_va, failed, failed_size);
    resuming = 0;
    return done;
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

#if defined(VITA_DIAGNOSTICS) || defined(VITA_DEBUG_TOOLS)
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

#if defined(VITA_DEBUG_TOOLS) && !defined(VITA_DIAGNOSTICS)
// Tools builds: each synchronous render's time from its kick to its 3D's end (the caller waits all
// of it), in the GPU wait counter (sgxd_wait_us_take).
static uint64_t tools_render_start;
int sgxd_session_time_renders;
#endif

int sgxd_session_render(const SgxdRecipe *r, SgxdSessionRender *render, char *failed,
                        unsigned failed_size) {
#if defined(VITA_DEBUG_TOOLS) && !defined(VITA_DIAGNOSTICS)
    tools_render_start = sceKernelGetProcessTimeWide();
#endif
    if (sgxd_session_phase(r, sgxd_recipe_ta_kick, "ta kick", failed, failed_size) < 0) {
        return -1;
    }
#ifdef VITA_DIAGNOSTICS
    const uint64_t t0 = sceKernelGetProcessTimeWide();
    const uint64_t t1 = spin_until(0x2000);
#elif defined(VITA_DEBUG_TOOLS)
    // Tools builds time a render only for the slow-frame recorder (sgxd_session_time_renders).
    const int timed = sgxd_session_time_renders;
    const uint64_t t0 = timed ? sceKernelGetProcessTimeWide() : 0;
    const uint64_t t1 = timed ? spin_until(0x2000) : 0;
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
#elif defined(VITA_DEBUG_TOOLS)
    const uint64_t t2 = timed ? sceKernelGetProcessTimeWide() : 0;
    const uint64_t t3 = timed ? spin_until(0x40000) : 0;
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
#ifdef VITA_DEBUG_TOOLS
    if (timed) {
        render->ta_us = (uint32_t)(t1 - t0);
        render->d3_us = (uint32_t)(t3 - t2);
    }
    sgxd_wait_us_add((uint32_t)(sceKernelGetProcessTimeWide() - tools_render_start));
#endif
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

// A render as the microkernel's two streams run it (sgxd_mk.h): the TA stream's part, then
// SGXD_OP_STREAM_3D and the 3D stream's. The TA side runs ahead over any 3D in flight, as far as
// the parameter buffer lets it (WAIT_PAGES). Each TA done stores its scene's DPM state, which
// its 3D kick loads. The 3D side kicks, and runs its done phase, only with no TA in flight (as in
// Sony's trace). The 3D waits for its own end, so nothing is pending when its job is complete.

// The pages a TA may start with while a binned scene waits for its 3D: the threshold its context's
// pages stall at (ZLS_PAGE_THRESHOLD, every binned scene's counted) less SGXD_RECIPE_PASS_PAGES,
// which the TA can then take (a TA out of pages stops the GPU, docs/sgxd/ta.md). With no scene
// waiting it starts whatever is allocated, as the TA over the one 3D in flight always has.
static uint32_t page_budget(const SgxdRecipe *r) {
    return r->pds_threshold > SGXD_RECIPE_PASS_PAGES ? r->pds_threshold - SGXD_RECIPE_PASS_PAGES
                                                       : 0;
}

// The TA stream's part: ops[0] the PTLA wait and, with wait_3ds, ops[1] a wait for every earlier
// submit's 3D (sgxd_session_ta_over_3d 0); their values the caller's.
static void ta_part(const SgxdRecipe *r, int wait_3ds, int read_pages, SgxdOpList *l) {
    add(l, SGXD_OP_WAIT_TWOD, 0, 0, 0);
    if (wait_3ds) {
        add(l, SGXD_OP_WAIT_COUNT, SGXD_MK_CHAINS, 0, 0);
    }
    add(l, SGXD_OP_WAIT_PAGES, 0, page_budget(r), 0);
    add(l, SGXD_OP_SIGNAL, 0, SGXD_SIGNAL_TA_START, 0);
    // The TA kick makes Sony's SLC requests (sgxd_recipe_events_owned).
    sgxd_recipe_ta_kick(r, l);
    add(l, SGXD_OP_WAIT_EVENT, 0, 0, TA_FINISHED);
    sgxd_recipe_ta_wait(l, AFTER_EVENT_POLL_US);
    if (read_pages) {
        add(l, SGXD_OP_READ, 0x0724, 0, 0);   // PAGE_STATUS
    }
    sgxd_recipe_ta_done(r, l);
    add(l, SGXD_OP_SIGNAL, 0, SGXD_SIGNAL_TA_SCENE, 0);
}

// The 3D stream's part, from the split on.
static void d3_part(const SgxdRecipe *r, SgxdOpList *l) {
    add(l, SGXD_OP_STREAM_3D, 0, 0, 0);
    add(l, SGXD_OP_WAIT_TA_IDLE, 0, 0, 0);
    add(l, SGXD_OP_SIGNAL, 0, SGXD_SIGNAL_D3_KICK, 0);
    sgxd_recipe_3d_kick(r, l);
    add(l, SGXD_OP_WAIT_EVENT, 0, 0, PIXELBE_END_RENDER);
    add(l, SGXD_OP_WAIT_TA_IDLE, 0, 0, 0);
    sgxd_recipe_3d_wait(l, AFTER_EVENT_POLL_US);
    sgxd_recipe_3d_done(r, l);
}

void sgxd_session_render_list(const SgxdRecipe *r, int read_pages, uint32_t ptla_fence,
                              SgxdOpList *l) {
    const unsigned first = l->count;
    ta_part(r, 0, read_pages, l);
    if (!l->overflow) {
        l->ops[first].value = ptla_fence;
    }
    d3_part(r, l);
}

int sgxd_session_render_async(const SgxdRecipe *r, int read_pages, char *failed,
                              unsigned failed_size) {
    // Queued behind the lists before it, which the kernel runs first. Built and sent between two
    // suspends: its DPM loads are the state's (sgxd_mkq_enter).
    sgxd_mkq_enter();
    SgxdOpList l = {chain_ops, 0, SGXD_MAX_CHAIN_OPS, 0};
    sgxd_session_render_list(r, read_pages, sgxd_ptla_fence(), &l);
    if (l.overflow) {
        sgxd_mkq_leave();
        snprintf(failed, failed_size, "async render: more than %u ops", SGXD_MAX_CHAIN_OPS);
        return -1;
    }
    const int ret = sgxd_chain_submit_entered(chain_ops, l.count, NULL, NULL);
    if (ret < 0) {
        snprintf(failed, failed_size, "async render: sgxd_chain_submit %08x", (unsigned)ret);
        return -1;
    }
    renders++;
    return 0;
}

// ------------------------------------------------------------------ overlapped lists
//
// No 3D is left pending for a later list: the 3D stream's part waits for its own end. These stay
// for their callers, with nothing to do.

const SgxdRecipe *sgxd_session_pending(void) {
    return NULL;
}

int sgxd_session_pending_touches(uint32_t lo, uint32_t hi) {
    (void)lo;
    (void)hi;
    return 0;
}

void sgxd_session_pending_fence(void) {
}

int sgxd_session_pending_fenced(void) {
    return 0;
}

int sgxd_session_ta_over_3d = 1;

// A pass's list, its first op the PTLA wait and, without the TA over the 3D, its second the wait
// for the earlier submits' 3Ds (their values the caller's).
static void build_overlap(const SgxdRecipe *r, SgxdOpList *l) {
    ta_part(r, !sgxd_session_ta_over_3d, 0, l);
    d3_part(r, l);
}

// ------------------------------------------------------------------ kept lists
//
// A pass's list is the same ops at every render on a recipe but for a few values: the PTLA fence,
// the earlier submits it may wait for, and the SGXD_KEPT_* fields. A list is kept with what it was
// built from (the recipe's other fields the kicks read, the free lists' loads, the TA's wait);
// a render whose key a kept list has sends it with the values rewritten in place, any other
// builds one in the slot used longest ago.
//
// A list is its recipe's while the recipe lives (sgxd_session_forget): a new recipe at a freed
// one's address with the same layout would otherwise send the freed one's list.

#define KEPT_LISTS 64
#define KEPT_OPS 384
#define KEPT_VALUES 32
#define KEY_WORDS (SGXD_R_COUNT + 17)

typedef struct KeptList {
    const SgxdRecipe *r;
    uint32_t used; // the kept_clock when last sent
    uint32_t key[KEY_WORDS];
    unsigned count, nkept;
    SgxdKept kept[KEPT_VALUES];
    SgxdOp ops[KEPT_OPS];
    // With the microkernel: the ops whose values are rewritten (the job carries them) and the
    // list's code.
    uint8_t variable[KEPT_OPS];
    SgxdMkqCode code;
} KeptList;

static KeptList kept_lists[KEPT_LISTS];
static uint32_t kept_clock;
#ifdef VITA_DIAGNOSTICS
uint32_t sgxd_session_kept_sent, sgxd_session_kept_built, sgxd_session_finishes;
#endif

static void kept_key(const SgxdRecipe *r, uint32_t key[KEY_WORDS]) {
    int ta = 0, d3 = 0;
    sgxd_recipe_pb_loaded(&ta, &d3);
    memcpy(key, r->offset, sizeof(r->offset));
    uint32_t *k = key + SGXD_R_COUNT;
    *k++ = r->base;
    *k++ = (uint32_t)r->keep_code_caches;
    *k++ = (uint32_t)r->shared_pb;
    *k++ = r->req_base;
    *k++ = r->pt;
    *k++ = r->ta_free_list;
    *k++ = r->ta_alloc;
    *k++ = r->ta_threshold;
    *k++ = r->pds_threshold;
    *k++ = r->global_list;
    *k++ = r->pixev_va;
    *k++ = r->pixev_units;
    *k++ = r->pixev_attribute_size;
    *k++ = (uint32_t)ta;
    *k++ = (uint32_t)d3;
    *k++ = (uint32_t)sgxd_session_ta_over_3d;
    *k++ = r->samples;
}

void sgxd_session_forget(const SgxdRecipe *r) {
    for (unsigned n = 0; n < KEPT_LISTS; n++) {
        if (!r || kept_lists[n].r == r) {
            kept_lists[n].r = NULL;
        }
    }
}

// The values a sent list's waits take: the PTLA count, and the earlier submits' count.
static void set_waits(SgxdOp *ops) {
    ops[0].value = sgxd_ptla_fence();
    if (!sgxd_session_ta_over_3d) {
        ops[1].value = sgxd_mkq_jobs_sent();
    }
}

int sgxd_session_render_overlap(const SgxdRecipe *r, const uint32_t touches[6], char *failed,
                                unsigned failed_size) {
    (void)touches;
    // Both streams' lists need the shared parameter buffer: Sony's DPM slots, a TA over a 3D.
    if (!r->shared_pb) {
        snprintf(failed, failed_size, "overlapped render: a recipe with its own parameter buffer");
        return -1;
    }
    // Keyed, built and sent between two suspends: its DPM loads are the state's (sgxd_mkq_enter).
    sgxd_mkq_enter();
    uint32_t key[KEY_WORDS];
    kept_key(r, key);
    KeptList *k = NULL, *oldest = &kept_lists[0];
    for (unsigned n = 0; n < KEPT_LISTS && !k; n++) {
        KeptList *const at = &kept_lists[n];
        if (at->r == r && memcmp(at->key, key, sizeof(key)) == 0) {
            k = at;
        } else if (!at->r || (oldest->r && (int32_t)(at->used - oldest->used) < 0)) {
            oldest = at;
        }
    }
    const int kept = k != NULL;
    if (!kept) {
        k = oldest;
        SgxdOpList l = {k->ops, 0, KEPT_OPS, 0, k->kept, 0, KEPT_VALUES};
        build_overlap(r, &l);
        if (l.overflow) {
            k->r = NULL;
            sgxd_mkq_leave();
            snprintf(failed, failed_size, "overlapped render: more than %u ops or %u kept values",
                     KEPT_OPS, KEPT_VALUES);
            return -1;
        }
        k->r = r;
        memcpy(k->key, key, sizeof(key));
        k->count = l.count;
        k->nkept = l.nkept;
        memset(k->variable, 0, sizeof(k->variable));
        for (unsigned n = 0; n < k->nkept; n++) {
            k->variable[k->kept[n].op] = 1;
        }
        k->code = (SgxdMkqCode){0, 0, 0};
#ifdef VITA_DIAGNOSTICS
        sgxd_session_kept_built++;
#endif
    }
    k->used = ++kept_clock;
    set_waits(k->ops);
    for (unsigned n = 0; n < k->nkept; n++) {
        k->ops[k->kept[n].op].value = sgxd_recipe_kept_value(r, k->kept[n].field);
    }
    if (kept) {
        sgxd_recipe_pb_kicked(r);
#ifdef VITA_DIAGNOSTICS
        sgxd_session_kept_sent++;
#endif
    }
#ifdef VITA_DIAGNOSTICS
    sgxd_mkq_kind_next = SGXD_MKQ_KIND_RENDER;
#endif
    const int ret = sgxd_chain_submit_entered(k->ops, k->count, k->variable, &k->code);
    if (ret < 0) {
        snprintf(failed, failed_size, "overlapped render: sgxd_chain_submit %08x", (unsigned)ret);
        return -1;
    }
    renders++;
    return 0;
}

int sgxd_session_finish_pending(char *failed, unsigned failed_size) {
    (void)failed;
    (void)failed_size;
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
    // The microkernel cannot be stopped where it waits: its state says where it is.
    const uint32_t fault = mk[SGXD_MK_FAULT];
    snprintf(failed, failed_size,
             "microkernel: %s; job %u of %u taken (busy %u), resume at %u, %u of %u lists done, "
             "PTLA %u of %u; the last read 0x%08x",
             fault ? "a poll timed out" : "no event came", (unsigned)mk[SGXD_MK_POS],
             (unsigned)mk[SGXD_MK_END], (unsigned)mk[SGXD_MK_BUSY], (unsigned)mk[SGXD_MK_RESUME],
             (unsigned)mk[SGXD_MK_CHAINS], sgxd_chain_fence(), (unsigned)mk[SGXD_MK_TWOD],
             sgxd_ptla_fence(), (unsigned)mk[SGXD_MK_VALUE]);
    if (fault) {
        const size_t used = strlen(failed);
        snprintf(failed + used, failed_size - used, ", op %u of job %u (kind %u)",
                 (unsigned)(fault - 1u), (unsigned)(mk[SGXD_MK_POS] - 1u),
                 (unsigned)mk[SGXD_MK_FAULT_KIND]);
    }
    {
        // The 3D stream, and the counts the streams wait on each other by.
        const size_t used = strlen(failed);
        snprintf(failed + used, failed_size - used,
                 "; 3D stream: job %u of %u taken, resume at %u; TA jobs done %u, TA busy %u, "
                 "scenes binned %u, 3Ds kicked %u",
                 (unsigned)mk[SGXD_MK_POS3], (unsigned)mk[SGXD_MK_END3],
                 (unsigned)mk[SGXD_MK_RESUME3], (unsigned)mk[SGXD_MK_CHAIN_TA],
                 (unsigned)mk[SGXD_MK_TA_BUSY], (unsigned)mk[SGXD_MK_TA_SCENES],
                 (unsigned)mk[SGXD_MK_D3_KICKS]);
    }
    {
        // The job's entry and data slot, and the code space's generation.
        const uint32_t job = mk[SGXD_MK_POS] - 1u;
        const size_t used = strlen(failed);
        snprintf(failed + used, failed_size - used,
                 "; job %u's entry 0x%x, data 0x%x; code generation %u, %u flips", (unsigned)job,
                 (unsigned)sgxd_mkq_job_entry(job), (unsigned)mk[SGXD_MK_DATA],
                 (unsigned)sgxd_mkq_generation(), (unsigned)sgxd_mkq_flips_done());
    }
    // The memory-only part first: a register read below can abort in the kernel (a unit in
    // reset), which would lose the whole message.
    sceClibPrintf("[sgxd] microkernel stop, before the register reads: %s\n", failed);
    // What IRQ 52 took, and the event registers now: the host takes bank 0's SW_EVENT only.
    const volatile SgxdEvents *ev = sgxd_ptla_page();
    uint32_t master[2] = {0, 0}, bank0[2] = {0, 0}, slc[16], ptla[2], bif[2], mbif[2];
    sgxdReadRegs(0x412c, 2, master);
    sgxdReadRegs(0x012c, 2, bank0);
    sgxdReadRegs(0x4d00, 16, slc);
    sgxdReadRegs(0x4178, 2, ptla);   // MASTER_PTLA_STATUS (BUSY, FIFO fullness), REQUEST
    sgxdReadRegs(0x0c04, 2, bif);    // BIF_INT_STAT, BIF_FAULT
    sgxdReadRegs(0x4c04, 2, mbif);   // MASTER_BIF_INT_STAT, MASTER_BIF_FAULT
    // The DPM, for a TA out of pages (docs/sgxd/ta.md): the thresholds and the
    // global list (0x0620..0x063c, ZLS_PAGE_THRESHOLD at 0x0624), the lists' status,
    // ABORT_STATUS_MTILE, PAGE_STATUS, DPM_PAGE and GLOBAL_PAGE_STATUS (0x0700..0x072c).
    uint32_t dpm_thr[8], dpm_st[12];
    sgxdReadRegs(0x0620, 8, dpm_thr);
    sgxdReadRegs(0x0700, 12, dpm_st);
    size_t used = strlen(failed);
    snprintf(failed + used, failed_size - used,
             "; IRQ 52 %u (SW_EVENT %u, unexpected 0x%08x); master status 0x%08x host enable "
             "0x%08x, bank 0 status 0x%08x host enable 0x%08x; SLC 0x4d00..:",
             ev ? (unsigned)ev->irqs : 0u, ev ? (unsigned)ev->sw_events : 0u,
             ev ? (unsigned)ev->unexpected : 0u, (unsigned)master[0], (unsigned)master[1],
             (unsigned)bank0[0], (unsigned)bank0[1]);
    for (unsigned n = 0; n < 16; n++) {
        used = strlen(failed);
        snprintf(failed + used, failed_size - used, " %x", (unsigned)slc[n]);
    }
    used = strlen(failed);
    snprintf(failed + used, failed_size - used,
             "; PTLA status 0x%x request 0x%x; BIF int 0x%x fault 0x%x, master 0x%x 0x%x",
             (unsigned)ptla[0], (unsigned)ptla[1], (unsigned)bif[0], (unsigned)bif[1],
             (unsigned)mbif[0], (unsigned)mbif[1]);
    used = strlen(failed);
    snprintf(failed + used, failed_size - used, "; DPM 0x0620..:");
    for (unsigned n = 0; n < 8; n++) {
        used = strlen(failed);
        snprintf(failed + used, failed_size - used, " %x", (unsigned)dpm_thr[n]);
    }
    used = strlen(failed);
    snprintf(failed + used, failed_size - used, ", 0x0700..:");
    for (unsigned n = 0; n < 12; n++) {
        used = strlen(failed);
        snprintf(failed + used, failed_size - used, " %x", (unsigned)dpm_st[n]);
    }
    // Each core: its code bases 0 and 1, EVENT_STATUS2 and EVENT_STATUS, ISP_STATUS2,
    // USE0/USE1_DM_SLOT, the six USE<n>_SERV_PIXEL/VERTEX/EVENT queues and PDS_STATUS.
    for (unsigned c = 0; c < 4; c++) {
        const uint32_t bank = 0x8000u + c * 0x4000u;
        uint32_t st2 = 0, st = 0, isp = 0, slot[2] = {0, 0}, serv[6] = {0}, pds = 0;
        uint32_t cb[2] = {0, 0};
        sgxdReadRegs(bank + 0x0a0cu, 2, cb);
        sgxdReadRegs(bank + 0x0118u, 1, &st2);
        sgxdReadRegs(bank + 0x012cu, 1, &st);
        sgxdReadRegs(bank + 0x04e4u, 1, &isp);
        sgxdReadRegs(bank + 0x0aa4u, 2, slot);
        sgxdReadRegs(bank + 0x0b08u, 6, serv);
        sgxdReadRegs(bank + 0x0ba4u, 1, &pds);
        used = strlen(failed);
        snprintf(failed + used, failed_size - used,
                 "; core %u code bases %x %x st2 %x st %x isp2 %x slot %x %x serv %x %x %x %x %x "
                 "%x pds %x",
                 c, (unsigned)cb[0], (unsigned)cb[1], (unsigned)st2, (unsigned)st, (unsigned)isp,
                 (unsigned)slot[0], (unsigned)slot[1], (unsigned)serv[0], (unsigned)serv[1],
                 (unsigned)serv[2], (unsigned)serv[3], (unsigned)serv[4], (unsigned)serv[5],
                 (unsigned)pds);
    }
    static char check[1536];
    sgxd_mkq_check_render(check, sizeof(check));
    used = strlen(failed);
    snprintf(failed + used, failed_size - used, "; %s", check);
}
