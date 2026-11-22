// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// azahar-native sgxd_mk.h: sgxd's microkernel, the async queue's lists compiled into USSE code
// and run on the GPU (docs/sgxd/microkernel.md).
//
// Core 0's event program starts the runtime for every event (KICK2 0x10000, TA end 0x10, 3D end
// 0x8, PTLA 0x1000; the timer stays off). One run at a time (lock #7). A run finishes a PTLA
// completion, then goes on where the last run stopped (RESUME) or takes the next job; with none
// left it clears BUSY, reads END again, and ends if it has not moved. A wait not met stores its
// own index in RESUME and ends the run. A POLL that runs out stores 1 + its index in FAULT and
// raises SW_EVENT; no job runs after a fault.
//
// The state block, the jobs and the data slots are mapped uncached for the CPU and
// CACHECONSISTENT for the GPU: no flush between the two sides. Freestanding C, built into
// azahar and sgxd_test.
#ifndef SGXD_MK_H
#define SGXD_MK_H

#include <stdint.h>

#include "sgxd.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SGXD_MK_END,          // host: one past the last job published (free-running)
    SGXD_MK_POS,          // GPU: the next job
    SGXD_MK_BUSY,         // host sets it before its kick; GPU clears it, out of jobs
    SGXD_MK_CHAINS,       // GPU: SGXD_SIGNAL_DONE ops run
    SGXD_MK_CHAIN_TA,     // GPU: SGXD_SIGNAL_TA ops run
    SGXD_MK_TWOD,         // GPU: PTLA completions finished
    SGXD_MK_FAULT,        // GPU: 0, or 1 + the op the list stopped at (the job POS - 1's)
    SGXD_MK_FAULT_KIND,   // GPU: that op's kind
    SGXD_MK_VALUE,        // GPU: the last READ, the stopped poll's last read, or the count a
                          // WAIT_TWOD that was not met waits for
    SGXD_MK_WAKE_ARMED,   // host sets, GPU clears
    SGXD_MK_WAKE_CHAINS,  // host
    SGXD_MK_WAKE_TWOD,    // host
    SGXD_MK_WAKES,        // GPU
    SGXD_MK_RESUME,       // GPU: the instruction index a run goes on at, 0 for none
    SGXD_MK_DATA,         // GPU: the running job's data slot
    SGXD_MK_SV_COUNT,     // GPU: the supervised writes the next call makes (SGXD_MK_SV_WORD)
    SGXD_MK_SV_LINK,      // GPU: the supervised call's link and p0..p3, kept across the call
    // The 3D stream (the TA stream's are END, POS and RESUME above): its jobs published, its next
    // job, where its run goes on; each stream's running job's data slot (DATA is the one running
    // now, which a supervised call's return reads).
    SGXD_MK_END3,
    SGXD_MK_POS3,
    SGXD_MK_RESUME3,
    SGXD_MK_DATA_TA,
    SGXD_MK_DATA3,
    // GPU: a TA kicked and its TA side not done; the scenes binned (their TA side done) and the
    // 3Ds kicked: TA_SCENES - D3_KICKS scenes wait for their 3D.
    SGXD_MK_TA_BUSY,
    SGXD_MK_TA_SCENES,
    SGXD_MK_D3_KICKS,
    // GPU: whether the scheduler's round got anywhere (a job taken, a wait met).
    SGXD_MK_PROGRESS,
    // Unused (always 0). The kernel module's suspend wait compares the two run counts.
    SGXD_MK_TRACE_AT,
    SGXD_MK_RUNS,
    SGXD_MK_RUNS_ENDED,
    SGXD_MK_STATE_WORDS,
};

// A POLL's reads per microsecond of its timeout: an ldr and its WDF take well under one, so the
// wait lasts at most about as long as asked.
#define SGXD_MK_POLL_SPINS 1u
// A job's data slot: its list's variable values, at most this many.
#define SGXD_MK_DATA_WORDS 64u
// The supervised writes of one call, {register index, value} pairs from state word SGXD_MK_SV_WORD
// (after the state words, before the jobs): a run of a list's consecutive supervised writes is
// one call (one phase switch), a bank-0 code base four pairs.
#define SGXD_MK_SV_WORD 64u
#define SGXD_MK_SV_PAIRS 16u
// The registers an event task writes only in supervisor mode: SOFT_RESET and the USE code
// bases, in any bank. An event task's own store of a code base takes no effect, and its
// SOFT_RESET reaches cores 0 and 1 only (docs/sgxd/pds.md). sgxd_mk_compile writes them
// through the runtime's supervisor vector, a bank-0 code base as each core's (Sony's form).
static inline int sgxd_mk_supervised(uint32_t reg) {
    const uint32_t r = reg & 0x3fffu;
    return r == 0x0080u || (r >= 0x0a0cu && r <= 0x0a48u);
}
// The code's temporaries, r0..r31, in the event program's DOUTU (units of four). More would not
// help: a KICK2 event task's r36 and up hold nothing.
#define SGXD_MK_TEMP_UNITS 8u

// Two streams of jobs, each a ring: the TA stream's (jobs_va, data_va) and the 3D stream's
// (jobs3_va, data3_va). A run is the scheduler's rounds: the 3D stream, then the TA stream, until
// a round gets nowhere. A list's end starts the round again, so a ready 3D goes before the next TA.
typedef struct SgxdMkLayout {
    uint32_t state_va;   // the state block, SGXD_MK_STATE_WORDS words
    uint32_t jobs_va;    // jobs words: each job's list, its first instruction's index
    uint32_t jobs;       // a power of two
    uint32_t data_va;    // jobs data slots of SGXD_MK_DATA_WORDS words, job n's at slot n % jobs
    uint32_t jobs3_va;   // the 3D stream's jobs words and data slots, as jobs_va and data_va
    uint32_t data3_va;
} SgxdMkLayout;

// The runtime's entry points (code base window indices).
typedef struct SgxdMkRuntime {
    uint32_t start;  // core 0's event program's DOUTU
    uint32_t next;   // a list's end (its RESUME 0): the round again
    uint32_t exit;   // a run's end, its stores fenced
    uint32_t wake;   // the wake check, a call
    uint32_t supervisor;  // the USE_SPRV vector: a register write in supervisor mode
    uint32_t supervised;  // a call: SV_COUNT supervised writes from SGXD_MK_SV_WORD, through the vector
    uint32_t blocked[2];  // a wait not met (its stream's RESUME stored): the TA stream's, the 3D's
} SgxdMkRuntime;

#define SGXD_MK_STREAM_TA 0u
#define SGXD_MK_STREAM_3D 1u

// Assembles the runtime at code base window index `base`. Returns its length in instructions, or
// a negative SGX_USSE_E*.
int sgxd_mk_runtime(uint64_t *code, uint32_t cap, uint32_t base, const SgxdMkLayout *layout,
                    SgxdMkRuntime *rt);

// Diagnostics: called for each wait sgxd_mk_compile places, with the index a run stores in RESUME
// when the wait is not met and the wait's op. NULL (the default): none.
extern void (*sgxd_mk_wait_placed)(uint32_t at, const SgxdOp *op);

// Compiles a list at index `base` of the runtime's window (its entry is `base`), ending in a jump
// to rt->next. variable[n] nonzero (variable may be NULL) takes op n's value from the job's data
// slot: WRITEs only; every WAIT_TWOD's value comes from there too. The data slot holds those
// values in the list's order (sgxd_mk_values). Returns the code's length in instructions, or a
// negative SGX_USSE_E*; SGX_USSE_EINVAL for an op the code cannot take (a DELAY, a write
// writable refuses, more than SGXD_MK_DATA_WORDS values), at *refused (its index).
int sgxd_mk_compile(uint64_t *code, uint32_t cap, uint32_t base, const SgxdMkLayout *layout,
                    const SgxdMkRuntime *rt, const SgxdOp *ops, uint32_t nops,
                    const uint8_t *variable, int (*writable)(uint32_t reg), uint32_t *refused);
// The same for a stream (SGXD_MK_STREAM_*): its waits store its RESUME and go to its blocked
// entry. sgxd_mk_compile is the TA stream's.
int sgxd_mk_compile_stream(uint64_t *code, uint32_t cap, uint32_t base, const SgxdMkLayout *layout,
                           const SgxdMkRuntime *rt, const SgxdOp *ops, uint32_t nops,
                           const uint8_t *variable, int (*writable)(uint32_t reg),
                           uint32_t *refused, uint32_t stream);

// Whether op n of a list takes its value from the data slot (sgxd_mk_compile's rule): a
// WAIT_TWOD's and a STORE's always, a WRITE's when marked.
static inline int sgxd_mk_is_variable(const SgxdOp *op, const uint8_t *variable, uint32_t n) {
    return op->op == SGXD_OP_WAIT_TWOD || op->op == SGXD_OP_WAIT_COUNT ||
           op->op == SGXD_OP_STORE || (op->op == SGXD_OP_WRITE && variable && variable[n]);
}

// The data slot's words a variable op takes: a STORE its address, then its value.
static inline uint32_t sgxd_mk_var_words(const SgxdOp *op) {
    return op->op == SGXD_OP_STORE ? 2u : 1u;
}

// The list's data slot: its variable values in order. Returns how many words (at most
// SGXD_MK_DATA_WORDS are written; more is a list sgxd_mk_compile refused).
static inline uint32_t sgxd_mk_values(volatile uint32_t *slot, const SgxdOp *ops, uint32_t nops,
                                      const uint8_t *variable) {
    uint32_t k = 0;
    for (uint32_t n = 0; n < nops; n++) {
        if (sgxd_mk_is_variable(&ops[n], variable, n)) {
            if (ops[n].op == SGXD_OP_STORE) {
                if (k < SGXD_MK_DATA_WORDS) {
                    slot[k] = ops[n].mask;
                }
                k++;
            }
            if (k < SGXD_MK_DATA_WORDS) {
                slot[k] = ops[n].value;
            }
            k++;
        }
    }
    return k;
}

#ifdef __cplusplus
}
#endif

#endif
