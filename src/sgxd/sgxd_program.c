// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// sgxd_program — see sgxd_program.h.

#include "sgxd_program.h"

#include <string.h>

#include "sgxd_pds.h"

// DOUTU1's fields.
#define DOUTU1_PERINSTANCE (1u << 0)
#define DOUTU1_SDSOFT (1u << 1)
#define DOUTU1_ITERATORSDEPENDENCY (1u << 2)
#define DOUTU1_TEXTUREDEPENDENCY (1u << 3)
// The DS1 temporaries the vertex fetch's MULs write, a pair apart from word 50.
#define FETCH_TEMP 50u
// IF0: the DOUTU on the batch's last vertex.
#define CC_IF0 3u

static int finish(const Pds *p, SgxdPdsProgram *out) {
    const unsigned units = pds_units(p);
    if (units * 4 + p->ncode > SGXD_PROGRAM_MAX_PDS) {
        return -1;
    }
    out->count = pds_words(p, out->words);
    out->units = units;
    return 0;
}

uint32_t sgxd_program_doutu0(unsigned cb, uint32_t code_offset, unsigned temps) {
    return doutu0(cb, code_offset >> 3, (temps + 3) / 4);
}

int sgxd_program_vertex_pds(const SgxdVertexFetch *fetch, unsigned count, uint32_t kick0,
                            int per_instance, SgxdPdsProgram *out) {
    return sgxd_program_vertex_pds_at(fetch, count, kick0, per_instance, out, NULL);
}

int sgxd_program_vertex_pds_at(const SgxdVertexFetch *fetch, unsigned count, uint32_t kick0,
                               int per_instance, SgxdPdsProgram *out, unsigned *address_at) {
    uint16_t strides[SGXD_PROGRAM_MAX_STRIDES + 1] = {0};
    unsigned stride_of[SGXD_PROGRAM_MAX_FETCH], nstrides = 0;
    if (count > SGXD_PROGRAM_MAX_FETCH) {
        return -1;
    }
    for (unsigned i = 0; i < count; i++) {
        unsigned k = 0;
        while (k < nstrides && strides[k] != fetch[i].stride) {
            k++;
        }
        if (k == nstrides) {
            if (nstrides == SGXD_PROGRAM_MAX_STRIDES || fetch[i].stride > 0xffff) {
                return -1;
            }
            strides[nstrides++] = (uint16_t)fetch[i].stride;
        }
        if (fetch[i].words == 0 || fetch[i].words > 256) {
            return -1;
        }
        stride_of[i] = k;
    }
    Pds p;
    pds_init(&p);
    // DS1: word 0 strides 0 and 1, word 1 DOUTU word 2, words 2.. the other strides in pairs.
    pds_word(&p, 1, strides[0] | (uint32_t)strides[1] << 16);
    pds_word(&p, 1, 0);
    for (unsigned k = 2; k < nstrides; k += 2) {
        pds_word(&p, 1, strides[k] | (uint32_t)strides[k + 1] << 16);
    }
    unsigned pairs[SGXD_PROGRAM_MAX_FETCH];
    for (unsigned i = 0; i < count; i++) {
        pairs[i] = pds_pair(&p, 0, fetch[i].address, doutd1(fetch[i].pa, fetch[i].words));
        if (address_at) {
            address_at[i] = pairs[i] * 4;   // DS0's pair n: the first word of data unit n
        }
    }
    const unsigned kick = pds_pair(&p, 0, kick0, per_instance ? DOUTU1_PERINSTANCE : 0);
    // The MUL's second source counts 16-bit halves of DS1: word 0's two, then word 2's on.
    for (unsigned k = 0; k < nstrides; k++) {
        pds_emit(&p, mul(1, 0, k < 2 ? k : k + 2, 1, FETCH_TEMP + 2 * k));
    }
    for (unsigned i = 0; i < count; i++) {
        pds_emit(&p, movsa(movs(3, 0, pairs[i], 0, FETCH_TEMP / 2 + stride_of[i], S1L, S1H, S2L,
                                S2L)));
    }
    pds_emit(&p, cc(doutu(kick, 1), CC_IF0));
    pds_emit(&p, HALT);
    return finish(&p, out);
}

int sgxd_program_secondary_pds(uint32_t sa_address, unsigned sa_words, uint32_t kick0,
                               SgxdPdsProgram *out) {
    return sgxd_program_secondary_pds_at(sa_address, sa_words, kick0, out, NULL);
}

int sgxd_program_secondary_pds_at(uint32_t sa_address, unsigned sa_words, uint32_t kick0,
                                  SgxdPdsProgram *out, unsigned *address_at) {
    if (sa_words > 256) {
        return -1;
    }
    Pds p;
    pds_init(&p);
    const unsigned dma = sa_words ? pds_pair(&p, 0, sa_address, doutd1(0, sa_words)) : 0;
    if (address_at) {
        *address_at = dma * 4;
    }
    const unsigned kick = pds_pair(&p, 0, kick0, 0);
    pds_word(&p, 1, 0);   // DOUTU word 2
    if (sa_words) {
        pds_emit(&p, doutd(dma));
    }
    pds_emit(&p, doutu(kick, 0));
    pds_emit(&p, HALT);
    return finish(&p, out);
}

int sgxd_program_pixel_pds(uint32_t kick0, const uint32_t *iterators, const unsigned *words,
                           unsigned count, SgxdPdsProgram *out) {
    if (count > 12) {
        return -1;
    }
    Pds p;
    pds_init(&p);
    const unsigned kick =
        pds_pair(&p, 0, kick0, DOUTU1_SDSOFT | (count ? DOUTU1_ITERATORSDEPENDENCY : 0));
    pds_word(&p, 1, 0);   // DOUTU word 2
    pds_emit(&p, doutu(kick, 0));
    for (unsigned i = 0; i < count; i++) {
        if (words[i] == 0 || words[i] > 4) {
            return -1;
        }
        pds_emit(&p, douti(pds_word(&p, 1, iterators[i]), words[i]));
    }
    pds_emit(&p, HALT);
    return finish(&p, out);
}

// The blit's words: sgxd_recipe.c's (PHAS_LAST, NOP, END_VERTEX, NOP_END, SKIPINV, the copy
// form `or o, pa, #0` with a repeat, and IMGUI_FS_RAW(4)).
#define USSE_PHAS_LAST 0xfa44070000000000ull
#define USSE_NOP 0xf800094000000000ull
#define USSE_NOP_END 0xf804014000000000ull
#define USSE_END_VERTEX 0xfb275000a0200000ull
#define USSE_SKIPINV (0x00800000ull << 32)
// or o[d..d+n-1], pa[d..], #0
#define USSE_COPY(d, n) ((uint64_t)(0x50a10009u | ((n) - 1u) << 12) << 32 | (0xa0000000u | (d) << 21 | (d) << 7))
// The colour iterator (V0, four F32 lanes) and the texture issue at TC0 (sgxd_recipe.c's
// imgui_prim, SGXD_IMGUI_TEXEL).
#define BLIT_DOUTI_COLOUR SGXD_PROGRAM_BLIT_DOUTI_COLOUR
#define BLIT_DOUTI_TEX 0x0000f900u
// The merge's issues: TC0 (USEISSUE none, TEXPROJ RHW, TEXISSUE TC0), then TC1 with
// TEXLASTISSUE.
#define MERGE_DOUTI_TC0 0x0000f100u
#define MERGE_DOUTI_TC1 0x0000f901u

unsigned sgxd_program_blit_vertex(uint64_t words[4]) {
    words[0] = USSE_PHAS_LAST;
    words[1] = USSE_NOP;
    words[2] = USSE_COPY(0u, SGXD_PROGRAM_BLIT_VERTEX_WORDS);
    words[3] = USSE_END_VERTEX;
    return 4;
}

unsigned sgxd_program_blit_fragment(uint64_t words[4]) {
    words[0] = USSE_PHAS_LAST;
    words[1] = (0x50a10009a0000000ull | 4ull << 7) | USSE_SKIPINV;   // or o0, pa4, #0
    words[2] = USSE_NOP_END;
    return 3;
}

// pack.u8.f16.scl o0, pa4 (end): the patcher's output pack (40a40a3da01d8000, from pa0) with
// its source pair, USE0 bits 13..8 in register pairs, at 2.
#define USSE_PACK_PA4_TO_O0 0x40a40a3da01d8200ull

unsigned sgxd_program_screen_fragment(uint64_t words[4]) {
    words[0] = USSE_PHAS_LAST;
    words[1] = USSE_PACK_PA4_TO_O0;
    return 2;
}

unsigned sgxd_program_screen_blend_fragment(uint64_t words[6]) {
    words[0] = USSE_PHAS_LAST;
    words[1] = 0x40800d7ea0198002ull; // pack.f16.f32 pa0, pa0..pa3
    words[2] = 0x40a00a3ea01d8000ull; // pack.u8.f16.scl pa0, pa0
    words[3] = 0x40a00a3ea09d8200ull; // pack.u8.f16.scl pa4, pa4 (the line before's, USE0 bits
                                      // 13..8 at 2 and the destination at 4)
    words[4] = 0x80a00082a0000004ull; // sop2 pa0, pa0, pa4: SRC2, ZERO (pa0 * pa4 per byte)
    words[5] = 0x80848add90000000ull; // sop2 o0, pa0, o0: SRCALPHA, ONE_MINUS_SRCALPHA (end)
    return 6;
}

// The blit's pixel primary, with the texel converted by fconv into tag_words PA words.
static int textured_pixel_pds(uint32_t kick0, const uint32_t tex[4], unsigned fconv,
                              unsigned tag_words, SgxdPdsProgram *out) {
    Pds p;
    pds_init(&p);
    const unsigned kick = pds_pair(&p, 0, kick0,
                                   DOUTU1_SDSOFT | DOUTU1_ITERATORSDEPENDENCY |
                                       DOUTU1_TEXTUREDEPENDENCY);
    pds_word(&p, 1, 0);   // DOUTU word 2
    const unsigned colour = pds_word(&p, 1, BLIT_DOUTI_COLOUR);
    const unsigned issue = pds_word(&p, 1, BLIT_DOUTI_TEX);
    const unsigned quad = pds_quad(&p, tex);
    pds_emit(&p, doutu(kick, 0));
    pds_emit(&p, douti(colour, 4));
    // The DOUTI's TAGSIZE (MOVS bits 7..6) is one less than the texel's words.
    pds_emit(&p, douti(issue, 1) | (tag_words - 1) << 6);
    pds_emit(&p, doutt(quad, fconv));
    pds_emit(&p, HALT);
    return finish(&p, out);
}

int sgxd_program_blit_pixel_pds(uint32_t kick0, const uint32_t tex[4], SgxdPdsProgram *out) {
    return textured_pixel_pds(kick0, tex, 0, 1, out);
}

int sgxd_program_screen_pixel_pds(uint32_t kick0, const uint32_t tex[4], SgxdPdsProgram *out) {
    return textured_pixel_pds(kick0, tex, 2, 2, out);
}

// The texel's two words raw (FCONV 0) into pa0 and pa1, then the colour iterator into pa2-pa5.
// A pixel task starts at a USE issue with USELASTISSUE, which the colour's DOUTI carries: with
// texture issues alone the 3D never finishes. Issues fill the PAs in PDS order.
int sgxd_program_raw64_pixel_pds(uint32_t kick0, const uint32_t tex[4], SgxdPdsProgram *out) {
    Pds p;
    pds_init(&p);
    const unsigned kick = pds_pair(&p, 0, kick0,
                                   DOUTU1_SDSOFT | DOUTU1_ITERATORSDEPENDENCY |
                                       DOUTU1_TEXTUREDEPENDENCY);
    pds_word(&p, 1, 0);   // DOUTU word 2
    const unsigned issue = pds_word(&p, 1, BLIT_DOUTI_TEX);
    const unsigned colour = pds_word(&p, 1, BLIT_DOUTI_COLOUR);
    const unsigned quad = pds_quad(&p, tex);
    pds_emit(&p, doutu(kick, 0));
    pds_emit(&p, douti(issue, 1) | 1u << 6);   // TAGSIZE 1: two words
    pds_emit(&p, doutt(quad, 0));
    pds_emit(&p, douti(colour, 4));
    pds_emit(&p, HALT);
    return finish(&p, out);
}

unsigned sgxd_program_raw64_fragment(uint64_t words[2]) {
    words[0] = USSE_PHAS_LAST;
    words[1] = 0x3884054183000000ull;   // vmov.f32 o0.xy, pa0.xy (END)
    return 2;
}

unsigned sgxd_program_keep_fragment(uint64_t words[2]) {
    words[0] = USSE_PHAS_LAST;
    words[1] = USSE_NOP_END;
    return 2;
}

unsigned sgxd_program_merge_vertex(uint64_t words[4]) {
    words[0] = USSE_PHAS_LAST;
    words[1] = USSE_NOP;
    words[2] = USSE_COPY(0u, SGXD_PROGRAM_MERGE_VERTEX_WORDS);
    words[3] = USSE_END_VERTEX;
    return 4;
}

int sgxd_program_merge_pixel_pds(uint32_t kick0, const uint32_t tex0[4], const uint32_t tex1[4],
                                 SgxdPdsProgram *out) {
    Pds p;
    pds_init(&p);
    const unsigned kick = pds_pair(&p, 0, kick0,
                                   DOUTU1_SDSOFT | DOUTU1_ITERATORSDEPENDENCY |
                                       DOUTU1_TEXTUREDEPENDENCY);
    pds_word(&p, 1, 0);   // DOUTU word 2
    const unsigned colour = pds_word(&p, 1, BLIT_DOUTI_COLOUR);
    const unsigned issue0 = pds_word(&p, 1, MERGE_DOUTI_TC0);
    const unsigned issue1 = pds_word(&p, 1, MERGE_DOUTI_TC1);
    const unsigned quad0 = pds_quad(&p, tex0);
    const unsigned quad1 = pds_quad(&p, tex1);
    pds_emit(&p, doutu(kick, 0));
    pds_emit(&p, douti(colour, 4));
    pds_emit(&p, douti(issue0, 1));
    pds_emit(&p, doutt(quad0, 0));
    pds_emit(&p, douti(issue1, 1));
    pds_emit(&p, doutt(quad1, 0));
    pds_emit(&p, HALT);
    return finish(&p, out);
}

// A texture issue's DOUTI: TEXISSUE the coordinate set, USEISSUE none and TEXPROJ RHW, as the
// merge's MERGE_DOUTI_TC0 and TC1 (0x0000f100 | TCn), with TEXLASTISSUE (0x800) on the last.
// `proj_t`: TEXPROJ T (bits 9..8 = 2), the coordinate divided by its set's T: a projected read.
#define PICA_DOUTI_TEX(coord, last, proj_t) \
    (((proj_t) ? 0x0000f200u : 0x0000f100u) | (coord) | ((last) ? 0x800u : 0u))
#define PICA_TEXEL_WORDS 4u
#define PICA_FCONV_F32 3u
#define PICA_TEXEL_WORDS_F16 2u
#define PICA_FCONV_F16 2u
#define PICA_TEXEL_WORDS_RAW 1u
#define PICA_FCONV_RAW 0u

int sgxd_program_pica_pixel_pds(uint32_t kick0, const uint32_t *iterators, const unsigned *words,
                                unsigned count, const SgxdTextureIssue *issues,
                                unsigned issue_count, SgxdPdsProgram *out) {
    if (count > 12 || issue_count > 7) {
        return -1;
    }
    Pds p;
    pds_init(&p);
    const unsigned kick =
        pds_pair(&p, 0, kick0,
                 DOUTU1_SDSOFT | (count ? DOUTU1_ITERATORSDEPENDENCY : 0) |
                     (issue_count ? DOUTU1_TEXTUREDEPENDENCY : 0));
    pds_word(&p, 1, 0);   // DOUTU word 2
    pds_emit(&p, doutu(kick, 0));
    unsigned issued = 0;
    for (unsigned i = 0; i < count; i++) {
        if (words[i] == 0 || words[i] > 4) {
            return -1;
        }
        pds_emit(&p, douti(pds_word(&p, 1, iterators[i]), words[i]));
        for (unsigned k = 0; k < issue_count; k++) {
            if (issues[k].after != i) {
                continue;
            }
            const unsigned last = ++issued == issue_count;
            const unsigned issue =
                pds_word(&p, 1, PICA_DOUTI_TEX(issues[k].coord, last, issues[k].proj_t));
            const unsigned quad = pds_quad(&p, issues[k].tex);
            // The DOUTI's TAGSIZE (MOVS bits 7..6) is one less than the texel's words.
            const unsigned texel_words = issues[k].raw   ? PICA_TEXEL_WORDS_RAW
                                         : issues[k].f16 ? PICA_TEXEL_WORDS_F16
                                                         : PICA_TEXEL_WORDS;
            const unsigned fconv = issues[k].raw   ? PICA_FCONV_RAW
                                   : issues[k].f16 ? PICA_FCONV_F16
                                                   : PICA_FCONV_F32;
            pds_emit(&p, douti(issue, 1) | (texel_words - 1u) << 6);
            pds_emit(&p, doutt(quad, fconv));
        }
    }
    if (issued != issue_count) {
        return -1;   // an issue after an iterator that is not there
    }
    pds_emit(&p, HALT);
    return finish(&p, out);
}

uint32_t sgxd_program_pixel_dms(unsigned pa, unsigned temps, unsigned sa) {
    return pixel_dms(pa, temps, sa);
}

void sgxd_program_vertex_dms(unsigned pa, unsigned temps, unsigned sa, unsigned out,
                             uint32_t *w2, uint32_t *w4, uint32_t *w5) {
    vertex_dms(pa, temps, sa, out, w2, w4, w5);
}
