// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// sgxd_mkq: the async queue, run by sgxd's microkernel on the GPU (vita_native/src/sgxd_mk.h,
// docs/sgxd/microkernel.md): each list is compiled once for its shape into code in CDRAM, and a
// send is a job, the code's entry and a data slot with the list's variable values, kicked
// (sgxdKick) only when the GPU had run out of jobs. Vita only.

#ifndef SGXD_MKQ_H
#define SGXD_MKQ_H

#include <stdint.h>

#include "sgxd.h"
#include "sgxd_mk.h"

#ifdef __cplusplus
extern "C" {
#endif

// The microkernel's GPU addresses from va: one 512 KiB CDRAM block, its event program and its code
// (code base 14) in the first 256 KiB, the state block, jobs and data slots (CACHECONSISTENT) in
// the second. 4 MiB-aligned GPU space the caller keeps free.
#define SGXD_MKQ_SPACE (4u << 20)
// The jobs a ring holds (a power of two); one more is the running one.
#define SGXD_MKQ_JOBS 256u

// The 3D stream's wait for its TA job, in SgxdMkqCode::value_op: a value no op of the list gives.
#define SGXD_MKQ_VALUE_JOB 0xffffu
// A list's code kept by its sender (sgxd_session's kept lists), so a send skips the shape
// lookup and reads only the ops in value_op: value_op[s][k] is op n << 1 for its value, n << 1 | 1
// for a STORE's address (its mask), or SGXD_MKQ_VALUE_JOB. Zeroed: none yet.
typedef struct SgxdMkqCode {
    uint32_t entry, generation;
    uint8_t values[2];
    uint16_t value_op[2][SGXD_MK_DATA_WORDS];
} SgxdMkqCode;

// Allocates and maps the microkernel's memory (once a process), assembles the runtime and its
// event program, points core 0's event program and code base 14 at them, and starts the state
// block's counts at 0. The GPU must be idle. Returns 0, or -1 with failed filled.
int sgxd_mkq_start(uint32_t va, char *failed, unsigned failed_size);
// The GPU idle: USE_SPRV and code base 15 back. The event program stays the microkernel's until a
// recipe's kick points it elsewhere.
void sgxd_mkq_stop(void);
// The state block (SGXD_MK_*), or NULL before the start.
volatile uint32_t *sgxd_mkq_state(void);
// A list as a job: its code (code's if current, else the shape's, compiled when new), its
// variable values into the job's data slot, then END; with BUSY clear, BUSY set and a kick.
// variable as sgxd_mk_compile's; code may be NULL. A full ring spins until the GPU has made room
// (a second at most). Returns 0 or a negative error (AZAHAR_ERR_STATE after a fault or when a
// flip found the GPU not idle within a second, which the console names; the compiler's
// SGX_USSE_E* for a list it refused, at sgxd_mkq_refused).
int sgxd_mkq_submit(const SgxdOp *ops, unsigned count, const uint8_t *variable, SgxdMkqCode *code);
// The jobs sent (the END word, free-running).
uint32_t sgxd_mkq_jobs_sent(void);
// Job n's entry (its list's first instruction), as the ring holds it now; the code space's
// generation (one more at each flip) and the flips done: a stop's post-mortem.
uint32_t sgxd_mkq_job_entry(uint32_t job);
uint32_t sgxd_mkq_generation(void);
uint32_t sgxd_mkq_flips_done(void);
// Spins until every job sent has been taken and the GPU has run out of jobs (BUSY clear): no run
// is left but one finishing its last few instructions. 0, or -1 on a fault or after a second.
int sgxd_mkq_idle(void);
// Shapes compiled, their instructions and flips (the code space full: the GPU idled, the USE
// code cache invalidated, every shape dropped): perflog's and sgxd_test's, diagnostics builds
// only. And the last refusal: the op's index and kind.
#ifdef VITA_DIAGNOSTICS
extern uint32_t sgxd_mkq_compiles, sgxd_mkq_instructions, sgxd_mkq_flips;
#endif
#ifdef VITA_DIAGNOSTICS
// Where the GPU stands, for perflog's sampler: the kind of the job the next sgxd_mkq_submit sends
// (sgxd_mkq_kind_next, set by its sender, back to OTHER after the send), a job's kind, and the
// kind of the wait a run stopped at, by the index it left in SGXD_MK_RESUME.
enum {
    SGXD_MKQ_KIND_OTHER,       // a remap, a reset, a synchronous caller's
    SGXD_MKQ_KIND_RENDER,      // a render list with no 3D pending before it
    SGXD_MKQ_KIND_RENDER_OVER, // a render list whose TA runs over the pending 3D
    SGXD_MKQ_KIND_FINISH,      // the pending 3D's wait and done alone (sgxd_session_finish_pending)
    SGXD_MKQ_KIND_PTLA,        // a PTLA fill or copy
    SGXD_MKQ_KINDS,
};
enum {
    SGXD_MKQ_WAIT_NONE,  // the run is in the list's code (or between runs)
    SGXD_MKQ_WAIT_TA,    // TA_FINISHED
    SGXD_MKQ_WAIT_3D,    // PIXELBE_END_RENDER
    SGXD_MKQ_WAIT_TWOD,  // a PTLA count
    SGXD_MKQ_WAIT_EVENT, // another event
    SGXD_MKQ_WAITS,
};
extern uint8_t sgxd_mkq_kind_next;
uint8_t sgxd_mkq_job_kind(uint32_t job);
uint8_t sgxd_mkq_wait_kind(uint32_t resume);
// Who sent a render job, for perflog's sampler: a label (1 and on; 0 none) names a sender (a
// static string) and a key of its (a target's address, a call site). sgxd_mkq_label makes one
// on first use (0 once all are taken); the next render job sent takes sgxd_mkq_label_next (back
// to 0 after it); sgxd_mkq_label_sent counts the render jobs sent under each.
enum { SGXD_MKQ_LABELS = 64 };
extern uint8_t sgxd_mkq_label_next;
extern uint32_t sgxd_mkq_label_sent[SGXD_MKQ_LABELS];
uint8_t sgxd_mkq_label(const char *what, uint32_t key);
const char *sgxd_mkq_label_what(uint8_t label, uint32_t *key);
uint8_t sgxd_mkq_job_label(uint32_t job);
#endif
// A stop's check: the last render list taken, each state register it writes read back against the
// value it wrote (its constant or its job's data slot), the ones that differ listed as
// reg:wanted/read (registers a list's write did not reach: stale code bases).
void sgxd_mkq_check_render(char *out, unsigned size);
extern uint32_t sgxd_mkq_refused, sgxd_mkq_refused_kind;
// The code (the runtime, then the lists) and its instructions in use, and the state block with
// the jobs and data slots after it, for a fault's post-mortem.
const volatile uint64_t *sgxd_mkq_code(uint32_t *instructions);
const volatile uint32_t *sgxd_mkq_memory(uint32_t *words);

#ifdef __cplusplus
}
#endif

#endif
