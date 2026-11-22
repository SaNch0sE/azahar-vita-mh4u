// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// azahar-native sgxd_mk.c: the microkernel's runtime and its compiled lists (sgxd_mk.h),
// assembled with sgxd_usse.c. Every form is one checked by a probe
// (docs/sgxd/microkernel.md): tests are AND or XOR against zero, a subtraction ~b + 1 + a.
// Registers: r0 the state block, r1 RESUME (the prologue) or POS (the next job), r6 and r7 an
// operand, r9..r15 scratch (the wake check uses r10..r15), r18 0xffffffff, r19 0x80000000, r20
// IR0 in the prologue and the job's data slot after it; the segment's values in the pool
// (pool_regs), which the next job's set-up uses as scratch too.
// A supervised call leaves none of them for certain: its return point sets r0, r18, r19 and
// r20 again, and a list loads its segment's values again after it.

#include "sgxd_mk.h"
#include "sgxd.h"
#include "sgxd_usse.h"

#define R(n) USSE_R(n)
#define IMM(n) USSE_IMM(n)
#define AL SGX_USSE_ALWAYS

#define MK_EVENT_STATUS 0x012cu          // bank 0
#define MK_SW_EVENT 0x4000u
#define MK_MASTER_EVENT_STATUS 0x412cu
#define MK_MASTER_EVENT_HOST_CLEAR 0x4134u
#define MK_TWOD_COMPLETE 0x08000000u
// MASTER_CLKGATECTL2 with the 2D clock back to auto (bits 8-9 = 2; the start sequence's value),
// as a PTLA completion leaves it; a PTLA list turns it on (sgxd_ptla.c).
#define MK_MASTER_CLKGATECTL2 0x4020u
#define MK_CLKGATECTL2_AUTO 0x2a9u
#define MK_PTLA_EVENT 0x1000u            // IR0's PTLACOMPLETE
#define MK_DATA_REG 20u

static void sub(struct sgx_usse_asm *a, struct sgx_usse_reg dst, struct sgx_usse_reg x,
                struct sgx_usse_reg y) {
    sgx_usse_bitop(a, AL, SGX_USSE_XOR, dst, y, R(18), 0);
    sgx_usse_imad2(a, AL, dst, dst, IMM(1), IMM(1));
    sgx_usse_imad2(a, AL, dst, dst, IMM(1), x);
}

// p0 = (r & 0x80000000) != 0, through scratch.
static void negative(struct sgx_usse_asm *a, struct sgx_usse_reg r, struct sgx_usse_reg scratch) {
    sgx_usse_bitop(a, AL, SGX_USSE_AND, scratch, r, R(19), 0);
    sgx_usse_test(a, AL, 0, SGX_USSE_TST_AND, SGX_USSE_NONZERO, scratch, scratch);
}

// state[word] += 1, through r9.
static void count(struct sgx_usse_asm *a, unsigned word) {
    sgx_usse_ld(a, AL, R(9), R(0), IMM(word), 0);
    sgx_usse_wdf(a, 0);
    sgx_usse_imad2(a, AL, R(9), R(9), IMM(1), IMM(1));
    sgx_usse_st(a, AL, R(0), IMM(word), R(9));
}

// The wake check, a subroutine ending in a return: WAKE_ARMED, CHAINS past WAKE_CHAINS and TWOD
// past WAKE_TWOD, then WAKE_ARMED cleared, WAKES counted and SW_EVENT raised. r10..r15.
static void emit_wake(struct sgx_usse_asm *a) {
    const int no_wake = sgx_usse_label(a);
    sgx_usse_ld(a, AL, R(10), R(0), IMM(SGXD_MK_WAKE_ARMED), 0);
    sgx_usse_ld(a, AL, R(11), R(0), IMM(SGXD_MK_CHAINS), 0);
    sgx_usse_ld(a, AL, R(13), R(0), IMM(SGXD_MK_TWOD), 0);
    sgx_usse_ld(a, AL, R(12), R(0), IMM(SGXD_MK_WAKE_CHAINS), 0);
    sgx_usse_ld(a, AL, R(14), R(0), IMM(SGXD_MK_WAKE_TWOD), 0);
    sgx_usse_wdf(a, 0);
    sgx_usse_test(a, AL, 0, SGX_USSE_TST_AND, SGX_USSE_ZERO, R(10), R(10));
    sgx_usse_ba(a, SGX_USSE_P0, no_wake);
    sub(a, R(15), R(11), R(12));
    negative(a, R(15), R(10));
    sgx_usse_ba(a, SGX_USSE_P0, no_wake);
    sub(a, R(15), R(13), R(14));
    negative(a, R(15), R(10));
    sgx_usse_ba(a, SGX_USSE_P0, no_wake);
    sgx_usse_st(a, AL, R(0), IMM(SGXD_MK_WAKE_ARMED), IMM(0));
    sgx_usse_ld(a, AL, R(10), R(0), IMM(SGXD_MK_WAKES), 0);
    sgx_usse_wdf(a, 0);
    sgx_usse_imad2(a, AL, R(10), R(10), IMM(1), IMM(1));
    sgx_usse_st(a, AL, R(0), IMM(SGXD_MK_WAKES), R(10));
    sgx_usse_idf(a, 0);
    sgx_usse_wdf(a, 0);
    sgx_usse_limm(a, AL, R(10), MK_SW_EVENT);
    sgx_usse_str(a, AL, MK_EVENT_STATUS, R(10));
    sgx_usse_bind(a, no_wake);
    sgx_usse_ret(a, AL);
}

int sgxd_mk_runtime(uint64_t *code, uint32_t cap, uint32_t base, const SgxdMkLayout *l,
                    SgxdMkRuntime *rt) {
    struct sgx_usse_asm as, *a = &as;
    if (!l->jobs || (l->jobs & (l->jobs - 1u)) || l->jobs > 0x10000u) {
        return SGX_USSE_EINVAL;
    }
    sgx_usse_init(a, code, cap, base);
    const int no_twod = sgx_usse_label(a), go = sgx_usse_label(a), next = sgx_usse_label(a);
    const int idle = sgx_usse_label(a), exit = sgx_usse_label(a), quiet = sgx_usse_label(a);
    const int wake = sgx_usse_label(a);
    const int supervisor = sgx_usse_label(a), supervised = sgx_usse_label(a);
    const int back = sgx_usse_label(a);

    // The prologue. pa0 before any ldr + WDF (docs/sgxd/microkernel.md).
    sgx_usse_smlsi(a);
    sgx_usse_mov(a, AL, R(MK_DATA_REG), USSE_PA(0));
    sgx_usse_lock(a, 7);
    sgx_usse_limm(a, AL, R(0), l->state_va);
    sgx_usse_limm(a, AL, R(18), 0xffffffffu);
    sgx_usse_limm(a, AL, R(19), 0x80000000u);
    // A PTLA completion: cleared, the 2D clock back
    // to auto, counted, its stores fenced, then the wake check. No SLC request: the PTLA goes
    // through the SLC, as the units whose output it copies do.
    sgx_usse_limm(a, AL, R(10), MK_PTLA_EVENT);
    sgx_usse_test(a, AL, 0, SGX_USSE_TST_AND, SGX_USSE_ZERO, R(MK_DATA_REG), R(10));
    sgx_usse_ba(a, SGX_USSE_P0, no_twod);
    sgx_usse_limm(a, AL, R(9), MK_TWOD_COMPLETE);
    sgx_usse_str(a, AL, MK_MASTER_EVENT_HOST_CLEAR, R(9));
    sgx_usse_limm(a, AL, R(9), MK_CLKGATECTL2_AUTO);
    sgx_usse_str(a, AL, MK_MASTER_CLKGATECTL2, R(9));
    count(a, SGXD_MK_TWOD);
    sgx_usse_idf(a, 0);
    sgx_usse_wdf(a, 0);
    sgx_usse_call(a, AL, wake);
    sgx_usse_bind(a, no_twod);
    // RESUME through the link register, the job's data slot with it; 0 is a new job.
    sgx_usse_ld(a, AL, R(1), R(0), IMM(SGXD_MK_RESUME), 0);
    sgx_usse_ld(a, AL, R(MK_DATA_REG), R(0), IMM(SGXD_MK_DATA), 0);
    sgx_usse_wdf(a, 0);
    sgx_usse_test(a, AL, 0, SGX_USSE_TST_AND, SGX_USSE_NONZERO, R(1), R(1));
    sgx_usse_ba(a, SGX_USSE_P0, go);

    // The next job: none after a fault (whose stores the fault fenced); none published, the
    // queue idle; otherwise POS moves past it, its data slot goes into r20 and DATA, and its list
    // runs.
    sgx_usse_bind(a, next);
    sgx_usse_ld(a, AL, R(1), R(0), IMM(SGXD_MK_POS), 0);
    sgx_usse_ld(a, AL, R(2), R(0), IMM(SGXD_MK_END), 0);
    sgx_usse_ld(a, AL, R(9), R(0), IMM(SGXD_MK_FAULT), 0);
    sgx_usse_wdf(a, 0);
    sgx_usse_test(a, AL, 0, SGX_USSE_TST_AND, SGX_USSE_NONZERO, R(9), R(9));
    sgx_usse_ba(a, SGX_USSE_P0, quiet);
    sgx_usse_test(a, AL, 0, SGX_USSE_TST_XOR, SGX_USSE_ZERO, R(1), R(2));
    sgx_usse_ba(a, SGX_USSE_P0, idle);
    sgx_usse_bitop(a, AL, SGX_USSE_AND, R(9), R(1), IMM(l->jobs - 1u), 0);
    sgx_usse_limm(a, AL, R(10), l->jobs_va);
    sgx_usse_imad2(a, AL, R(3), R(9), IMM(4), R(10));
    sgx_usse_ld(a, AL, R(4), R(3), IMM(0), 0);
    sgx_usse_limm(a, AL, R(11), l->data_va);
    sgx_usse_limm(a, AL, R(12), SGXD_MK_DATA_WORDS * 4u);
    sgx_usse_imad2(a, AL, R(MK_DATA_REG), R(9), R(12), R(11));
    sgx_usse_imad2(a, AL, R(1), R(1), IMM(1), IMM(1));
    sgx_usse_st(a, AL, R(0), IMM(SGXD_MK_POS), R(1));
    sgx_usse_st(a, AL, R(0), IMM(SGXD_MK_DATA), R(MK_DATA_REG));
    sgx_usse_wdf(a, 0);
    sgx_usse_setl(a, R(4));
    sgx_usse_ret(a, AL);

    // Out of jobs: BUSY cleared, then END read again (the host writes END, then reads BUSY). The
    // BUSY store is the run's: no run ends at a store fence with nothing stored before it (the
    // first run after the takeover never got past one, sgxd_test 21).
    sgx_usse_bind(a, idle);
    sgx_usse_st(a, AL, R(0), IMM(SGXD_MK_BUSY), IMM(0));
    sgx_usse_idf(a, 0);
    sgx_usse_wdf(a, 0);
    sgx_usse_ld(a, AL, R(2), R(0), IMM(SGXD_MK_END), 0);
    sgx_usse_wdf(a, 0);
    sgx_usse_test(a, AL, 0, SGX_USSE_TST_XOR, SGX_USSE_ZERO, R(1), R(2));
    sgx_usse_ba(a, SGX_USSE_P0, quiet);
    sgx_usse_st(a, AL, R(0), IMM(SGXD_MK_BUSY), IMM(1));
    sgx_usse_ba(a, AL, next);

    // A run's end: everything stored before the lock goes.
    sgx_usse_bind(a, exit);
    sgx_usse_idf(a, 0);
    sgx_usse_wdf(a, 0);
    sgx_usse_bind(a, quiet);
    sgx_usse_release(a, 7);
    sgx_usse_phas_end(a);
    sgx_usse_nop_end(a);
    sgx_usse_bind(a, go);
    sgx_usse_setl(a, R(1));
    sgx_usse_ret(a, AL);
    sgx_usse_bind(a, wake);
    emit_wake(a);
    // Supervised writes: the link and p0..p3 into r29, `sprvv.end`, and the
    // vector's next phase is the instruction after it (one return point for every caller); p0..p3
    // and the link back, then return. The caller has stored SV_COUNT {index, value} pairs from
    // SGXD_MK_SV_WORD and fenced them: the vector does not see an event task's secondary
    // attributes. Nothing is taken from a temporary after the call (caution: none was seen to
    // change r0..r25).
    sgx_usse_bind(a, supervised);
    sgx_usse_savl(a, R(29));
    sgx_usse_bitop(a, AL, SGX_USSE_SHL, R(29), R(29), IMM(4), 0);
    sgx_usse_bitop(a, SGX_USSE_P0, SGX_USSE_OR, R(29), R(29), IMM(1), 0);
    sgx_usse_bitop(a, SGX_USSE_P1, SGX_USSE_OR, R(29), R(29), IMM(2), 0);
    sgx_usse_bitop(a, SGX_USSE_P2, SGX_USSE_OR, R(29), R(29), IMM(4), 0);
    sgx_usse_bitop(a, SGX_USSE_P3, SGX_USSE_OR, R(29), R(29), IMM(8), 0);
    sgx_usse_limm(a, AL, R(27), l->state_va);
    sgx_usse_st(a, AL, R(27), IMM(SGXD_MK_SV_LINK), R(29));
    sgx_usse_idf(a, 0);
    sgx_usse_wdf(a, 0);
    sgx_usse_sprvv(a);
    sgx_usse_bind(a, back);
    sgx_usse_smlsi(a);
    sgx_usse_limm(a, AL, R(27), l->state_va);
    // The caller's registers from what holds across the call: the state block's address, the
    // constants, the data slot and the link.
    sgx_usse_ld(a, AL, R(29), R(27), IMM(SGXD_MK_SV_LINK), 0);
    sgx_usse_ld(a, AL, R(MK_DATA_REG), R(27), IMM(SGXD_MK_DATA), 0);
    sgx_usse_limm(a, AL, R(0), l->state_va);
    sgx_usse_limm(a, AL, R(18), 0xffffffffu);
    sgx_usse_limm(a, AL, R(19), 0x80000000u);
    sgx_usse_wdf(a, 0);
    sgx_usse_test(a, AL, 0, SGX_USSE_TST_AND, SGX_USSE_NONZERO, R(29), IMM(1));
    sgx_usse_test(a, AL, 1, SGX_USSE_TST_AND, SGX_USSE_NONZERO, R(29), IMM(2));
    sgx_usse_test(a, AL, 2, SGX_USSE_TST_AND, SGX_USSE_NONZERO, R(29), IMM(4));
    sgx_usse_test(a, AL, 3, SGX_USSE_TST_AND, SGX_USSE_NONZERO, R(29), IMM(8));
    sgx_usse_bitop(a, AL, SGX_USSE_SHR, R(29), R(29), IMM(4), 0);
    sgx_usse_setl(a, R(29));
    sgx_usse_ret(a, AL);
    // The supervisor vector (core 0's USE_SPRV): each pair's register written, in order, then
    // the next phase at the return point.
    // r26..r28, r30 and r31 hold nothing of a list's here.
    const int pair = sgx_usse_label(a);
    sgx_usse_bind(a, supervisor);
    sgx_usse_limm(a, AL, R(27), l->state_va);
    sgx_usse_ld(a, AL, R(26), R(27), IMM(SGXD_MK_SV_COUNT), 0);
    sgx_usse_limm(a, AL, R(28), l->state_va + SGXD_MK_SV_WORD * 4u);
    sgx_usse_wdf(a, 0);
    sgx_usse_bind(a, pair);
    sgx_usse_ld(a, AL, R(30), R(28), IMM(0), 0);
    sgx_usse_ld(a, AL, R(31), R(28), IMM(1), 0);
    sgx_usse_wdf(a, 0);
    sgx_usse_str_indirect(a, R(30), R(31));
    sgx_usse_imad2(a, AL, R(28), R(28), IMM(1), IMM(8));
    sgx_usse_dec_test(a, AL, 0, SGX_USSE_NONZERO, R(26), R(26), 1);
    sgx_usse_ba(a, SGX_USSE_P0, pair);
    sgx_usse_phas_return(a, sgx_usse_label_index(a, back));
    if (sgx_usse_finish(a) < 0) {
        return a->err;
    }
    rt->start = base;
    rt->next = sgx_usse_label_index(a, next);
    rt->exit = sgx_usse_label_index(a, exit);
    rt->wake = sgx_usse_label_index(a, wake);
    rt->supervisor = sgx_usse_label_index(a, supervisor);
    rt->supervised = sgx_usse_label_index(a, supervised);
    return (int)a->len;
}

// The branch emitted at `at` (an index into a->code) to target (an absolute index).
static void patch(struct sgx_usse_asm *a, uint32_t at, uint32_t target) {
    if (!a->err && at < a->len) {
        a->code[at] = (a->code[at] & ~0xfffffull) | (target & 0xfffffu);
    }
}

// FAULT = 1 + index, FAULT_KIND = kind, RESUME 0, SW_EVENT, then the run's end.
static void compiled_fault(struct sgx_usse_asm *a, uint32_t index, uint32_t kind, uint32_t exit) {
    sgx_usse_limm(a, AL, R(9), index + 1u);
    sgx_usse_st(a, AL, R(0), IMM(SGXD_MK_FAULT), R(9));
    sgx_usse_limm(a, AL, R(9), kind);
    sgx_usse_st(a, AL, R(0), IMM(SGXD_MK_FAULT_KIND), R(9));
    sgx_usse_st(a, AL, R(0), IMM(SGXD_MK_RESUME), IMM(0));
    sgx_usse_idf(a, 0);
    sgx_usse_wdf(a, 0);
    sgx_usse_limm(a, AL, R(9), MK_SW_EVENT);
    sgx_usse_str(a, AL, MK_EVENT_STATUS, R(9));
    sgx_usse_ba_index(a, AL, exit);
}

// The temporaries a segment's values go in: its large values used more than once (the most used
// first) from the front, its variable values from the back.
// All inside r0..r31: a KICK2 event task's r36 and up hold nothing, whatever its DOUTU declares.
static const uint8_t pool_regs[] = {2, 3, 4, 5, 8, 16, 17, 21, 22, 23, 24, 25};
// A poll group's reads: r26..r31 (the supervisor vector's and the run log's otherwise, never
// inside a group).
#define POLL_GROUP 6u
#define POLL_READS 26u
#define POOL (sizeof(pool_regs) / sizeof(pool_regs[0]))
// Variable values loaded together, one WDF: up to eight loads ahead of one WDF all land (test 22).
#define VAR_BATCH 8u
typedef struct ValueCache {
    uint32_t value[POOL];
    uint32_t n;
} ValueCache;

static void cache_build(ValueCache *c, uint32_t cap, const SgxdOp *ops, const uint8_t *variable,
                        uint32_t from, uint32_t to) {
    uint32_t seen[256], uses[256], nseen = 0;
    for (uint32_t k = from; k < to; k++) {
        uint32_t vals[3], nv = 0;
        if (ops[k].op == SGXD_OP_WRITE && ops[k].value >= 0x80u && !sgxd_mk_is_variable(&ops[k], variable, k)) {
            vals[nv++] = ops[k].value;
        } else if (ops[k].op == SGXD_OP_POLL) {
            if (ops[k].mask > 0xffffu) {
                vals[nv++] = ops[k].mask;
            }
            if (ops[k].value >= 0x80u) {
                vals[nv++] = ops[k].value;
            }
        }
        for (uint32_t v = 0; v < nv; v++) {
            uint32_t i = 0;
            while (i < nseen && seen[i] != vals[v]) {
                i++;
            }
            if (i == nseen && nseen < 256) {
                seen[nseen] = vals[v];
                uses[nseen++] = 0;
            }
            if (i < 256) {
                uses[i]++;
            }
        }
    }
    while (c->n < cap) {
        uint32_t best = nseen;
        for (uint32_t i = 0; i < nseen; i++) {
            if (uses[i] > 1 && (best == nseen || uses[i] > uses[best])) {
                best = i;
            }
        }
        if (best == nseen) {
            break;
        }
        c->value[c->n++] = seen[best];
        uses[best] = 0;
    }
}

// value as an operand: a 7-bit immediate, the cache's temporary, or loaded into r.
static struct sgx_usse_reg cached(struct sgx_usse_asm *a, const ValueCache *c, unsigned r,
                                  uint32_t value) {
    if (value < 0x80u) {
        return IMM(value);
    }
    for (uint32_t i = 0; i < c->n; i++) {
        if (c->value[i] == value) {
            return R(pool_regs[i]);
        }
    }
    sgx_usse_limm(a, AL, R(r), value);
    return R(r);
}

// One POLL: the first check ahead of the countdown's set-up (a poll met at once costs its read and
// four instructions), then the countdown, then the fault.
static void emit_poll(struct sgx_usse_asm *a, const ValueCache *c, const SgxdOp *op, uint32_t n,
                      uint32_t exit_at) {
    const uint32_t spins = op->timeout_us * SGXD_MK_POLL_SPINS + 1u;
    const struct sgx_usse_reg mask = op->mask <= 0xffffu ? IMM(op->mask) : cached(a, c, 7, op->mask);
    const struct sgx_usse_reg value = cached(a, c, 6, op->value);
    sgx_usse_ldr(a, R(10), op->reg, 0);
    sgx_usse_wdf(a, 0);
    sgx_usse_bitop(a, AL, SGX_USSE_AND, R(11), R(10), mask, 0);
    sgx_usse_test(a, AL, 0, SGX_USSE_TST_XOR, SGX_USSE_ZERO, R(11), value);
    const uint32_t met_first = a->len;
    sgx_usse_ba_index(a, SGX_USSE_P0, 0);
    sgx_usse_limm(a, AL, R(13), spins);
    const uint32_t loop = sgx_usse_here(a);
    sgx_usse_ldr(a, R(10), op->reg, 0);
    sgx_usse_wdf(a, 0);
    sgx_usse_bitop(a, AL, SGX_USSE_AND, R(11), R(10), mask, 0);
    sgx_usse_test(a, AL, 0, SGX_USSE_TST_XOR, SGX_USSE_ZERO, R(11), value);
    const uint32_t met = a->len;
    sgx_usse_ba_index(a, SGX_USSE_P0, 0);
    if (spins <= 0xffffu) {
        sgx_usse_dec_test(a, AL, 1, SGX_USSE_NONZERO, R(13), R(13), 1);
        sgx_usse_ba_index(a, SGX_USSE_P1, loop);
    } else {
        sgx_usse_imad2(a, AL, R(13), R(13), IMM(1), R(18));
        sgx_usse_test(a, AL, 1, SGX_USSE_TST_AND, SGX_USSE_ZERO, R(13), R(13));
        sgx_usse_ba_index(a, SGX_USSE_NOT_P1, loop);
    }
    sgx_usse_st(a, AL, R(0), IMM(SGXD_MK_VALUE), R(10));
    compiled_fault(a, n, SGXD_OP_POLL, exit_at);
    patch(a, met_first, sgx_usse_here(a));
    patch(a, met, sgx_usse_here(a));
}

void (*sgxd_mk_wait_placed)(uint32_t at, const SgxdOp *op);

int sgxd_mk_compile(uint64_t *code, uint32_t cap, uint32_t base, const SgxdMkLayout *l,
                    const SgxdMkRuntime *rt, const SgxdOp *ops, uint32_t nops,
                    const uint8_t *variable, int (*writable)(uint32_t reg), uint32_t *refused) {
    struct sgx_usse_asm as, *a = &as;
    sgx_usse_init(a, code, cap, base);
    *refused = 0xffffffffu;
    uint32_t nvar = 0;
    for (uint32_t n = 0; n < nops; n++) {
        if (sgxd_mk_is_variable(&ops[n], variable, n) &&
            (nvar += sgxd_mk_var_words(&ops[n])) > SGXD_MK_DATA_WORDS) {
            *refused = n;
            return SGX_USSE_EINVAL;
        }
    }

    // The list, segment by segment: a segment runs from the entry or a wait to the next wait
    // (temporaries do not outlive a run). Each segment's large values used more than once are
    // loaded into temporaries once, at its start; its variable values are loaded from the data
    // slot (r20) up to VAR_BATCH at a time as they are reached.
    uint32_t var = 0;   // the next variable value's word in the data slot
    for (uint32_t n = 0; n < nops && !a->err;) {
        uint32_t seg_end = n, seg_vars = 0;
        while (seg_end < nops && ops[seg_end].op != SGXD_OP_WAIT_EVENT &&
               ops[seg_end].op != SGXD_OP_WAIT_TWOD) {
            seg_vars += sgxd_mk_is_variable(&ops[seg_end], variable, seg_end)
                            ? sgxd_mk_var_words(&ops[seg_end])
                            : 0u;
            seg_end++;
        }
        const uint32_t batch = seg_vars < VAR_BATCH ? seg_vars : VAR_BATCH;
        ValueCache cache = {{0}, 0};
        cache_build(&cache, POOL - batch, ops, variable, n, seg_end);
        for (uint32_t i = 0; i < cache.n; i++) {
            sgx_usse_limm(a, AL, R(pool_regs[i]), cache.value[i]);
        }
        const uint32_t seg_var_end = var + seg_vars;
        uint32_t loaded_lo = 0, loaded_hi = 0;   // the data words in the batch's temporaries
        for (; n < seg_end && !a->err;) {
            const SgxdOp *op = &ops[n];
            const int reg_ok = !(op->reg & 3u) && op->reg < 0x20000u;
            if (!reg_ok && op->op != SGXD_OP_SIGNAL) {
                *refused = n;
                return SGX_USSE_EINVAL;
            }
            switch (op->op) {
            case SGXD_OP_WRITE:
                if (!writable(op->reg)) {
                    *refused = n;
                    return SGX_USSE_EINVAL;
                }
                if (sgxd_mk_supervised(op->reg)) {
                    // This and the consecutive supervised writes after it (a bank-0 code base as
                    // each core's four) in one call to the vector;
                    // never one whose value the job carries.
                    uint32_t pairs = 0, end = n;
                    while (end < seg_end && ops[end].op == SGXD_OP_WRITE &&
                           sgxd_mk_supervised(ops[end].reg)) {
                        const SgxdOp *q = &ops[end];
                        const uint32_t r = q->reg & 0x3fffu;
                        const unsigned cores = q->reg < 0x4000u && r != 0x0080u ? 4u : 1u;
                        if (sgxd_mk_is_variable(q, variable, end) || !writable(q->reg)) {
                            *refused = end;
                            return SGX_USSE_EINVAL;
                        }
                        if (pairs + cores > SGXD_MK_SV_PAIRS) {
                            break;
                        }
                        const struct sgx_usse_reg value = cached(a, &cache, 10, q->value);
                        for (unsigned c = 0; c < cores; c++) {
                            const uint32_t reg = cores > 1u ? 0x8000u + c * 0x4000u + r : q->reg;
                            sgx_usse_limm(a, AL, R(9), reg >> 2);
                            sgx_usse_st(a, AL, R(0), IMM(SGXD_MK_SV_WORD + pairs * 2u), R(9));
                            sgx_usse_st(a, AL, R(0), IMM(SGXD_MK_SV_WORD + pairs * 2u + 1u),
                                        value);
                            pairs++;
                        }
                        end++;
                    }
                    sgx_usse_st(a, AL, R(0), IMM(SGXD_MK_SV_COUNT), IMM(pairs));
                    sgx_usse_idf(a, 0);
                    sgx_usse_wdf(a, 0);
                    sgx_usse_call_index(a, AL, rt->supervised);
                    // No temporary survives the call for certain (the runtime's supervised
                    // helper): the segment's values again, and the variable values loaded anew
                    // at the next that needs one.
                    for (uint32_t i = 0; i < cache.n; i++) {
                        sgx_usse_limm(a, AL, R(pool_regs[i]), cache.value[i]);
                    }
                    loaded_lo = loaded_hi = 0;
                    n = end - 1;
                } else if (sgxd_mk_is_variable(op, variable, n)) {
                    if (var < loaded_lo || var >= loaded_hi) {
                        loaded_lo = var;
                        loaded_hi = var + batch < seg_var_end ? var + batch : seg_var_end;
                        for (uint32_t k = loaded_lo; k < loaded_hi; k++) {
                            sgx_usse_ld(a, AL, R(pool_regs[POOL - batch + (k - loaded_lo)]),
                                        R(MK_DATA_REG), IMM(k), 0);
                        }
                        sgx_usse_wdf(a, 0);
                    }
                    sgx_usse_str(a, AL, op->reg, R(pool_regs[POOL - batch + (var - loaded_lo)]));
                    var++;
                } else {
                    sgx_usse_str(a, AL, op->reg, cached(a, &cache, 9, op->value));
                }
                n++;
                break;
            case SGXD_OP_STORE:
                // The address and the word from the data slot, the store fenced before what
                // follows (a PTE's, before the MMU invalidate).
                sgx_usse_ld(a, AL, R(6), R(MK_DATA_REG), IMM(var), 0);
                sgx_usse_ld(a, AL, R(7), R(MK_DATA_REG), IMM(var + 1u), 0);
                sgx_usse_wdf(a, 0);
                sgx_usse_st(a, AL, R(6), IMM(0), R(7));
                sgx_usse_idf(a, 0);
                sgx_usse_wdf(a, 0);
                var += 2u;
                n++;
                break;
            case SGXD_OP_READ:
                sgx_usse_ldr(a, R(10), op->reg, 0);
                sgx_usse_wdf(a, 0);
                sgx_usse_st(a, AL, R(0), IMM(SGXD_MK_VALUE), R(10));
                n++;
                break;
            case SGXD_OP_POLL: {
                // Consecutive polls (up to POLL_GROUP): their reads issued together, one WDF;
                // all met, the group is done, otherwise each polls in turn.
                uint32_t group = 1;
                while (n + group < seg_end && group < POLL_GROUP &&
                       ops[n + group].op == SGXD_OP_POLL) {
                    group++;
                }
                for (uint32_t k = n; k < n + group; k++) {
                    if ((ops[k].reg & 3u) || ops[k].reg >= 0x20000u) {
                        *refused = k;
                        return SGX_USSE_EINVAL;
                    }
                }
                uint32_t all_met = 0;
                if (group > 1) {
                    for (uint32_t k = 0; k < group; k++) {
                        sgx_usse_ldr(a, R(POLL_READS + k), ops[n + k].reg, 0);
                    }
                    sgx_usse_wdf(a, 0);
                    uint32_t slow[POLL_GROUP];
                    for (uint32_t k = 0; k < group; k++) {
                        const SgxdOp *q = &ops[n + k];
                        sgx_usse_bitop(a, AL, SGX_USSE_AND, R(11), R(POLL_READS + k),
                                       q->mask <= 0xffffu ? IMM(q->mask) : cached(a, &cache, 7, q->mask),
                                       0);
                        sgx_usse_test(a, AL, 0, SGX_USSE_TST_XOR, SGX_USSE_ZERO, R(11),
                                      cached(a, &cache, 6, q->value));
                        slow[k] = a->len;
                        sgx_usse_ba_index(a, SGX_USSE_NOT_P0, 0);
                    }
                    all_met = a->len;
                    sgx_usse_ba_index(a, AL, 0);
                    const uint32_t slow_at = sgx_usse_here(a);
                    for (uint32_t k = 0; k < group; k++) {
                        patch(a, slow[k], slow_at);
                    }
                }
                for (uint32_t k = n; k < n + group; k++) {
                    emit_poll(a, &cache, &ops[k], k, rt->exit);
                }
                if (group > 1) {
                    patch(a, all_met, sgx_usse_here(a));
                }
                n += group;
                break;
            }
            case SGXD_OP_SIGNAL:
                sgx_usse_idf(a, 0);
                sgx_usse_wdf(a, 0);
                count(a, op->value == SGXD_SIGNAL_TA ? SGXD_MK_CHAIN_TA : SGXD_MK_CHAINS);
                sgx_usse_idf(a, 0);
                sgx_usse_wdf(a, 0);
                // Only a list's end wakes the app (sgxdWaker's targets are chains and twod).
                if (op->value != SGXD_SIGNAL_TA) {
                    sgx_usse_call_index(a, AL, rt->wake);
                }
                n++;
                break;
            default:
                *refused = n;
                return SGX_USSE_EINVAL;
            }
        }
        if (n < nops) {
            // The wait that ends the segment; the next segment's values load after it.
            const SgxdOp *op = &ops[n];
            const uint32_t here = sgx_usse_here(a);
            if (sgxd_mk_wait_placed) {
                sgxd_mk_wait_placed(here, op);
            }
            if (op->op == SGXD_OP_WAIT_EVENT) {
                sgx_usse_limm(a, AL, R(7), op->mask);
                sgx_usse_ldr(a, R(10), MK_MASTER_EVENT_STATUS, 0);
                sgx_usse_wdf(a, 0);
                sgx_usse_test(a, AL, 0, SGX_USSE_TST_AND, SGX_USSE_ZERO, R(10), R(7));
            } else {
                sgx_usse_ld(a, AL, R(10), R(0), IMM(SGXD_MK_TWOD), 0);
                sgx_usse_ld(a, AL, R(6), R(MK_DATA_REG), IMM(var), 0);
                sgx_usse_wdf(a, 0);
                var++;
                sub(a, R(11), R(10), R(6));
                negative(a, R(11), R(12));
            }
            const uint32_t met = a->len;
            sgx_usse_ba_index(a, SGX_USSE_NOT_P0, 0);
            // Not met: the next event's run goes on here. A WAIT_TWOD leaves the count it waits
            // for in VALUE, for a stop's post-mortem.
            if (op->op == SGXD_OP_WAIT_TWOD) {
                sgx_usse_st(a, AL, R(0), IMM(SGXD_MK_VALUE), R(6));
            }
            sgx_usse_limm(a, AL, R(9), here);
            sgx_usse_st(a, AL, R(0), IMM(SGXD_MK_RESUME), R(9));
            sgx_usse_ba_index(a, AL, rt->exit);
            patch(a, met, sgx_usse_here(a));
            if (op->op == SGXD_OP_WAIT_EVENT) {
                sgx_usse_str(a, AL, MK_MASTER_EVENT_HOST_CLEAR, R(7));
            }
            n++;
        }
    }
    // The list's end: the next job.
    sgx_usse_st(a, AL, R(0), IMM(SGXD_MK_RESUME), IMM(0));
    sgx_usse_ba_index(a, AL, rt->next);
    return a->err < 0 ? a->err : (int)a->len;
}
