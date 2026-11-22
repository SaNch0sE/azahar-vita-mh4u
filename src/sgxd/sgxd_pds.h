// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// sgxd_pds — the PDS instruction encoders sgxd_recipe and sgxd_program share, and their program
// form: two data segments, then code.

#ifndef SGXD_PDS_H
#define SGXD_PDS_H

#include <stdint.h>
#include <string.h>

// A program's room: words in each data segment, and instructions.
#define SGXD_PDS_MAX_DATA 32u
#define SGXD_PDS_MAX_CODE 32u

#define S1L 0u
#define S1H 1u
#define S2L 2u
#define S2H 3u
#define HALT 0xaf000000u

static inline uint32_t movs(unsigned dest, unsigned s1sel, unsigned s1, unsigned s2sel, unsigned s2,
                     unsigned w0, unsigned w1, unsigned w2, unsigned w3) {
    return 7u << 24 | s1sel << 23 | (s1 & 0x1fu) << 18 | s2sel << 17 | (s2 & 0x1fu) << 12 |
           w0 << 10 | w1 << 8 | w2 << 6 | w3 << 4 | dest;
}
static inline uint32_t doutd(unsigned p) { return movs(3, 0, p, 0, 24, S1L, S1H, S1L, S1H); }
static inline uint32_t douta(unsigned p) { return movs(6, 0, p, 0, 24, S1L, S1H, S2L, S2L); }
static inline uint32_t doutu(unsigned p, unsigned w2) {
    const unsigned s = w2 & 1 ? S2H : S2L;
    return movs(5, 0, p, 0, w2 / 2, S1L, S1H, s, s);
}
static inline uint32_t cc(uint32_t i, unsigned c) { return (i & ~(7u << 24)) | c << 24; }
static inline uint32_t movsa(uint32_t i) { return i | 5u << 27; }
static inline uint32_t mul(unsigned s1reg, unsigned s1, unsigned s2, unsigned d1, unsigned d) {
    return 1u << 30 | 4u << 27 | 7u << 24 | s1reg << 23 | s1 << 16 | s2 << 8 | d1 << 6 | d;
}
// DOUTI of DS1 word w whose USE iterator writes itr_words words: the MOVS's last two swizzle
// fields are TAGSIZE and ITRSIZE, each one less than the words.
static inline uint32_t douti(unsigned w, unsigned itr_words) {
    return (movs(2, 0, 1, 0, w / 2, w & 1 ? S2H : S2L, S1L, S1L, S1L) & ~0xf0u) |
           (itr_words - 1) << 4;
}
// DOUTT of the four words at data pair p of both segments (DS0 words 0 and 1, DS1 2 and 3), with
// FCONV fconv (0 unchanged, 2 F16, 3 F32): the MOVS's swizzle fields hold FCONV and MINPACK.
static inline uint32_t doutt(unsigned p, unsigned fconv) {
    return movs(4, 0, p, 0, p, S1L, S1L, S1L, S1L) | fconv << 4;
}
static inline uint32_t doutu0(unsigned cb, unsigned idx, unsigned tu) { return cb | idx << 4 | tu << 24; }
static inline uint32_t doutd1(unsigned pa, unsigned n) { return (n - 1) | pa << 8; }
static inline uint32_t douta1(unsigned pa) { return pa << 8; }

// A PDS program: two data segments of words, then code.
typedef struct Pds {
    uint32_t ds[2][SGXD_PDS_MAX_DATA];
    unsigned len[2];
    uint32_t code[SGXD_PDS_MAX_CODE];
    unsigned ncode;
} Pds;

static inline unsigned pds_word(Pds *p, unsigned b, uint32_t v) {
    p->ds[b][p->len[b]++] = v;
    return p->len[b] - 1;
}
static inline unsigned pds_pair(Pds *p, unsigned b, uint32_t lo, uint32_t hi) {
    if (p->len[b] & 1) {
        p->ds[b][p->len[b]++] = 0;
    }
    p->ds[b][p->len[b]++] = lo;
    p->ds[b][p->len[b]++] = hi;
    return p->len[b] / 2 - 1;
}
// Four words at one pair index of both segments, a DOUTT's: DS0 takes words 0 and 1, DS1 2 and 3.
static inline unsigned pds_quad(Pds *p, const uint32_t w[4]) {
    unsigned n = p->len[0] > p->len[1] ? p->len[0] : p->len[1];
    n = (n + 1) & ~1u;
    for (unsigned b = 0; b < 2; b++) {
        while (p->len[b] < n) {
            p->ds[b][p->len[b]++] = 0;
        }
        p->ds[b][p->len[b]++] = w[2 * b];
        p->ds[b][p->len[b]++] = w[2 * b + 1];
    }
    return n / 2;
}
static inline void pds_emit(Pds *p, uint32_t i) { p->code[p->ncode++] = i; }
static inline unsigned pds_units(const Pds *p) {
    const unsigned n = p->len[0] > p->len[1] ? p->len[0] : p->len[1];
    return (n + 1) / 2;
}
// Data units (DS0 w0, w1, DS1 w0, w1), then the code. Returns the words written.
static inline unsigned pds_words(const Pds *p, uint32_t *out) {
    const unsigned u = pds_units(p);
    memset(out, 0, u * 16);
    for (unsigned n = 0; n < p->len[0]; n++) {
        out[(n / 2) * 4 + n % 2] = p->ds[0][n];
    }
    for (unsigned n = 0; n < p->len[1]; n++) {
        out[(n / 2) * 4 + 2 + n % 2] = p->ds[1][n];
    }
    memcpy(out + u * 4, p->code, p->ncode * 4);
    return u * 4 + p->ncode;
}

// libgxm's CalculatePixelDMSInfo for one sample: the pixel
// task's size word from its PAs, temporaries and SA. 0x08018000 for up to 4 PAs (Sony's).
static inline uint32_t pixel_dms(unsigned pa, unsigned temps, unsigned sa) {
    const unsigned size = (pa + (temps + 1) / 2 * 2 + 3) / 4 * 4, regs = size * 4;
    const unsigned sa_chunks = (sa + 31) / 32;
    if (!regs) {
        return 3u << 15 | sa_chunks << 17;
    }
    const unsigned chunks = 0x900 / 32 - sa_chunks;
    const unsigned use_regs = (chunks - 2) / 3 * 32, quarter = (chunks - 4) / 4 * 32;
    unsigned use = use_regs / regs, pds;
    use = use > 4 ? 4 : use < 1 ? 1 : use;
    if (quarter < regs) {
        pds = 1;
    } else {
        const unsigned use_chunks = (regs * use + 31) / 32;
        const unsigned pds_chunks = chunks - 4 - 2 * (use_chunks - 1);
        const unsigned full = pds_chunks / use_chunks;
        unsigned limit = quarter / regs;
        limit = limit < 16 ? limit : 16;
        limit = limit < 4 * use ? limit : 4 * use;
        pds = use * full + (pds_chunks - use_chunks * full) * 32 / regs;
        pds = pds < limit ? pds : limit;
    }
    return (pds & 15) << 22 | (use - 1) << 15 | sa_chunks << 17 | (size >> 2) << 27;
}

// The vertex DMS: the VDM index list's words 2, 4 and 5.
static inline void vertex_dms(unsigned pa, unsigned temps, unsigned sa, unsigned out, uint32_t *w2,
                       uint32_t *w4, uint32_t *w5) {
    unsigned size = pa + (temps + 1) / 2 * 2;
    size = (size + 3) / 4 * 4;
    if (size && size % 16 == 0) {
        size += 4;
    }
    unsigned max_inst = 8;
    if (size) {
        int m = (int)(0x900 - 126 - (sa + 31) / 32 * 32) / 3 / (int)size;
        max_inst = m < 1 ? 1 : (m > 8 ? 8 : (unsigned)m);
    }
    out = (out + 3) / 4 * 4;
    unsigned part = out ? 128 / out : 16;
    part = part < 16 ? part : 16;
    part = part < max_inst ? part : max_inst;
    const unsigned adv = (part < 4 ? part : 4) - 1;
    *w2 = (max_inst - 1) << 28;
    *w4 = (part - 1) << 28;
    *w5 = (out >> 2) << 25 | adv << 23 | (size >> 2) << 9;
}

#endif
