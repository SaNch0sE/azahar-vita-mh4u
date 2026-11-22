// sgxd_session — what every owner of the GPU through sgxd runs around the recipe: the init
// script after the take, the SLC's flush and invalidate, and a render's phases (the TA, then the
// 3D), each phase one sgxdSubmit, or the whole render one sgxdSubmitAsync list.
// sgxd_test, the frontend and the rasterizer share it.
//
// Vita only: it calls sgxdSubmit and the kernel's clock. Every function returns 0, or -1 with
// the op that stopped the list described in `failed` (the caller logs it).

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

// sgxd_session_render as one async list (sgxd_chain_submit, needs sgxd_ptla_events): the SLC
// invalidate, the TA kick, then from IRQ 52 the TA's done phase, the 3D kick and done. It is
// queued in the kernel behind the lists and PTLA ops sent so far, and returns at once. The
// list's fence is sgxd_chain_fence() afterwards: sgxd_chain_ta_reached(fence) when the TA has
// read the vertices, sgxd_chain_reached(fence) when the frame is in memory.
int sgxd_session_render_async(const SgxdRecipe *r, int read_pages, char *failed,
                              unsigned failed_size);
// After the last async render completed: its TA and 3D times (the ISR's stamps) and pages.
void sgxd_session_render_async_result(SgxdSessionRender *render);
// After a chain wait failed: stops the queue and describes the op it stopped at.
void sgxd_session_render_async_failed(char *failed, unsigned failed_size);

int sgxd_session_render(const SgxdRecipe *r, SgxdSessionRender *render, char *failed,
                        unsigned failed_size);

#ifdef __cplusplus
}
#endif

#endif
