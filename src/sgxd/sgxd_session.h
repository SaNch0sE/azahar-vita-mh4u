// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// sgxd_session: what every owner of the GPU through sgxd runs around the recipe: the init
// script after the take, the SLC's flush and invalidate, and a render's phases, each one
// sgxdSubmit, or the whole render one async list for sgxd's microkernel (docs/sgxd/README.md).
// Vita only. Every function returns 0, or -1 with the op that stopped the list in `failed`.

#ifndef SGXD_SESSION_H
#define SGXD_SESSION_H

#include <stdint.h>

#include "sgxd.h"
#include "sgxd_recipe.h"

#ifdef __cplusplus
extern "C" {
#endif

// A poll's cap in a render's wait phases.
#define SGXD_SESSION_POLL_US 1000000u

// The GPU addresses of the init script (below): the sixteen code bases, the kicker word, and
// bank 0's and core 0's event programs.
typedef struct SgxdSessionAddresses {
    uint32_t code[16];
    uint32_t kicker;
    uint32_t bank0_pds, bank0_units, core0_pds, core0_units;
} SgxdSessionAddresses;

// The recipe's: code base 1 at CF (the fragment programs), the others at CV, and IDLE as both
// event programs. kicker_va is a word the caller keeps at GPU_KICKER_VALUE's 0x5a.
void sgxd_session_recipe_addresses(const SgxdRecipe *r, uint32_t kicker_va,
                                   SgxdSessionAddresses *a);

// SceGpu's init script after the start sequence (the register writes of Sony's resume), less the
// BIF writes sgxd owns, with the caller's addresses. Once after each take.
int sgxd_session_init_script(const SgxdSessionAddresses *a, char *failed, unsigned failed_size);

// MASTER_SLC_CTRL_FLUSH, then with inval MASTER_SLC_CTRL_INVAL, each acknowledged (recipe T0.1,
// T0.2). An invalidate only while the GPU is idle: it drops lines a running program dirtied.
int sgxd_session_slc(int inval, char *failed, unsigned failed_size);

// The SLC invalidate, then the init script: the take's bring-up for a recipe.
// sgxd_session_init again after a suspend, the GPU taken again: the same lists, sent without
// waiting for the renderer's drain first (the GPU is idle, and the drain is the render thread's).
int sgxd_session_resume(const SgxdRecipe *r, uint32_t kicker_va, char *failed,
                        unsigned failed_size);
// The session whose init script sgxd_session_reinit runs again (the take's): after a reset of
// every unit (sgxdResetAll), as Sony's driver runs its init script after every start.
void sgxd_session_set_reinit(const SgxdRecipe *r, uint32_t kicker_va);
int sgxd_session_reinit(char *failed, unsigned failed_size);
int sgxd_session_init(const SgxdRecipe *r, uint32_t kicker_va, char *failed,
                      unsigned failed_size);

// One of sgxd_recipe's op-list builders through sgxdSubmit, named `what` in `failed`.
typedef void (*SgxdSessionBuild)(const SgxdRecipe *r, SgxdOpList *list);
int sgxd_session_phase(const SgxdRecipe *r, SgxdSessionBuild build, const char *what,
                       char *failed, unsigned failed_size);
// The wait and SLC phases as builders, with SGXD_SESSION_POLL_US.
void sgxd_session_ta_wait(const SgxdRecipe *r, SgxdOpList *list);
void sgxd_session_3d_wait(const SgxdRecipe *r, SgxdOpList *list);
void sgxd_session_slc_inval(const SgxdRecipe *r, SgxdOpList *list);

typedef struct SgxdSessionRender {
    int read_pages;       // in: read PAGE_STATUS (0x0724) after the TA
    uint32_t ta_us;       // out: the TA's wait, kick to TA_FINISHED
    uint32_t d3_us;       // out: the 3D's wait
    uint32_t pages;       // out, with read_pages: PAGE_STATUS's high half, the pass's pages
} SgxdSessionRender;

// One render of the recipe as it is staged: the TA kick, wait and done, then the 3D's. The 3D's
// done phase flushes the SLC, so the frame is in memory when it returns (the display reads
// memory, not the SLC).
// Renders sgxd_session_render has completed since the app started.
uint32_t sgxd_session_renders(void);

// sgxd_session_render as one async list (sgxd_chain_submit, needs sgxd_ptla_events): the TA kick
// with Sony's SLC requests, then, run by the microkernel at the TA's end, the TA's done phase, the
// 3D kick and done. It is queued behind the lists and PTLA ops sent so far, and returns at once. The
// list's fence is sgxd_chain_fence() afterwards: sgxd_chain_ta_reached(fence) when the TA has
// read the vertices, sgxd_chain_reached(fence) when the frame is in memory.
int sgxd_session_render_async(const SgxdRecipe *r, int read_pages, char *failed,
                              unsigned failed_size);
// That list, appended to l (overflow set when it does not fit): the WAIT_TWOD for ptla_fence
// first, the SIGNAL_DONE last. Built with sgxd_ptla_events registered (its kicks' SLC form).
void sgxd_session_render_list(const SgxdRecipe *r, int read_pages, uint32_t ptla_fence,
                              SgxdOpList *l);
// 1 (default): the TA runs over a 3D in flight, as Sony's microkernel does. It needs the one
// shared parameter buffer (sgxd_recipe_pb): with a buffer per recipe, the TA's free-list load
// over another buffer's 3D stops the GPU.
extern int sgxd_session_ta_over_3d;

// sgxd_session_render_async as a kept list (values rewritten per render, sgxd_session.c) for a
// recipe on the shared parameter buffer. Its 3D stream's part waits for its own end, so no 3D is
// left pending; `touches` is unused.
int sgxd_session_render_overlap(const SgxdRecipe *r, const uint32_t touches[6], char *failed,
                                unsigned failed_size);
// A recipe is no more (its block given back, a backend's end; NULL: every recipe): the lists kept
// for it go, so a recipe made later at its address builds its own.
void sgxd_session_forget(const SgxdRecipe *r);
#ifdef VITA_DIAGNOSTICS
// The overlapped lists sent as kept (a list kept for the recipe and key, values rewritten) and
// as built (perflog; diagnostics builds only).
extern uint32_t sgxd_session_kept_sent, sgxd_session_kept_built;
// Always 0: kept for perflog.
extern uint32_t sgxd_session_finishes;
#endif
// No 3D is ever left pending: these keep their callers, with nothing to do.
const SgxdRecipe *sgxd_session_pending(void);
int sgxd_session_pending_touches(uint32_t lo, uint32_t hi);
void sgxd_session_pending_fence(void);
int sgxd_session_pending_fenced(void);
int sgxd_session_finish_pending(char *failed, unsigned failed_size);
// After an overlapped list ended: no times (the microkernel takes none; sgxd_flags syncpasses
// times passes synchronously) and no pages.
void sgxd_session_render_overlap_result(SgxdSessionRender *render);
// After the last async render completed: its pages (read_pages), no times.
void sgxd_session_render_async_result(SgxdSessionRender *render);
// After a chain wait failed: where the microkernel stands, the event registers, each core's
// state and the last render list's registers against its values.
void sgxd_session_render_async_failed(char *failed, unsigned failed_size);

#if defined(VITA_DEBUG_TOOLS) && !defined(VITA_DIAGNOSTICS)
// Tools builds: nonzero while the slow-frame recorder times its frame (SgxdBackend::tools_timing):
// sgxd_session_render then fills ta_us and d3_us as the diagnostics build does.
extern int sgxd_session_time_renders;
#endif
int sgxd_session_render(const SgxdRecipe *r, SgxdSessionRender *render, char *failed,
                        unsigned failed_size);

#ifdef __cplusplus
}
#endif

#endif
