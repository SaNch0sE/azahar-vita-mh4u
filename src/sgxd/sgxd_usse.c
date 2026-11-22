// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// sgxd_usse.c: the USSE assembler (sgxd_usse.h): operands checked and mapped to their banks,
// labels and fixups; the words come from sgxd_usse_enc.h.

#include "sgxd_usse.h"
#include "sgxd_usse_enc.h"

/* Integer source 1/2 bank and extension bit. */
static int src12(struct sgx_usse_asm* a, struct sgx_usse_reg r, uint32_t* bank, uint32_t* ext,
                 uint32_t* num, uint32_t imm_bits) {
    *ext = 0;
    switch (r.bank) {
    case SGX_USSE_R:
        *bank = 0;
        break;
    case SGX_USSE_O:
        *bank = 1;
        break;
    case SGX_USSE_PA:
        *bank = 2;
        break;
    case SGX_USSE_SA:
        *bank = 3;
        break;
    case SGX_USSE_IMM:
        *bank = 2;
        *ext = 1;
        if (r.n >> imm_bits)
            goto bad;
        *num = r.n;
        return 1;
    default:
        goto bad;
    }
    /* r124..r127 alias the internal registers */
    if (r.n > 127 || (r.bank == SGX_USSE_R && r.n >= 124))
        goto bad;
    *num = r.n;
    return 1;
bad:
    a->err = SGX_USSE_EINVAL;
    return 0;
}

static int dest(struct sgx_usse_asm* a, struct sgx_usse_reg r, uint32_t* bank, uint32_t* ext) {
    *ext = 0;
    switch (r.bank) {
    case SGX_USSE_R:
        *bank = 0;
        break;
    case SGX_USSE_O:
        *bank = 1;
        break;
    case SGX_USSE_PA:
        *bank = 2;
        break;
    case SGX_USSE_SA:
        *bank = 0;
        *ext = 1;
        break;
    default:
        a->err = SGX_USSE_EINVAL;
        return 0;
    }
    if (r.n > 127 || (r.bank == SGX_USSE_R && r.n >= 124)) {
        a->err = SGX_USSE_EINVAL;
        return 0;
    }
    return 1;
}

/* Source 0 of the three-source forms and the load/store base. */
static int src0(struct sgx_usse_asm* a, struct sgx_usse_reg r, uint32_t* bank, uint32_t* ext) {
    switch (r.bank) {
    case SGX_USSE_R:
        *bank = 0;
        *ext = 0;
        break;
    case SGX_USSE_PA:
        *bank = 1;
        *ext = 0;
        break;
    case SGX_USSE_O:
        *bank = 0;
        *ext = 1;
        break;
    case SGX_USSE_SA:
        *bank = 1;
        *ext = 1;
        break;
    default:
        a->err = SGX_USSE_EINVAL;
        return 0;
    }
    if (r.n > 127 || (r.bank == SGX_USSE_R && r.n >= 124)) {
        a->err = SGX_USSE_EINVAL;
        return 0;
    }
    return 1;
}

static void emit(struct sgx_usse_asm* a, uint64_t word) {
    if (a->err)
        return;
    if (a->len >= a->cap) {
        a->err = SGX_USSE_ENOSPC;
        return;
    }
    a->code[a->len++] = word;
}

/* A builder's word, or the error when a field did not fit. */
static void emit_word(struct sgx_usse_asm* a, uint64_t word, int bad) {
    if (bad) {
        if (!a->err)
            a->err = SGX_USSE_EINVAL;
        return;
    }
    emit(a, word);
}

void sgx_usse_init(struct sgx_usse_asm* a, uint64_t* buf, uint32_t cap, uint32_t base) {
    a->code = buf;
    a->cap = cap;
    a->len = 0;
    a->base = base;
    a->err = 0;
    a->nlabels = 0;
    a->nfix = 0;
}

int sgx_usse_label(struct sgx_usse_asm* a) {
    if (a->nlabels >= SGX_USSE_MAX_LABELS) {
        a->err = SGX_USSE_ENOSPC;
        return 0;
    }
    a->label[a->nlabels] = ~0u;
    return a->nlabels++;
}

void sgx_usse_bind(struct sgx_usse_asm* a, int label) {
    if (label < 0 || (uint32_t)label >= a->nlabels || a->label[label] != ~0u) {
        a->err = SGX_USSE_EINVAL;
        return;
    }
    a->label[label] = a->len;
}

uint32_t sgx_usse_label_index(struct sgx_usse_asm* a, int label) {
    if (label < 0 || (uint32_t)label >= a->nlabels || a->label[label] == ~0u) {
        a->err = SGX_USSE_EINVAL;
        return 0;
    }
    return a->base + a->label[label];
}

static void fixup(struct sgx_usse_asm* a, int label, int relative) {
    if (a->nfix >= SGX_USSE_MAX_FIXUPS || label < 0 || (uint32_t)label >= a->nlabels) {
        a->err = SGX_USSE_EINVAL;
        return;
    }
    a->fix[a->nfix].at = a->len;
    a->fix[a->nfix].label = label;
    a->fix[a->nfix].relative = relative;
    a->nfix++;
}

int sgx_usse_finish(struct sgx_usse_asm* a) {
    uint32_t n;

    for (n = 0; n < a->nfix && !a->err; n++) {
        uint32_t at = a->fix[n].at, target = a->label[a->fix[n].label];
        uint32_t field;

        if (target == ~0u) {
            a->err = SGX_USSE_EINVAL;
            break;
        }
        if (a->fix[n].relative)
            field = (target - at) & 0xfffff;
        else
            field = a->base + target;
        if (!a->fix[n].relative && field > 0xfffff) {
            a->err = SGX_USSE_ERANGE;
            break;
        }
        a->code[at] = (a->code[at] & ~0xfffffull) | field;
    }
    return a->err;
}

void sgx_usse_limm(struct sgx_usse_asm* a, enum sgx_usse_pred p, struct sgx_usse_reg dst,
                   uint32_t value) {
    uint32_t db, dx;
    int bad = 0;
    uint64_t w;

    if (!dest(a, dst, &db, &dx))
        return;
    w = sgxd_usse_limm(
        (struct sgxd_usse_limm_fields){
            .dest_bank_ext = dx, .pred = p & 7, .dest_bank = db, .dest_n = dst.n, .value = value},
        &bad);
    emit_word(a, w, bad);
}

void sgx_usse_bitop(struct sgx_usse_asm* a, enum sgx_usse_pred p, enum sgx_usse_bitop op,
                    struct sgx_usse_reg dst, struct sgx_usse_reg s1, struct sgx_usse_reg s2,
                    int invert) {
    static const uint8_t op1[] = {
        [SGX_USSE_AND] = 2, [SGX_USSE_OR] = 2,  [SGX_USSE_XOR] = 3,
        [SGX_USSE_SHL] = 4, [SGX_USSE_SHR] = 5, [SGX_USSE_ASR] = 5,
    };
    static const uint8_t op2[] = {
        [SGX_USSE_OR] = 1,
        [SGX_USSE_ASR] = 1,
    };
    uint32_t db, dx, b1, x1, n1, b2, x2, n2;
    int bad = 0;
    uint64_t w;

    if ((unsigned int)op > SGX_USSE_ASR) {
        a->err = SGX_USSE_EINVAL;
        return;
    }
    if (!dest(a, dst, &db, &dx) || !src12(a, s1, &b1, &x1, &n1, 7) ||
        !src12(a, s2, &b2, &x2, &n2, 16))
        return;
    /* a 16-bit immediate spreads over three fields */
    w = sgxd_usse_vbw((struct sgxd_usse_vbw_fields){.op1 = op1[op],
                                                    .pred = p & 7,
                                                    .dest_ext = dx,
                                                    .src1_ext = x1,
                                                    .src2_ext = x2,
                                                    .src2_invert = invert ? 1 : 0,
                                                    .src2_exth = x2 ? (n2 >> 14) & 3 : 0,
                                                    .op2 = op2[op],
                                                    .dest_bank = db,
                                                    .src1_bank = b1,
                                                    .src2_bank = b2,
                                                    .dest_n = dst.n,
                                                    .src2_sel = x2 ? (n2 >> 7) & 0x7f : 0,
                                                    .src1_n = n1,
                                                    .src2_n = x2 ? n2 & 0x7f : n2},
                      &bad);
    emit_word(a, w, bad);
}

void sgx_usse_mov(struct sgx_usse_asm* a, enum sgx_usse_pred p, struct sgx_usse_reg dst,
                  struct sgx_usse_reg src) {
    sgx_usse_bitop(a, p, SGX_USSE_OR, dst, src, USSE_IMM(0), 0);
}

static void imad2(struct sgx_usse_asm* a, enum sgx_usse_pred p, struct sgx_usse_reg dst,
                  struct sgx_usse_reg s0, struct sgx_usse_reg s1, struct sgx_usse_reg s2,
                  int is_signed) {
    uint32_t db, dx, b0, x0, b1, x1, n1, b2, x2, n2;
    int bad = 0;
    uint64_t w;

    if (!dest(a, dst, &db, &dx) || !src0(a, s0, &b0, &x0) || !src12(a, s1, &b1, &x1, &n1, 7) ||
        !src12(a, s2, &b2, &x2, &n2, 7))
        return;
    w = sgxd_usse_i32mad2((struct sgxd_usse_i32mad2_fields){.pred = p & 7,
                                                            .dest_bank_ext = dx,
                                                            .src1_bank_ext = x1,
                                                            .src2_bank_ext = x2,
                                                            .src0_bank_ext = x0,
                                                            .is_signed = is_signed ? 1 : 0,
                                                            .src0_bank = b0,
                                                            .dest_bank = db,
                                                            .src1_bank = b1,
                                                            .src2_bank = b2,
                                                            .dest_n = dst.n,
                                                            .src0_n = s0.n,
                                                            .src1_n = n1,
                                                            .src2_n = n2},
                          &bad);
    emit_word(a, w, bad);
}

void sgx_usse_imad2(struct sgx_usse_asm* a, enum sgx_usse_pred p, struct sgx_usse_reg dst,
                    struct sgx_usse_reg s0, struct sgx_usse_reg s1, struct sgx_usse_reg s2) {
    imad2(a, p, dst, s0, s1, s2, 0);
}

void sgx_usse_imad2_signed(struct sgx_usse_asm* a, enum sgx_usse_pred p, struct sgx_usse_reg dst,
                           struct sgx_usse_reg s0, struct sgx_usse_reg s1, struct sgx_usse_reg s2) {
    imad2(a, p, dst, s0, s1, s2, 1);
}

void sgx_usse_add(struct sgx_usse_asm* a, enum sgx_usse_pred p, struct sgx_usse_reg dst,
                  struct sgx_usse_reg s0, struct sgx_usse_reg s2) {
    sgx_usse_imad2(a, p, dst, s0, USSE_IMM(1), s2);
}

/* sign test (none, negative, positive), zero test (none, zero, nonzero), AND */
static const uint8_t tests[][3] = {
    [SGX_USSE_ZERO] = {0, 1, 1},        [SGX_USSE_NONZERO] = {0, 2, 1},
    [SGX_USSE_NEG] = {1, 0, 1},         [SGX_USSE_POS_OR_ZERO] = {2, 0, 1},
    [SGX_USSE_NEG_OR_ZERO] = {1, 1, 0}, [SGX_USSE_POS_NONZERO] = {2, 2, 1},
};

void sgx_usse_test(struct sgx_usse_asm* a, enum sgx_usse_pred p, unsigned int pdst,
                   enum sgx_usse_tstop op, enum sgx_usse_cond cond, struct sgx_usse_reg s1,
                   struct sgx_usse_reg s2) {
    static const uint8_t alu[][2] = {
        [SGX_USSE_TST_AND] = {3, 0},
        [SGX_USSE_TST_XOR] = {3, 2},
        [SGX_USSE_TST_SUB_S32] = {1, 14},
        [SGX_USSE_TST_SUB_U32] = {1, 15},
    };
    uint32_t b1, x1, n1, b2, x2, n2;
    int bad = 0;
    uint64_t w;

    if (pdst > 3 || (unsigned int)cond > SGX_USSE_POS_NONZERO ||
        (unsigned int)op > SGX_USSE_TST_SUB_U32) {
        a->err = SGX_USSE_EINVAL;
        return;
    }
    if (!src12(a, s1, &b1, &x1, &n1, 7) || !src12(a, s2, &b2, &x2, &n2, 7))
        return;
    /* No register result, the destination the special bank's sp0: every test in Sony's
     * microkernel. */
    w = sgxd_usse_vtst((struct sgxd_usse_vtst_fields){.pred = p & 7,
                                                      .dest_ext = 1,
                                                      .src1_ext = x1,
                                                      .src2_ext = x2,
                                                      .sign_test = tests[cond][0],
                                                      .zero_test = tests[cond][1],
                                                      .test_crcomb_and = tests[cond][2],
                                                      .pdst_n = pdst,
                                                      .dest_bank = 1,
                                                      .src1_bank = b1,
                                                      .src2_bank = b2,
                                                      .alu_sel = alu[op][0],
                                                      .alu_op = alu[op][1],
                                                      .src1_n = n1,
                                                      .src2_n = n2},
                       &bad);
    emit_word(a, w, bad);
}

void sgx_usse_dec_test(struct sgx_usse_asm* a, enum sgx_usse_pred p, unsigned int pdst,
                       enum sgx_usse_cond cond, struct sgx_usse_reg dst, struct sgx_usse_reg s1,
                       uint32_t imm) {
    uint32_t b1, x1, n1;
    int bad = 0;
    uint64_t w;

    if (pdst > 3 || (unsigned int)cond > SGX_USSE_POS_NONZERO || imm > 0x7f ||
        dst.bank != SGX_USSE_R || dst.n >= 124) {
        a->err = SGX_USSE_EINVAL;
        return;
    }
    if (!src12(a, s1, &b1, &x1, &n1, 7))
        return;
    /* Sony's 0x4801028020b5c281: source 2 an immediate, the result written back to a
     * temporary, ALU select 1, op 7. */
    w = sgxd_usse_vtst((struct sgxd_usse_vtst_fields){.pred = p & 7,
                                                      .src1_ext = x1,
                                                      .src2_ext = 1,
                                                      .sign_test = tests[cond][0],
                                                      .zero_test = tests[cond][1],
                                                      .test_crcomb_and = tests[cond][2],
                                                      .pdst_n = pdst,
                                                      .src1_bank = b1,
                                                      .src2_bank = 2,
                                                      .dest_n = dst.n,
                                                      .test_wben = 1,
                                                      .alu_sel = 1,
                                                      .alu_op = 7,
                                                      .src1_n = n1,
                                                      .src2_n = imm},
                       &bad);
    emit_word(a, w, bad);
}

static void ldst(struct sgx_usse_asm* a, int load, enum sgx_usse_pred p, struct sgx_usse_reg dst,
                 struct sgx_usse_reg base, struct sgx_usse_reg off, struct sgx_usse_reg val,
                 unsigned int drc) {
    uint32_t b0, x0, b1, x1, n1, b2, x2, n2, dpa = 0;
    int bad = 0;
    uint64_t w;

    if (drc > 1) {
        a->err = SGX_USSE_EINVAL;
        return;
    }
    if (!src0(a, base, &b0, &x0) || !src12(a, off, &b1, &x1, &n1, 7) ||
        !src12(a, val, &b2, &x2, &n2, 7))
        return;
    if (load) {
        if (dst.bank == SGX_USSE_PA)
            dpa = 1;
        else if (dst.bank != SGX_USSE_R || dst.n >= 124)
            a->err = SGX_USSE_EINVAL;
    }
    /* One access (MOE expand), the cache bypass bit as every firmware access sets it. */
    w = sgxd_usse_vldst((struct sgxd_usse_vldst_fields){.op = load ? 1 : 2,
                                                        .pred = p & 7,
                                                        .moe_expand = 1,
                                                        .src0_bank_ext = x0,
                                                        .src1_bank_ext = x1,
                                                        .src2_bank_ext = x2,
                                                        .dest_bank_primattr = dpa,
                                                        .src0_bank = b0,
                                                        .cache_by_pass12 = 1,
                                                        .drc_sel = drc,
                                                        .src1_bank = b1,
                                                        .src2_bank = b2,
                                                        .dest_n = load ? dst.n : 0,
                                                        .src0_n = base.n,
                                                        .src1_n = n1,
                                                        .src2_n = n2},
                        &bad);
    emit_word(a, w, bad);
}

void sgx_usse_ld(struct sgx_usse_asm* a, enum sgx_usse_pred p, struct sgx_usse_reg dst,
                 struct sgx_usse_reg base, struct sgx_usse_reg offset, unsigned int drc) {
    ldst(a, 1, p, dst, base, offset, USSE_IMM(0), drc);
}

void sgx_usse_st(struct sgx_usse_asm* a, enum sgx_usse_pred p, struct sgx_usse_reg base,
                 struct sgx_usse_reg offset, struct sgx_usse_reg value) {
    ldst(a, 0, p, USSE_R(0), base, offset, value, 0);
}

void sgx_usse_ldr(struct sgx_usse_asm* a, struct sgx_usse_reg dst, uint32_t reg, unsigned int drc) {
    uint32_t index = reg >> 2;
    int bad = 0;
    uint64_t w;

    if ((reg & 3) || index >> 18 || drc > 3 ||
        (dst.bank != SGX_USSE_R && dst.bank != SGX_USSE_PA) || dst.n > 127 ||
        (dst.bank == SGX_USSE_R && dst.n >= 124)) {
        a->err = SGX_USSE_EINVAL;
        return;
    }
    w = sgxd_usse_ldr(dst.bank == SGX_USSE_PA, dst.n, index, drc, &bad);
    emit_word(a, w, bad);
}

/* The short predicate of STR: always, p0, p1, !p0. */
static int short_pred(struct sgx_usse_asm* a, enum sgx_usse_pred p, uint32_t* out) {
    switch (p) {
    case SGX_USSE_ALWAYS:
        *out = 0;
        return 1;
    case SGX_USSE_P0:
        *out = 1;
        return 1;
    case SGX_USSE_P1:
        *out = 2;
        return 1;
    case SGX_USSE_NOT_P0:
        *out = 3;
        return 1;
    default:
        a->err = SGX_USSE_EINVAL;
        return 0;
    }
}

void sgx_usse_str(struct sgx_usse_asm* a, enum sgx_usse_pred p, uint32_t reg,
                  struct sgx_usse_reg src) {
    uint32_t index = reg >> 2, sp, b1, x1, n1;
    int bad = 0;
    uint64_t w;

    if ((reg & 3) || index >> 18) {
        a->err = SGX_USSE_EINVAL;
        return;
    }
    if (!short_pred(a, p, &sp) || !src12(a, src, &b1, &x1, &n1, 7))
        return;
    w = sgxd_usse_str(sp, b1, x1, n1, index, &bad);
    emit_word(a, w, bad);
}

void sgx_usse_ldr_indirect(struct sgx_usse_asm* a, struct sgx_usse_reg dst,
                           struct sgx_usse_reg index, unsigned int drc) {
    uint32_t b2, x2, n2;
    int bad = 0;
    uint64_t w;

    if (drc > 3 || (dst.bank != SGX_USSE_R && dst.bank != SGX_USSE_PA) || dst.n > 127 ||
        (dst.bank == SGX_USSE_R && dst.n >= 124)) {
        a->err = SGX_USSE_EINVAL;
        return;
    }
    if (!src12(a, index, &b2, &x2, &n2, 0))
        return;
    if (x2) {
        a->err = SGX_USSE_EINVAL;
        return;
    }
    w = sgxd_usse_ldr_indirect(dst.bank == SGX_USSE_PA, dst.n, drc, b2, n2, &bad);
    emit_word(a, w, bad);
}

void sgx_usse_str_indirect(struct sgx_usse_asm* a, struct sgx_usse_reg index,
                           struct sgx_usse_reg src) {
    uint32_t b1, x1, n1, b2, x2, n2;
    int bad = 0;
    uint64_t w;

    if (!src12(a, src, &b1, &x1, &n1, 7) || !src12(a, index, &b2, &x2, &n2, 0))
        return;
    if (x2) {
        a->err = SGX_USSE_EINVAL;
        return;
    }
    w = sgxd_usse_str_indirect(b1, x1, n1, b2, n2, &bad);
    emit_word(a, w, bad);
}

/* An absolute branch; the fixup or the caller gives its target. */
static uint64_t branch_word(enum sgx_usse_pred p, uint32_t target, int link, int monitor,
                            int* bad) {
    return sgxd_usse_br((struct sgxd_usse_br_fields){.pred = p & 7,
                                                     .br_monitor = monitor ? 1 : 0,
                                                     .save_link = link ? 1 : 0,
                                                     .br_off = target},
                        bad);
}

static void branch(struct sgx_usse_asm* a, enum sgx_usse_pred p, int label, int link, int monitor) {
    int bad = 0;
    uint64_t w = branch_word(p, 0, link, monitor, &bad);

    fixup(a, label, 0);
    emit_word(a, w, bad);
}

void sgx_usse_ba(struct sgx_usse_asm* a, enum sgx_usse_pred p, int label) {
    branch(a, p, label, 0, 0);
}

void sgx_usse_call(struct sgx_usse_asm* a, enum sgx_usse_pred p, int label) {
    branch(a, p, label, 1, 0);
}

void sgx_usse_ba_monitor(struct sgx_usse_asm* a, enum sgx_usse_pred p, int label) {
    branch(a, p, label, 0, 1);
}

static void branch_index(struct sgx_usse_asm* a, enum sgx_usse_pred p, uint32_t index, int link,
                         int monitor) {
    int bad = 0;
    uint64_t w;

    if (index > 0xfffff) {
        a->err = SGX_USSE_ERANGE;
        return;
    }
    w = branch_word(p, index, link, monitor, &bad);
    emit_word(a, w, bad);
}

void sgx_usse_ba_index(struct sgx_usse_asm* a, enum sgx_usse_pred p, uint32_t index) {
    branch_index(a, p, index, 0, 0);
}

void sgx_usse_call_index(struct sgx_usse_asm* a, enum sgx_usse_pred p, uint32_t index) {
    branch_index(a, p, index, 1, 0);
}

void sgx_usse_ba_monitor_index(struct sgx_usse_asm* a, enum sgx_usse_pred p, uint32_t index) {
    branch_index(a, p, index, 0, 1);
}

void sgx_usse_ret(struct sgx_usse_asm* a, enum sgx_usse_pred p) {
    int bad = 0;
    uint64_t w = sgxd_usse_ret(p & 7, &bad);

    emit_word(a, w, bad);
}

void sgx_usse_setl(struct sgx_usse_asm* a, struct sgx_usse_reg src) {
    int bad = 0;
    uint64_t w;

    if (src.bank != SGX_USSE_R || src.n >= 124) {
        a->err = SGX_USSE_EINVAL;
        return;
    }
    w = sgxd_usse_setl(src.n, &bad);
    emit_word(a, w, bad);
}

void sgx_usse_savl(struct sgx_usse_asm* a, struct sgx_usse_reg dst) {
    int bad = 0;
    uint64_t w;

    if (dst.bank != SGX_USSE_R || dst.n >= 124) {
        a->err = SGX_USSE_EINVAL;
        return;
    }
    w = sgxd_usse_savl(dst.n, &bad);
    emit_word(a, w, bad);
}

void sgx_usse_nop(struct sgx_usse_asm* a) {
    int bad = 0;
    uint64_t w = sgxd_usse_nop(0, 0, &bad);

    emit_word(a, w, bad);
}

void sgx_usse_nop_end(struct sgx_usse_asm* a) {
    int bad = 0;
    uint64_t w = sgxd_usse_nop(0, 1, &bad);

    emit_word(a, w, bad);
}

void sgx_usse_phas_end(struct sgx_usse_asm* a) {
    int bad = 0;
    uint64_t w = sgxd_usse_phas((struct sgxd_usse_phas_fields){.imm = 1, .wait_cond = 7}, &bad);

    emit_word(a, w, bad);
}

void sgx_usse_sprvv(struct sgx_usse_asm* a) {
    int bad = 0;
    uint64_t w = sgxd_usse_phas((struct sgxd_usse_phas_fields){.sprvv = 1, .end = 1}, &bad);

    emit_word(a, w, bad);
}

void sgx_usse_phas_return(struct sgx_usse_asm* a, uint32_t next) {
    int bad = 0;
    uint64_t w;

    next &= 0xfffffu;
    w = sgxd_usse_phas((struct sgxd_usse_phas_fields){.end = 1,
                                                      .imm = 1,
                                                      .mode = 1,
                                                      .exe_addr_high = next >> 14,
                                                      .src1_n_or_exe_addr_mid = (next >> 7) & 0x7f,
                                                      .src2_n_or_exe_addr_low = next & 0x7f},
                       &bad);
    emit_word(a, w, bad);
}

void sgx_usse_smlsi(struct sgx_usse_asm* a) {
    int bad = 0;
    uint64_t w = sgxd_usse_smlsi((struct sgxd_usse_smlsi_fields){.src2_inc = 1}, &bad);

    emit_word(a, w, bad);
}

void sgx_usse_idf(struct sgx_usse_asm* a, unsigned int drc) {
    int bad = 0;
    uint64_t w = sgxd_usse_idf(drc & 3, &bad);

    emit_word(a, w, bad);
}

void sgx_usse_wdf(struct sgx_usse_asm* a, unsigned int drc) {
    int bad = 0;
    uint64_t w = sgxd_usse_wdf(drc & 3, 0, &bad);

    emit_word(a, w, bad);
}

void sgx_usse_lock(struct sgx_usse_asm* a, unsigned int mutex) {
    int bad = 0;
    uint64_t w = sgxd_usse_lock(mutex & 0xf, 0, &bad);

    emit_word(a, w, bad);
}

void sgx_usse_release(struct sgx_usse_asm* a, unsigned int mutex) {
    int bad = 0;
    uint64_t w = sgxd_usse_lock(mutex & 0xf, 1, &bad);

    emit_word(a, w, bad);
}

void sgx_usse_setm(struct sgx_usse_asm* a, unsigned int monitor) {
    int bad = 0;
    uint64_t w = sgxd_usse_setm(monitor & 0xf, &bad);

    emit_word(a, w, bad);
}

void sgx_usse_emitpds(struct sgx_usse_asm* a, unsigned int r_task, unsigned int r_pds,
                      unsigned int r_ir0, uint32_t sideband) {
    int bad = 0;
    uint64_t w;

    /* VEC34 cores count the emit sources in register pairs */
    if ((r_task | r_pds | r_ir0) & 1 || r_task >= 124 || r_pds >= 124 || r_ir0 >= 124 ||
        sideband >> 14) {
        a->err = SGX_USSE_EINVAL;
        return;
    }
    w = sgxd_usse_emitpds(r_task / 2, r_pds / 2, r_ir0 / 2, sideband, &bad);
    emit_word(a, w, bad);
}
