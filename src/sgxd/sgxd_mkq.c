// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// sgxd_mkq: the async queue run by sgxd's microkernel (sgxd_mkq.h).

#include "sgxd_mkq.h"
#include "sgxd_pds.h"
#include "sgxd_ptla.h"
#include "sgxd_usse.h"

#include <stdio.h>
#include <string.h>

#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>

#define W(r, v) {SGXD_OP_WRITE, (r), (v), 0, 0}

// One CDRAM block: the code, then the state. A full code space flips: the GPU idles and every
// shape is compiled again. 1.25 MiB holds about 150 render lists of ~1060 instructions.
#define CODE_BLOCK (1280u << 10)
#define MK_BLOCK (CODE_BLOCK + STATE_BLOCK)
// The code: core 0's event program in its first page (mapped plain, as sgxd_test's are), the
// code after it (READONLY), code base 14.
#define CODE_OFFSET 0x1000u
#define CODE_CAP ((CODE_BLOCK - CODE_OFFSET) / 8u)
#define CODE_BASE_REG (0x0a0cu + 14u * 4u)
// The state: the state words, the jobs, the data slots; CACHECONSISTENT.
#define STATE_BLOCK (256u << 10)
#define JOBS_OFFSET 0x100u
#define DATA_OFFSET 0x1000u
#define USE_CACHE_INVAL 0x0a08u
#define SLC_EVENT_STATUS 0x4d10u
#define SLC_EVENT_CLEAR 0x4d14u
#define SLC_CTRL_INVAL 0x4d28u
#define SLC_CTRL_FLUSH 0x4d2cu
#define P(r, m, v, us) {SGXD_OP_POLL, (r), (v), (m), (us)}
#define CORE0_PDS 0x8a68u
#define CORE0_PDS_UNITS 0x8a6cu
// Core 0's EVENT_TIMER stays off (Sony's runs a 1 ms tick): runs start only on a KICK2 or a
// completion, so a lost event stops the queue visibly rather than costing a millisecond.
#define CORE0_EVENT_TIMER 0x8accu
// Core 0's USE_SPRV, the supervisor vector (the runtime's, rt.supervisor), and code base 15,
// which the vector's index is taken in: the
// microkernel's code base while it runs, both put back at the stop.
#define CORE0_USE_SPRV 0x8ba0u
#define CODE_BASE_15 0x0a48u

// DOUTA of IR0, IR1 and data rows into pa0-pa2, then DOUTU with code base 14, the runtime's
// start and its temporaries.
#define PDS_DOUTA_WORDS 7u
static void pds_douta_code(uint32_t code[PDS_DOUTA_WORDS]) {
    code[0] = movs(6, 0, 0, 1, 0, 2, 0, 0, 0); // IR0
    code[1] = movs(6, 0, 0, 1, 1, 2, 1, 0, 0); // IR1
    for (unsigned n = 2; n < 5; n++) {
        code[n] = movs(6, 0, 1, 0, 0, S1L, S1H, S1L, S1L);
    }
    code[5] = movs(5, 0, 2, 0, 0, S1L, S1H, S2L, S1L);
    code[6] = HALT;
}
#define PDS_DOUTA_UNITS 3u

// The shapes compiled: a table by hash, each with its ops (values the job carries zeroed) and
// variable marks in the pool, to compare exactly.
#define SHAPES 2048u
#define POOL_OPS 65536u
typedef struct Shape {
    uint64_t hash;
    uint32_t count, at, entry;
} Shape;

static SceUID code_uid = -1;
static volatile uint64_t *code_mem;   // the code's first instruction (window index 0)
static volatile uint32_t *state, *jobs, *data;
static uint32_t code_va, state_va;
static SgxdMkLayout layout;
static SgxdMkRuntime rt;
static uint32_t runtime_len, code_used;
static uint32_t job_end;
static uint32_t sprv_before, base15_before;
static uint32_t generation = 1, flips_done;
static Shape shapes[SHAPES];
static uint32_t nshapes;
static SgxdOp pool[POOL_OPS];
static uint8_t pool_var[POOL_OPS];
static uint32_t pool_used;
// sgxd_mk_compile writes here (patches read back), then the words go to CDRAM in one copy.
static uint64_t scratch[16384];

#ifdef VITA_DIAGNOSTICS
uint32_t sgxd_mkq_compiles, sgxd_mkq_instructions, sgxd_mkq_flips;
uint8_t sgxd_mkq_kind_next;
// Each job's kind, at its slot, and each placed wait's kind, at the index a run stores in RESUME
// when it stops there (the code's window index; a flip places waits anew over the old ones).
static uint8_t job_kind[SGXD_MKQ_JOBS];
static uint8_t wait_kind[CODE_CAP];

static void wait_placed(uint32_t at, const SgxdOp *op) {
    if (at < CODE_CAP) {
        wait_kind[at] = op->op == SGXD_OP_WAIT_TWOD ? SGXD_MKQ_WAIT_TWOD
                        : op->mask == 0x00002000u   ? SGXD_MKQ_WAIT_TA
                        : op->mask == 0x00040000u   ? SGXD_MKQ_WAIT_3D
                                                    : SGXD_MKQ_WAIT_EVENT;
    }
}

uint8_t sgxd_mkq_job_kind(uint32_t job) {
    return job_kind[job & (SGXD_MKQ_JOBS - 1u)];
}

uint8_t sgxd_mkq_wait_kind(uint32_t resume) {
    return resume < CODE_CAP ? wait_kind[resume] : SGXD_MKQ_WAIT_EVENT;
}

uint8_t sgxd_mkq_label_next;
uint32_t sgxd_mkq_label_sent[SGXD_MKQ_LABELS];
static uint8_t job_label[SGXD_MKQ_JOBS];
static const char *label_what[SGXD_MKQ_LABELS];
static uint32_t label_key[SGXD_MKQ_LABELS];
static unsigned labels = 1;

uint8_t sgxd_mkq_label(const char *what, uint32_t key) {
    for (unsigned n = 1; n < labels; n++) {
        if (label_what[n] == what && label_key[n] == key) {
            return (uint8_t)n;
        }
    }
    if (labels == SGXD_MKQ_LABELS) {
        return 0;
    }
    label_what[labels] = what;
    label_key[labels] = key;
    // Published after its fields: perflog reads them from another thread.
    __atomic_store_n(&labels, labels + 1, __ATOMIC_RELEASE);
    return (uint8_t)(labels - 1);
}

const char *sgxd_mkq_label_what(uint8_t label, uint32_t *key) {
    if (label == 0 || label >= __atomic_load_n(&labels, __ATOMIC_ACQUIRE)) {
        *key = 0;
        return "other";
    }
    *key = label_key[label];
    return label_what[label];
}

uint8_t sgxd_mkq_job_label(uint32_t job) {
    return job_label[job & (SGXD_MKQ_JOBS - 1u)];
}
#endif
uint32_t sgxd_mkq_refused, sgxd_mkq_refused_kind;

// What a list may write: sgxd's rule (the BIF directory, bank and MMU-invalidate registers are
// the kernel's), less what the microkernel owns in every bank: the event enables and host
// enable, code bases 14 and 15, the event program, the kicks and the kicker. A full SOFT_RESET
// does not reach cores 2 and 3 from core 0's USSE (docs/sgxd/pds.md), so the PDS arena's flip
// sends its own synchronously.
static const uint16_t owned[] = {0x0110, 0x0130, 0x0a44, 0x0a48, 0x0a58, 0x0a68,
                                 0x0a6c, 0x0a70, 0x0ac0, 0x0ac4, 0x0ac8};

static int writable(uint32_t reg) {
    const uint32_t r = reg & 0x3fffu;
    // The MMU invalidate after a remap's STORE (sgxd_remap_submit) is a list's to write; the BIF's
    // set-up and directories stay the kernel's.
    if ((reg & 3u) || reg >= 0x18000u || r == 0x0c00u || (r > 0x0c34u && r <= 0x0c84u)) {
        return 0;
    }
    for (unsigned n = 0; n < sizeof(owned) / sizeof(owned[0]); n++) {
        if (r == owned[n]) {
            return 0;
        }
    }
    return 1;
}

static int submit_sync(SgxdOp *ops, unsigned count, const char *what, char *failed,
                       unsigned failed_size) {
    const int done = sgxdSubmit(ops, count);
    if (done != (int)count) {
        snprintf(failed, failed_size, "microkernel %s: op %d of %u", what, done, count);
        return -1;
    }
    return 0;
}

static int map_block(const char *name, uint32_t size, SceUID *uid, void **base, char *failed,
                     unsigned failed_size) {
    *uid = sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, NULL);
    const int got = *uid >= 0 ? sceKernelGetMemBlockBase(*uid, base) : *uid;
    if (got < 0) {
        snprintf(failed, failed_size, "microkernel: no %u KiB of CDRAM for %s (0x%08x)",
                 size >> 10, name, (unsigned)got);
        return -1;
    }
    return 0;
}

int sgxd_mkq_start(uint32_t va, char *failed, unsigned failed_size) {
#ifdef VITA_DIAGNOSTICS
    sgxd_mk_wait_placed = wait_placed;
#endif
    if (code_uid < 0) {
        void *code_base = NULL;
        if (map_block("SgxdMk", MK_BLOCK, &code_uid, &code_base, failed, failed_size) < 0) {
            return -1;
        }
        void *state_base = (uint8_t *)code_base + CODE_BLOCK;
        code_va = va;
        state_va = va + CODE_BLOCK;
        const int mapped = sgxdMap(code_va, code_base, CODE_OFFSET, 0) |
                           sgxdMap(code_va + CODE_OFFSET, (uint8_t *)code_base + CODE_OFFSET,
                                   CODE_BLOCK - CODE_OFFSET, SGXD_MAP_READONLY) |
                           sgxdMap(state_va, state_base, STATE_BLOCK, SGXD_MAP_CACHECONSISTENT);
        if (mapped != 0) {
            snprintf(failed, failed_size, "microkernel: not mapped at GPU 0x%08x (0x%08x)",
                     (unsigned)va, (unsigned)mapped);
            return -1;
        }
        code_mem = (volatile uint64_t *)((uint8_t *)code_base + CODE_OFFSET);
        state = (volatile uint32_t *)state_base;
        jobs = (volatile uint32_t *)((uint8_t *)state_base + JOBS_OFFSET);
        data = (volatile uint32_t *)((uint8_t *)state_base + DATA_OFFSET);
        layout = (SgxdMkLayout){state_va, state_va + JOBS_OFFSET, SGXD_MKQ_JOBS,
                                state_va + DATA_OFFSET, mk_flags};
        // The runtime at window index 0, written once: code is never rewritten at an address
        // that has run (docs/sgxd/pds.md).
        const int len = sgxd_mk_runtime(scratch, sizeof(scratch) / 8u, 0, &layout, &rt);
        if (len <= 0) {
            snprintf(failed, failed_size, "microkernel: the runtime did not assemble (%d)", len);
            return -1;
        }
        for (int n = 0; n < len; n++) {
            code_mem[n] = scratch[n];
        }
        runtime_len = code_used = (uint32_t)len;
        const uint32_t pds_data[12] = {0, 0x100, 0, 0, 0, 0x200, 0, 0,
                                       14u | rt.start << 4 | SGXD_MK_TEMP_UNITS << 24, 1, 0, 0};
        memcpy(code_base, pds_data, sizeof(pds_data));
        uint32_t pds_code[PDS_DOUTA_WORDS];
        pds_douta_code(pds_code);
        memcpy((uint8_t *)code_base + PDS_DOUTA_UNITS * 16u, pds_code, sizeof(pds_code));
    } else if (va != code_va) {
        snprintf(failed, failed_size, "microkernel: started at 0x%08x before, not 0x%08x",
                 (unsigned)code_va, (unsigned)va);
        return -1;
    }
    for (unsigned n = 0; n < SGXD_MK_STATE_WORDS; n++) {
        state[n] = 0;
    }
    job_end = 0;
    if (sgxdReadRegs(CORE0_USE_SPRV, 1, &sprv_before) < 0 ||
        sgxdReadRegs(CODE_BASE_15, 1, &base15_before) < 0) {
        snprintf(failed, failed_size, "microkernel: USE_SPRV or code base 15 not read");
        return -1;
    }
    __asm__ volatile("dmb" ::: "memory");
    // The new code past the USE's code cache, then core 0's event program on it.
    SgxdOp ops[] = {W(CODE_BASE_REG, (code_va + CODE_OFFSET) >> 6), W(USE_CACHE_INVAL, 1),
                    W(CORE0_PDS, code_va), W(CORE0_PDS_UNITS, PDS_DOUTA_UNITS),
                    W(CORE0_EVENT_TIMER, 0),
                    W(CODE_BASE_15, (code_va + CODE_OFFSET) >> 6),
                    W(CORE0_USE_SPRV, rt.supervisor)};
    return submit_sync(ops, sizeof(ops) / sizeof(ops[0]), "event program", failed, failed_size);
}

void sgxd_mkq_stop(void) {
    if (!state) {
        return;
    }
    SgxdOp ops[] = {W(CORE0_USE_SPRV, sprv_before), W(CODE_BASE_15, base15_before)};
    sgxdSubmit(ops, sizeof(ops) / sizeof(ops[0]));
}

// A state register a render list writes and the GPU keeps (TE/MTE, ISP/PBE, the region and
// pixel-event set-up, code bases, requestor bases): one a stop's check compares.
static int kept_state(uint32_t reg) {
    const uint32_t r = reg & 0x3fffu;
    return (r >= 0x0200u && r <= 0x0250u && r != 0x0200u) || (r >= 0x0400u && r <= 0x04dcu &&
           r != 0x0428u && r != 0x043cu) || (r >= 0x0500u && r <= 0x0528u) ||
           (r >= 0x0510u && r <= 0x051cu) || r == 0x0148u || (r >= 0x0a0cu && r <= 0x0a18u) ||
           (r >= 0x0a5cu && r <= 0x0a64u) || r == 0x0ae8u || r == 0x0c90u || r == 0x0cacu ||
           r == 0x0cb0u || r == 0x0a74u || r == 0x0004u;
}

void sgxd_mkq_check_render(char *out, unsigned size) {
    out[0] = 0;
    if (!state) {
        return;
    }
    // The last job taken whose list starts a 3D (ISP_START_RENDER, 0x0428).
    const uint32_t pos = state[SGXD_MK_POS];
    const Shape *s = NULL;
    uint32_t job = 0;
    for (uint32_t back = 1; back <= SGXD_MKQ_JOBS && back <= pos && !s; back++) {
        job = pos - back;
        const uint32_t entry = jobs[job & (SGXD_MKQ_JOBS - 1u)];
        for (uint32_t n = 0; n < SHAPES && !s; n++) {
            if (shapes[n].entry == entry) {
                for (uint32_t k = 0; k < shapes[n].count; k++) {
                    if (pool[shapes[n].at + k].op == SGXD_OP_WRITE &&
                        pool[shapes[n].at + k].reg == 0x0428u) {
                        s = &shapes[n];
                        break;
                    }
                }
            }
        }
    }
    if (!s) {
        snprintf(out, size, "no render list among the last jobs");
        return;
    }
    // Each state register's last value in the list: the constant, or the job's data slot.
    static uint32_t reg[256], want[256];
    uint32_t nreg = 0, var = 0;
    const volatile uint32_t *slot = data + (job & (SGXD_MKQ_JOBS - 1u)) * SGXD_MK_DATA_WORDS;
    for (uint32_t k = 0; k < s->count; k++) {
        const SgxdOp *op = &pool[s->at + k];
        const int is_var = pool_var[s->at + k] || op->op == SGXD_OP_WAIT_TWOD;
        const uint32_t value = is_var ? slot[var + sgxd_mk_var_words(op) - 1u] : op->value;
        if (is_var) {
            var += sgxd_mk_var_words(op);
        }
        if (op->op != SGXD_OP_WRITE || !kept_state(op->reg)) {
            continue;
        }
        uint32_t i = 0;
        while (i < nreg && reg[i] != op->reg) {
            i++;
        }
        if (i == nreg && nreg < 256u) {
            reg[nreg++] = op->reg;
        }
        if (i < 256u) {
            want[i] = value;
        }
    }
    snprintf(out, size, "render job %u (%u ops, %u values): %u state registers;", (unsigned)job,
             (unsigned)s->count, (unsigned)var, (unsigned)nreg);
    unsigned differ = 0;
    for (uint32_t i = 0; i < nreg; i++) {
        uint32_t got = 0;
        sgxdReadRegs(reg[i], 1, &got);
        if (got != want[i]) {
            const size_t used = strlen(out);
            snprintf(out + used, size - used, " %x:%x/%x", (unsigned)reg[i], (unsigned)want[i],
                     (unsigned)got);
            differ++;
        }
    }
    const size_t used = strlen(out);
    snprintf(out + used, size - used, " (%u read back otherwise, as written/read)", differ);
}

volatile uint32_t *sgxd_mkq_state(void) {
    return state;
}

uint32_t sgxd_mkq_jobs_sent(void) {
    return job_end;
}

uint32_t sgxd_mkq_job_entry(uint32_t job) {
    return jobs ? jobs[job & (SGXD_MKQ_JOBS - 1u)] : 0;
}

uint32_t sgxd_mkq_generation(void) {
    return generation;
}

uint32_t sgxd_mkq_flips_done(void) {
    return flips_done;
}

const volatile uint64_t *sgxd_mkq_code(uint32_t *instructions) {
    *instructions = code_used;
    return code_mem;
}

const volatile uint32_t *sgxd_mkq_memory(uint32_t *words) {
    *words = (DATA_OFFSET + SGXD_MKQ_JOBS * SGXD_MK_DATA_WORDS * 4u) / 4u;
    return state;
}

// The shape's words: each op's kind, register, mask and timeout, its value unless the job
// carries it, and its mark.
static uint64_t shape_hash(const SgxdOp *ops, unsigned count, const uint8_t *variable) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned n = 0; n < count; n++) {
        const int var = sgxd_mk_is_variable(&ops[n], variable, n);
        // A STORE's address is a value too (the data slot's).
        const uint32_t mask = ops[n].op == SGXD_OP_STORE ? 0u : ops[n].mask;
        const uint32_t w[6] = {ops[n].op, ops[n].reg,        var ? 0u : ops[n].value,
                               mask,      ops[n].timeout_us, (uint32_t)var};
        for (unsigned k = 0; k < 6; k++) {
            h = (h ^ w[k]) * 0x100000001b3ull;
        }
    }
    return h ^ count;
}

static int shape_equal(const Shape *s, const SgxdOp *ops, unsigned count, const uint8_t *variable) {
    if (s->count != count) {
        return 0;
    }
    for (unsigned n = 0; n < count; n++) {
        const SgxdOp *p = &pool[s->at + n];
        const int var = sgxd_mk_is_variable(&ops[n], variable, n);
        if (p->op != ops[n].op || p->reg != ops[n].reg ||
            (ops[n].op != SGXD_OP_STORE && p->mask != ops[n].mask) ||
            p->timeout_us != ops[n].timeout_us || pool_var[s->at + n] != (uint8_t)var ||
            (!var && p->value != ops[n].value)) {
            return 0;
        }
    }
    return 1;
}

int sgxd_mkq_idle(void) {
    const SceUInt64 t0 = sceKernelGetProcessTimeWide();
    while (state[SGXD_MK_BUSY] || state[SGXD_MK_POS] != job_end) {
        if (state[SGXD_MK_FAULT] || sceKernelGetProcessTimeWide() - t0 > 1000000u) {
            return -1;
        }
    }
    return 0;
}

// The code space, the pool or the table full: once every job sent has run, the lists' code may
// be written again. The USSE fetches code through the SLC, so the SLC is written back and
// emptied too, not only the USE's code cache (with that alone, a list placed after a flip ended
// its run part way).
// Only the queue is waited for: sgxd_ptla_idle and sgxd_chain_idle would wait for the list this
// flip makes room for, counted but not sent, and fail with AZAHAR_ERR_STATE.
static int flip(void) {
    if (sgxd_mkq_idle() < 0) {
        // The caller's error is only AZAHAR_ERR_STATE: where the queue stands goes to the console.
        sceClibPrintf("[sgxd] microkernel flip %u: the queue not idle; job %u of %u taken (busy "
                      "%u), resume at %u, fault %u\n",
                      (unsigned)flips_done, (unsigned)state[SGXD_MK_POS],
                      (unsigned)state[SGXD_MK_END], (unsigned)state[SGXD_MK_BUSY],
                      (unsigned)state[SGXD_MK_RESUME], (unsigned)state[SGXD_MK_FAULT]);
        return -1;
    }
    SgxdOp inval[] = {
        W(USE_CACHE_INVAL, 1),
        W(SLC_CTRL_FLUSH, 0x10), P(SLC_EVENT_STATUS, 2, 2, 1000), W(SLC_EVENT_CLEAR, 2),
        W(SLC_CTRL_INVAL, 1), P(SLC_EVENT_STATUS, 1, 1, 1000), W(SLC_EVENT_CLEAR, 1),
    };
    const unsigned n = sizeof(inval) / sizeof(inval[0]);
    const int done = sgxdSubmit(inval, n);
    if (done != (int)n) {
        sceClibPrintf("[sgxd] microkernel flip %u: op %d of %u (the caches) did not complete\n",
                      (unsigned)flips_done, done, n);
        return -1;
    }
    flips_done++;
    memset(shapes, 0, sizeof(shapes));
    nshapes = 0;
    pool_used = 0;
    code_used = runtime_len;
    generation++;
#ifdef VITA_DIAGNOSTICS
    sgxd_mkq_flips++;
#endif
    return 0;
}

// The shape's code: found, or compiled into the code space. 0 (the runtime's start, never a
// list's) when it cannot be.
// A new shape's first instruction: a 1 KiB boundary at least 1 KiB past the code before it, so
// none of its lines can be in the USE's code cache from a prefetch (nothing writes USE_CACHE
// while the queue runs, docs/sgxd/pds.md).
#define CODE_GAP 128u
static uint32_t place(uint32_t used) {
    return (used + 2u * CODE_GAP - 1u) & ~(CODE_GAP - 1u);
}

static uint32_t shape_code(const SgxdOp *ops, unsigned count, const uint8_t *variable, int *err) {
    const uint64_t hash = shape_hash(ops, count, variable);
    uint32_t slot = (uint32_t)hash & (SHAPES - 1u);
    for (; shapes[slot].entry; slot = (slot + 1u) & (SHAPES - 1u)) {
        if (shapes[slot].hash == hash && shape_equal(&shapes[slot], ops, count, variable)) {
            return shapes[slot].entry;
        }
    }
    uint32_t refused = 0;
    uint32_t base = place(code_used);
    int len = sgxd_mk_compile(scratch, sizeof(scratch) / 8u, base, &layout, &rt, ops, count,
                              variable, writable, &refused);
    if (len < 0) {
        sgxd_mkq_refused = refused;
        sgxd_mkq_refused_kind = refused < count ? ops[refused].op : 0;
        *err = len;
        return 0;
    }
    if (base + (uint32_t)len > CODE_CAP || pool_used + count > POOL_OPS ||
        nshapes >= SHAPES * 3u / 4u) {
        if (flip() < 0) {
            *err = AZAHAR_ERR_STATE;
            return 0;
        }
        base = place(code_used);
        len = sgxd_mk_compile(scratch, sizeof(scratch) / 8u, base, &layout, &rt, ops, count,
                              variable, writable, &refused);
        if (len < 0 || base + (uint32_t)len > CODE_CAP || count > POOL_OPS) {
            *err = len < 0 ? len : SGX_USSE_ENOSPC;
            return 0;
        }
        slot = (uint32_t)hash & (SHAPES - 1u);
        while (shapes[slot].entry) {
            slot = (slot + 1u) & (SHAPES - 1u);
        }
    }
    const uint32_t entry = base;
    for (int n = 0; n < len; n++) {
        code_mem[entry + n] = scratch[n];
    }
    // The words in memory before a job names them.
    __asm__ volatile("dsb" ::: "memory");
    code_used = base + (uint32_t)len;
    for (unsigned n = 0; n < count; n++) {
        const int var = sgxd_mk_is_variable(&ops[n], variable, n);
        pool[pool_used + n] = ops[n];
        if (var) {
            pool[pool_used + n].value = 0;
        }
        pool_var[pool_used + n] = (uint8_t)var;
    }
    shapes[slot] = (Shape){hash, count, pool_used, entry};
    pool_used += count;
    nshapes++;
#ifdef VITA_DIAGNOSTICS
    sgxd_mkq_compiles++;
    sgxd_mkq_instructions += (uint32_t)len;
#endif
    return entry;
}

// Where each stream's data slot words come from (SgxdMkqCode::value_op): the TA stream's part op n
// is the list's op n, the 3D stream's part op 0 its wait for the TA job and part op n the list's
// op split + n.
static void value_plan(unsigned nta, unsigned n3, unsigned split, SgxdMkqCode *code) {
    for (unsigned s = 0; s < 2; s++) {
        const unsigned nops = s ? n3 : nta;
        unsigned k = 0;
        for (unsigned n = 0; n < nops; n++) {
            const SgxdOp *op = &part_ops[s][n];
            if (!sgxd_mk_is_variable(op, part_var[s], n)) {
                continue;
            }
            const unsigned from = s ? split + n : n;
            if (op->op == SGXD_OP_STORE) {
                if (k < SGXD_MK_DATA_WORDS) {
                    code->value_op[s][k] = (uint16_t)(from << 1 | 1u);
                }
                k++;
            }
            if (k < SGXD_MK_DATA_WORDS) {
                code->value_op[s][k] = s && n == 0 ? SGXD_MKQ_VALUE_JOB : (uint16_t)(from << 1);
            }
            k++;
        }
        code->values[s] = (uint8_t)(k < SGXD_MK_DATA_WORDS ? k : SGXD_MK_DATA_WORDS);
    }
}

// A data slot from the plan: sgxd_mk_values's words, reading only the ops that give them.
static void plan_values(volatile uint32_t *slot, const SgxdOp *ops, const uint16_t *from,
                        unsigned count, uint32_t job) {
    for (unsigned k = 0; k < count; k++) {
        const unsigned f = from[k];
        slot[k] = f == SGXD_MKQ_VALUE_JOB ? job : f & 1u ? ops[f >> 1].mask : ops[f >> 1].value;
    }
}

int sgxd_mkq_submit(const SgxdOp *ops, unsigned count, const uint8_t *variable, SgxdMkqCode *code) {
    if (state[SGXD_MK_FAULT]) {
        return AZAHAR_ERR_STATE;
    }
    uint32_t entry = code && code->generation == generation ? code->entry : 0;
    if (!entry) {
        int err = 0;
        entry = shape_code(ops, count, variable, &err);
        if (!entry) {
            return err;
        }
        if (code) {
            code->entry = entry;
            code->generation = generation;
            value_plan(nta, n3, split, code);
        }
    }
    // A job's slot is free once the job after it has been taken: the running one is POS - 1.
    if (job_end - state[SGXD_MK_POS] >= SGXD_MKQ_JOBS - 1u) {
        const SceUInt64 t0 = sceKernelGetProcessTimeWide();
        while (job_end - state[SGXD_MK_POS] >= SGXD_MKQ_JOBS - 1u) {
            if (state[SGXD_MK_FAULT]) {
                return AZAHAR_ERR_STATE;
            }
            if (sceKernelGetProcessTimeWide() - t0 > 1000000u) {
                return AZAHAR_ERR_FULL;
            }
        }
    }
    const uint32_t slot = job_end & (SGXD_MKQ_JOBS - 1u);
    sgxd_mk_values(data + slot * SGXD_MK_DATA_WORDS, ops, count, variable);
    jobs[slot] = entry;
#ifdef VITA_DIAGNOSTICS
    job_kind[slot] = sgxd_mkq_kind_next;
    job_label[slot] = 0;
    if (sgxd_mkq_kind_next == SGXD_MKQ_KIND_RENDER) {
        job_label[slot] = sgxd_mkq_label_next;
        __atomic_fetch_add(&sgxd_mkq_label_sent[sgxd_mkq_label_next], 1u, __ATOMIC_RELAXED);
        sgxd_mkq_label_next = 0;
    }
    sgxd_mkq_kind_next = SGXD_MKQ_KIND_OTHER;
#endif
    // The job before the END that publishes it; END before the look at BUSY (the GPU clears
    // BUSY, then reads END: one of the two sees the other's write).
    __asm__ volatile("dmb" ::: "memory");
    state[SGXD_MK_END] = ++job_end;
    __asm__ volatile("dmb" ::: "memory");
    if (!state[SGXD_MK_BUSY]) {
        state[SGXD_MK_BUSY] = 1;
        __asm__ volatile("dmb" ::: "memory");
#ifdef VITA_DIAGNOSTICS
        sgxd_kicks++;
#endif
        const int r = sgxdKick();
        if (r < 0) {
            return r;
        }
    }
    return 0;
}
