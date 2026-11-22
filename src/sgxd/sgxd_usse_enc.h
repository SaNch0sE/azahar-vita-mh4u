// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// The USSE instruction words sgxd writes, one builder per form: the fields in, the 64-bit word
// out. The microkernel's assembler (sgxd_usse.c), the render set-ups' fixed programs
// (sgxd_recipe.c) and the shader emitter (usse_encoder.cpp) all encode through these. The
// pattern forms follow Vita3K's decoder patterns, 64 characters from bit 63 down: a run of one
// letter is a field, 0 and 1 are fixed bits, - is written as 0. A field too wide for its bits
// sets *bad; the word then holds its low bits.
#ifndef SGXD_USSE_ENC_H
#define SGXD_USSE_ENC_H

#include <stdint.h>

static inline uint64_t sgxd_usse_put(uint32_t value, unsigned shift, unsigned width, int* bad) {
    if ((uint64_t)value >> width)
        *bad = 1;
    return ((uint64_t)value & ((UINT64_C(1) << width) - 1)) << shift;
}

/* VBW: bitwise and shifts: 01ooopppsnrydecxaaaaittttthhbwkkffggjjjjjjjlllllllmmmmmmmqqqqqqq */
struct sgxd_usse_vbw_fields {
    uint32_t op1, pred, skipinv, nosched, repeat_sel, sync_start, dest_ext, end, src1_ext, src2_ext,
        repeat_count, src2_invert, src2_rot, src2_exth, op2, bitwise_partial, dest_bank, src1_bank,
        src2_bank, dest_n, src2_sel, src1_n, src2_n;
};
static inline uint64_t sgxd_usse_vbw(struct sgxd_usse_vbw_fields f, int* bad) {
    return UINT64_C(0x4000000000000000) | sgxd_usse_put(f.op1, 59, 3, bad) |
           sgxd_usse_put(f.pred, 56, 3, bad) | sgxd_usse_put(f.skipinv, 55, 1, bad) |
           sgxd_usse_put(f.nosched, 54, 1, bad) | sgxd_usse_put(f.repeat_sel, 53, 1, bad) |
           sgxd_usse_put(f.sync_start, 52, 1, bad) | sgxd_usse_put(f.dest_ext, 51, 1, bad) |
           sgxd_usse_put(f.end, 50, 1, bad) | sgxd_usse_put(f.src1_ext, 49, 1, bad) |
           sgxd_usse_put(f.src2_ext, 48, 1, bad) | sgxd_usse_put(f.repeat_count, 44, 4, bad) |
           sgxd_usse_put(f.src2_invert, 43, 1, bad) | sgxd_usse_put(f.src2_rot, 38, 5, bad) |
           sgxd_usse_put(f.src2_exth, 36, 2, bad) | sgxd_usse_put(f.op2, 35, 1, bad) |
           sgxd_usse_put(f.bitwise_partial, 34, 1, bad) | sgxd_usse_put(f.dest_bank, 32, 2, bad) |
           sgxd_usse_put(f.src1_bank, 30, 2, bad) | sgxd_usse_put(f.src2_bank, 28, 2, bad) |
           sgxd_usse_put(f.dest_n, 21, 7, bad) | sgxd_usse_put(f.src2_sel, 14, 7, bad) |
           sgxd_usse_put(f.src1_n, 7, 7, bad) | sgxd_usse_put(f.src2_n, 0, 7, bad);
}

/* I16MAD: 16-bit integer multiply-add:
 * 10100ppasnredbck-tttmmffoolhhgiijjqquuuuuuuvvvvvvvwwwwwwwxxxxxxx */
struct sgxd_usse_i16mad_fields {
    uint32_t pred, abs, skipinv, nosched, src2_neg, sel1h_upper8, dest_bank_ext, end, src1_bank_ext,
        src2_bank_ext, repeat_count, mode, src2_format, src1_format, sel2h_upper8, or_shift,
        src0_bank, dest_bank, src1_bank, src2_bank, dest_n, src0_n, src1_n, src2_n;
};
static inline uint64_t sgxd_usse_i16mad(struct sgxd_usse_i16mad_fields f, int* bad) {
    return UINT64_C(0xa000000000000000) | sgxd_usse_put(f.pred, 57, 2, bad) |
           sgxd_usse_put(f.abs, 56, 1, bad) | sgxd_usse_put(f.skipinv, 55, 1, bad) |
           sgxd_usse_put(f.nosched, 54, 1, bad) | sgxd_usse_put(f.src2_neg, 53, 1, bad) |
           sgxd_usse_put(f.sel1h_upper8, 52, 1, bad) | sgxd_usse_put(f.dest_bank_ext, 51, 1, bad) |
           sgxd_usse_put(f.end, 50, 1, bad) | sgxd_usse_put(f.src1_bank_ext, 49, 1, bad) |
           sgxd_usse_put(f.src2_bank_ext, 48, 1, bad) | sgxd_usse_put(f.repeat_count, 44, 3, bad) |
           sgxd_usse_put(f.mode, 42, 2, bad) | sgxd_usse_put(f.src2_format, 40, 2, bad) |
           sgxd_usse_put(f.src1_format, 38, 2, bad) | sgxd_usse_put(f.sel2h_upper8, 37, 1, bad) |
           sgxd_usse_put(f.or_shift, 35, 2, bad) | sgxd_usse_put(f.src0_bank, 34, 1, bad) |
           sgxd_usse_put(f.dest_bank, 32, 2, bad) | sgxd_usse_put(f.src1_bank, 30, 2, bad) |
           sgxd_usse_put(f.src2_bank, 28, 2, bad) | sgxd_usse_put(f.dest_n, 21, 7, bad) |
           sgxd_usse_put(f.src0_n, 14, 7, bad) | sgxd_usse_put(f.src1_n, 7, 7, bad) |
           sgxd_usse_put(f.src2_n, 0, 7, bad);
}

/* I32MAD: 32-bit integer multiply-add:
 * 10101pps-nrcdeba0tttif00yy000kgghhjjlllllllmmmmmmmoooooooqqqqqqq */
struct sgxd_usse_i32mad_fields {
    uint32_t pred, src0_high, nosched, src1_high, src2_high, dest_bank_ext, end, src1_bank_ext,
        src2_bank_ext, repeat_count, is_signed, is_sat, src2_type, src0_bank, dest_bank, src1_bank,
        src2_bank, dest_n, src0_n, src1_n, src2_n;
};
static inline uint64_t sgxd_usse_i32mad(struct sgxd_usse_i32mad_fields f, int* bad) {
    return UINT64_C(0xa800000000000000) | sgxd_usse_put(f.pred, 57, 2, bad) |
           sgxd_usse_put(f.src0_high, 56, 1, bad) | sgxd_usse_put(f.nosched, 54, 1, bad) |
           sgxd_usse_put(f.src1_high, 53, 1, bad) | sgxd_usse_put(f.src2_high, 52, 1, bad) |
           sgxd_usse_put(f.dest_bank_ext, 51, 1, bad) | sgxd_usse_put(f.end, 50, 1, bad) |
           sgxd_usse_put(f.src1_bank_ext, 49, 1, bad) | sgxd_usse_put(f.src2_bank_ext, 48, 1, bad) |
           sgxd_usse_put(f.repeat_count, 44, 3, bad) | sgxd_usse_put(f.is_signed, 43, 1, bad) |
           sgxd_usse_put(f.is_sat, 42, 1, bad) | sgxd_usse_put(f.src2_type, 38, 2, bad) |
           sgxd_usse_put(f.src0_bank, 34, 1, bad) | sgxd_usse_put(f.dest_bank, 32, 2, bad) |
           sgxd_usse_put(f.src1_bank, 30, 2, bad) | sgxd_usse_put(f.src2_bank, 28, 2, bad) |
           sgxd_usse_put(f.dest_n, 21, 7, bad) | sgxd_usse_put(f.src0_n, 14, 7, bad) |
           sgxd_usse_put(f.src1_n, 7, 7, bad) | sgxd_usse_put(f.src2_n, 0, 7, bad);
}

/* I32MAD2: 32-bit integer multiply-add, a 16-bit multiplier:
 * 11010ppp-nssdercbooo00iga0000kttffhhjjjjjjjlllllllmmmmmmmqqqqqqq */
struct sgxd_usse_i32mad2_fields {
    uint32_t pred, nosched, sn, dest_bank_ext, end, src1_bank_ext, src2_bank_ext, src0_bank_ext,
        count, is_signed, negative_src1, negative_src2, src0_bank, dest_bank, src1_bank, src2_bank,
        dest_n, src0_n, src1_n, src2_n;
};
static inline uint64_t sgxd_usse_i32mad2(struct sgxd_usse_i32mad2_fields f, int* bad) {
    return UINT64_C(0xd000000000000000) | sgxd_usse_put(f.pred, 56, 3, bad) |
           sgxd_usse_put(f.nosched, 54, 1, bad) | sgxd_usse_put(f.sn, 52, 2, bad) |
           sgxd_usse_put(f.dest_bank_ext, 51, 1, bad) | sgxd_usse_put(f.end, 50, 1, bad) |
           sgxd_usse_put(f.src1_bank_ext, 49, 1, bad) | sgxd_usse_put(f.src2_bank_ext, 48, 1, bad) |
           sgxd_usse_put(f.src0_bank_ext, 47, 1, bad) | sgxd_usse_put(f.count, 44, 3, bad) |
           sgxd_usse_put(f.is_signed, 41, 1, bad) | sgxd_usse_put(f.negative_src1, 40, 1, bad) |
           sgxd_usse_put(f.negative_src2, 39, 1, bad) | sgxd_usse_put(f.src0_bank, 34, 1, bad) |
           sgxd_usse_put(f.dest_bank, 32, 2, bad) | sgxd_usse_put(f.src1_bank, 30, 2, bad) |
           sgxd_usse_put(f.src2_bank, 28, 2, bad) | sgxd_usse_put(f.dest_n, 21, 7, bad) |
           sgxd_usse_put(f.src0_n, 14, 7, bad) | sgxd_usse_put(f.src1_n, 7, 7, bad) |
           sgxd_usse_put(f.src2_n, 0, 7, bad);
}

/* VTST: test, a predicate from an ALU result (bit 54, a don't-care in Vita3K's pattern, is its
 * nosched bit): 01001pppsKoydrceavttiizzmhhhnnbbkkffgggggggwlluuuujjjjjjjqqqqqqq */
struct sgxd_usse_vtst_fields {
    uint32_t pred, skipinv, nosched, onceonly, syncstart, dest_ext, src1_neg, src1_ext, src2_ext,
        prec, src2_vscomp, rpt_count, sign_test, zero_test, test_crcomb_and, chan_cc, pdst_n,
        dest_bank, src1_bank, src2_bank, dest_n, test_wben, alu_sel, alu_op, src1_n, src2_n;
};
static inline uint64_t sgxd_usse_vtst(struct sgxd_usse_vtst_fields f, int* bad) {
    return UINT64_C(0x4800000000000000) | sgxd_usse_put(f.pred, 56, 3, bad) |
           sgxd_usse_put(f.skipinv, 55, 1, bad) | sgxd_usse_put(f.nosched, 54, 1, bad) |
           sgxd_usse_put(f.onceonly, 53, 1, bad) | sgxd_usse_put(f.syncstart, 52, 1, bad) |
           sgxd_usse_put(f.dest_ext, 51, 1, bad) | sgxd_usse_put(f.src1_neg, 50, 1, bad) |
           sgxd_usse_put(f.src1_ext, 49, 1, bad) | sgxd_usse_put(f.src2_ext, 48, 1, bad) |
           sgxd_usse_put(f.prec, 47, 1, bad) | sgxd_usse_put(f.src2_vscomp, 46, 1, bad) |
           sgxd_usse_put(f.rpt_count, 44, 2, bad) | sgxd_usse_put(f.sign_test, 42, 2, bad) |
           sgxd_usse_put(f.zero_test, 40, 2, bad) | sgxd_usse_put(f.test_crcomb_and, 39, 1, bad) |
           sgxd_usse_put(f.chan_cc, 36, 3, bad) | sgxd_usse_put(f.pdst_n, 34, 2, bad) |
           sgxd_usse_put(f.dest_bank, 32, 2, bad) | sgxd_usse_put(f.src1_bank, 30, 2, bad) |
           sgxd_usse_put(f.src2_bank, 28, 2, bad) | sgxd_usse_put(f.dest_n, 21, 7, bad) |
           sgxd_usse_put(f.test_wben, 20, 1, bad) | sgxd_usse_put(f.alu_sel, 18, 2, bad) |
           sgxd_usse_put(f.alu_op, 14, 4, bad) | sgxd_usse_put(f.src1_n, 7, 7, bad) |
           sgxd_usse_put(f.src2_n, 0, 7, bad);
}

/* VLDST: load and store: 111oopppsnmycrbakkkkddeetgffihjlqquuvvvvvvvwwwwwwwxxxxxxxzzzzzzz */
struct sgxd_usse_vldst_fields {
    uint32_t op, pred, skipinv, nosched, moe_expand, sync_start, cache_ext, src0_bank_ext,
        src1_bank_ext, src2_bank_ext, mask_count, addr_mode, mode, dest_bank_primattr, range_enable,
        data_type, increment_or_decrement, src0_bank, cache_by_pass12, drc_sel, src1_bank,
        src2_bank, dest_n, src0_n, src1_n, src2_n;
};
static inline uint64_t sgxd_usse_vldst(struct sgxd_usse_vldst_fields f, int* bad) {
    return UINT64_C(0xe000000000000000) | sgxd_usse_put(f.op, 59, 2, bad) |
           sgxd_usse_put(f.pred, 56, 3, bad) | sgxd_usse_put(f.skipinv, 55, 1, bad) |
           sgxd_usse_put(f.nosched, 54, 1, bad) | sgxd_usse_put(f.moe_expand, 53, 1, bad) |
           sgxd_usse_put(f.sync_start, 52, 1, bad) | sgxd_usse_put(f.cache_ext, 51, 1, bad) |
           sgxd_usse_put(f.src0_bank_ext, 50, 1, bad) | sgxd_usse_put(f.src1_bank_ext, 49, 1, bad) |
           sgxd_usse_put(f.src2_bank_ext, 48, 1, bad) | sgxd_usse_put(f.mask_count, 44, 4, bad) |
           sgxd_usse_put(f.addr_mode, 42, 2, bad) | sgxd_usse_put(f.mode, 40, 2, bad) |
           sgxd_usse_put(f.dest_bank_primattr, 39, 1, bad) |
           sgxd_usse_put(f.range_enable, 38, 1, bad) | sgxd_usse_put(f.data_type, 36, 2, bad) |
           sgxd_usse_put(f.increment_or_decrement, 35, 1, bad) |
           sgxd_usse_put(f.src0_bank, 34, 1, bad) | sgxd_usse_put(f.cache_by_pass12, 33, 1, bad) |
           sgxd_usse_put(f.drc_sel, 32, 1, bad) | sgxd_usse_put(f.src1_bank, 30, 2, bad) |
           sgxd_usse_put(f.src2_bank, 28, 2, bad) | sgxd_usse_put(f.dest_n, 21, 7, bad) |
           sgxd_usse_put(f.src0_n, 14, 7, bad) | sgxd_usse_put(f.src1_n, 7, 7, bad) |
           sgxd_usse_put(f.src2_n, 0, 7, bad);
}

/* BR: branch: 11111ppps000e-----wynba00r----------------iloooooooooooooooooooo */
struct sgxd_usse_br_fields {
    uint32_t pred, syncend, exception, pwait, sync_ext, nosched, br_monitor, save_link, br_type,
        any_inst, all_inst, br_off;
};
static inline uint64_t sgxd_usse_br(struct sgxd_usse_br_fields f, int* bad) {
    return UINT64_C(0xf800000000000000) | sgxd_usse_put(f.pred, 56, 3, bad) |
           sgxd_usse_put(f.syncend, 55, 1, bad) | sgxd_usse_put(f.exception, 51, 1, bad) |
           sgxd_usse_put(f.pwait, 45, 1, bad) | sgxd_usse_put(f.sync_ext, 44, 1, bad) |
           sgxd_usse_put(f.nosched, 43, 1, bad) | sgxd_usse_put(f.br_monitor, 42, 1, bad) |
           sgxd_usse_put(f.save_link, 41, 1, bad) | sgxd_usse_put(f.br_type, 38, 1, bad) |
           sgxd_usse_put(f.any_inst, 21, 1, bad) | sgxd_usse_put(f.all_inst, 20, 1, bad) |
           sgxd_usse_put(f.br_off, 0, 20, bad);
}

/* PHAS: a phase's end, the next phase and its wait:
 * 11111010s100eirc--matwwwppppppppbbnn--------xxxxxxoooooooddddddd */
struct sgxd_usse_phas_fields {
    uint32_t sprvv, end, imm, src1_bank_ext, src2_bank_ext, mode, rate_hi, rate_lo_or_nosched,
        wait_cond, temp_count, src1_bank, src2_bank, exe_addr_high, src1_n_or_exe_addr_mid,
        src2_n_or_exe_addr_low;
};
static inline uint64_t sgxd_usse_phas(struct sgxd_usse_phas_fields f, int* bad) {
    return UINT64_C(0xfa40000000000000) | sgxd_usse_put(f.sprvv, 55, 1, bad) |
           sgxd_usse_put(f.end, 51, 1, bad) | sgxd_usse_put(f.imm, 50, 1, bad) |
           sgxd_usse_put(f.src1_bank_ext, 49, 1, bad) | sgxd_usse_put(f.src2_bank_ext, 48, 1, bad) |
           sgxd_usse_put(f.mode, 45, 1, bad) | sgxd_usse_put(f.rate_hi, 44, 1, bad) |
           sgxd_usse_put(f.rate_lo_or_nosched, 43, 1, bad) |
           sgxd_usse_put(f.wait_cond, 40, 3, bad) | sgxd_usse_put(f.temp_count, 32, 8, bad) |
           sgxd_usse_put(f.src1_bank, 30, 2, bad) | sgxd_usse_put(f.src2_bank, 28, 2, bad) |
           sgxd_usse_put(f.exe_addr_high, 14, 6, bad) |
           sgxd_usse_put(f.src1_n_or_exe_addr_mid, 7, 7, bad) |
           sgxd_usse_put(f.src2_n_or_exe_addr_low, 0, 7, bad);
}

/* SMLSI: the register increments and limits of a repeat:
 * 11111010--01-n--ttttppppssssdrcieeeeeeeeaaaaaaaabbbbbbbbffffffff */
struct sgxd_usse_smlsi_fields {
    uint32_t nosched, temp_limit, pa_limit, sa_limit, dest_inc_mode, src0_inc_mode, src1_inc_mode,
        src2_inc_mode, dest_inc, src0_inc, src1_inc, src2_inc;
};
static inline uint64_t sgxd_usse_smlsi(struct sgxd_usse_smlsi_fields f, int* bad) {
    return UINT64_C(0xfa10000000000000) | sgxd_usse_put(f.nosched, 50, 1, bad) |
           sgxd_usse_put(f.temp_limit, 44, 4, bad) | sgxd_usse_put(f.pa_limit, 40, 4, bad) |
           sgxd_usse_put(f.sa_limit, 36, 4, bad) | sgxd_usse_put(f.dest_inc_mode, 35, 1, bad) |
           sgxd_usse_put(f.src0_inc_mode, 34, 1, bad) | sgxd_usse_put(f.src1_inc_mode, 33, 1, bad) |
           sgxd_usse_put(f.src2_inc_mode, 32, 1, bad) | sgxd_usse_put(f.dest_inc, 24, 8, bad) |
           sgxd_usse_put(f.src0_inc, 16, 8, bad) | sgxd_usse_put(f.src1_inc, 8, 8, bad) |
           sgxd_usse_put(f.src2_inc, 0, 8, bad);
}

/* LIMM: a 32-bit immediate: 11111100sn10deiiiiiipppmmmmm--tt----uuuuuuuvvvvvvvvvvvvvvvvvvvvv */
struct sgxd_usse_limm_fields {
    uint32_t skipinv, nosched, dest_bank_ext, end, pred, dest_bank, dest_n, value;
};
static inline uint64_t sgxd_usse_limm(struct sgxd_usse_limm_fields f, int* bad) {
    return UINT64_C(0xfc20000000000000) | sgxd_usse_put(f.skipinv, 55, 1, bad) |
           sgxd_usse_put(f.nosched, 54, 1, bad) | sgxd_usse_put(f.dest_bank_ext, 51, 1, bad) |
           sgxd_usse_put(f.end, 50, 1, bad) | sgxd_usse_put(f.value >> 26, 44, 6, bad) |
           sgxd_usse_put(f.pred, 41, 3, bad) | sgxd_usse_put((f.value >> 21) & 0x1f, 36, 5, bad) |
           sgxd_usse_put(f.dest_bank, 32, 2, bad) | sgxd_usse_put(f.dest_n, 21, 7, bad) |
           sgxd_usse_put(f.value & 0x1fffff, 0, 21, bad);
}

/* The flow-control group (BR's 11111ppps000 head) beside BR: bits 40..38 the operation. */
static inline uint64_t sgxd_usse_nop(uint32_t nosched, uint32_t end, int* bad) {
    return UINT64_C(0xf800014000000000) | sgxd_usse_put(end, 50, 1, bad) |
           sgxd_usse_put(nosched, 43, 1, bad);
}
static inline uint64_t sgxd_usse_ret(uint32_t pred, int* bad) {
    return UINT64_C(0xf800008000000000) | sgxd_usse_put(pred, 56, 3, bad);
}
/* SETL and SAVL: the link register from and to a temporary. */
static inline uint64_t sgxd_usse_setl(uint32_t src_n, int* bad) {
    return UINT64_C(0xf80000c000000000) | sgxd_usse_put(src_n, 7, 7, bad);
}
static inline uint64_t sgxd_usse_savl(uint32_t dest_n, int* bad) {
    return UINT64_C(0xf800010000000000) | sgxd_usse_put(dest_n, 21, 7, bad);
}

/* IDF and WDF: mark the outstanding accesses on data counter drc, and wait for them. */
static inline uint64_t sgxd_usse_idf(uint32_t drc, int* bad) {
    return UINT64_C(0xf820000000000000) | sgxd_usse_put(drc, 32, 2, bad);
}
static inline uint64_t sgxd_usse_wdf(uint32_t drc, uint32_t nosched, int* bad) {
    return UINT64_C(0xf920000000000000) | sgxd_usse_put(nosched, 43, 1, bad) |
           sgxd_usse_put(drc, 32, 2, bad);
}

/* LOCK and RELEASE of a hardware mutex, and SETM, a monitor's set. */
static inline uint64_t sgxd_usse_lock(uint32_t mutex, uint32_t release, int* bad) {
    return UINT64_C(0xfd20000000000000) | sgxd_usse_put(mutex, 4, 4, bad) |
           sgxd_usse_put(release, 0, 1, bad);
}
static inline uint64_t sgxd_usse_setm(uint32_t monitor, int* bad) {
    return UINT64_C(0xfa20000000000000) | sgxd_usse_put(monitor, 0, 4, bad);
}

/*
 * LDR and STR: a control register by its index (the byte offset / 4, 18 bits) into or from a
 * register, or by an index in a register (the indirect forms). dest_pa: the destination is a
 * primary attribute, else a temporary. src: a source 1/2 operand's bank, extension and number;
 * short_pred: STR's always, p0, p1, !p0.
 */
static inline uint64_t sgxd_usse_ldr(uint32_t dest_pa, uint32_t dest_n, uint32_t index,
                                     uint32_t drc, int* bad) {
    return UINT64_C(0xfe21000000000000) | sgxd_usse_put(dest_pa, 39, 1, bad) |
           sgxd_usse_put(index >> 14, 34, 4, bad) | sgxd_usse_put(drc, 32, 2, bad) |
           UINT64_C(2) << 28 | sgxd_usse_put(dest_n, 21, 7, bad) |
           sgxd_usse_put((index >> 7) & 0x7f, 14, 7, bad) | sgxd_usse_put(index & 0x7f, 0, 7, bad);
}
static inline uint64_t sgxd_usse_str(uint32_t short_pred, uint32_t src_bank, uint32_t src_ext,
                                     uint32_t src_n, uint32_t index, int* bad) {
    return UINT64_C(0xfe29000000000000) | sgxd_usse_put(src_ext, 49, 1, bad) |
           sgxd_usse_put(short_pred, 41, 2, bad) | sgxd_usse_put(index >> 14, 34, 4, bad) |
           sgxd_usse_put(src_bank, 30, 2, bad) | UINT64_C(2) << 28 |
           sgxd_usse_put((index >> 7) & 0x7f, 14, 7, bad) | sgxd_usse_put(src_n, 7, 7, bad) |
           sgxd_usse_put(index & 0x7f, 0, 7, bad);
}
static inline uint64_t sgxd_usse_ldr_indirect(uint32_t dest_pa, uint32_t dest_n, uint32_t drc,
                                              uint32_t index_bank, uint32_t index_n, int* bad) {
    return UINT64_C(0xfe20000000000000) | sgxd_usse_put(dest_pa, 39, 1, bad) |
           sgxd_usse_put(drc, 32, 2, bad) | sgxd_usse_put(index_bank, 28, 2, bad) |
           sgxd_usse_put(dest_n, 21, 7, bad) | sgxd_usse_put(index_n, 0, 7, bad);
}
static inline uint64_t sgxd_usse_str_indirect(uint32_t src_bank, uint32_t src_ext, uint32_t src_n,
                                              uint32_t index_bank, uint32_t index_n, int* bad) {
    return UINT64_C(0xfe28000000000000) | sgxd_usse_put(src_ext, 49, 1, bad) |
           sgxd_usse_put(src_bank, 30, 2, bad) | sgxd_usse_put(index_bank, 28, 2, bad) |
           sgxd_usse_put(src_n, 7, 7, bad) | sgxd_usse_put(index_n, 0, 7, bad);
}

/*
 * EMIT to the PDS: a task for the data mover, its sources in register pairs (task, PDS words,
 * IR0), the 14-bit sideband split over three fields.
 */
static inline uint64_t sgxd_usse_emitpds(uint32_t task_pair, uint32_t pds_pair, uint32_t ir0_pair,
                                         uint32_t sideband, int* bad) {
    return UINT64_C(0xfb20000000000000) | UINT64_C(2) << 46 | UINT64_C(1) << 45 |
           UINT64_C(1) << 44 | sgxd_usse_put(sideband >> 12, 54, 2, bad) |
           sgxd_usse_put((sideband >> 6) & 0x3f, 35, 6, bad) |
           sgxd_usse_put(sideband & 0x3f, 22, 6, bad) | sgxd_usse_put(task_pair, 14, 7, bad) |
           sgxd_usse_put(pds_pair, 7, 7, bad) | sgxd_usse_put(ir0_pair, 0, 7, bad);
}

/* The vertex program's end: the compiler's emitvtx.end of the vertex's outputs. */
#define SGXD_USSE_EMITVTX_END UINT64_C(0xfb275000a0200000)

#endif
