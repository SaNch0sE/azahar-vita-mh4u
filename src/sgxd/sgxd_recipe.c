// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// sgxd_recipe — see sgxd_recipe.h.

#include "sgxd_recipe.h"

#include <string.h>

#include "sgxd_pds.h"

const char *const sgxd_recipe_names[SGXD_R_COUNT] = {
    "PT", "PAGES", "RGN0", "RGN1", "RGN2", "RGN3", "EXTZ", "TPC0", "TPC1", "TPC2", "TPC3",
    "BGREC", "DPMSLOT", "FB", "DEPTH", "STREAM", "STATE", "VERTS", "INDICES", "PDS", "COPY", "VSPDS",
    "VSEC", "TERM", "FSEC", "FPRIM", "BGSEC", "BGPRIM", "PIXEV", "IDLE", "COPY1", "COPY2",
    "COPY3", "COPY4", "COPY5", "COPY6", "COPY7", "CV", "CF", "END",
};

// ---------------------------------------------------------------------------- the scene

// The one triangle in clip space (x, y, z, w), indices 0, 1, 2.
#define NV 4u
static const float triangle_verts[3 * NV] = {
    -0.5f, -0.5f, 0.25f, 1.0f, 0.5f, -0.5f, 0.5f, 1.0f, 0.0f, 0.5f, 0.75f, 1.0f,
};
static const uint16_t triangle_indices[3] = {0, 1, 2};
static const SgxdRecipeDraw triangle_draw = {0, 3, SGXD_RECIPE_ISP_A, SGXD_R_FPRIM, 0, 0, NULL};

#define OUTSELECTS (4u << 24 | 0x1000u)
// VTXSIZE 8: the position and the base colour (bit 11, the TSP data's base colour).
#define IMGUI_OUTSELECTS (8u << 24 | 0x1000u | 0x800u)

static uint32_t fbits(float x) {
    uint32_t bits;
    memcpy(&bits, &x, 4);
    return bits;
}

static unsigned floor_log2(uint32_t v) {
    unsigned n = 0;
    while (v >>= 1) {
        n++;
    }
    return n;
}

// ---------------------------------------------------------------------------- USSE

#define PHAS_LAST 0xfa44070000000000ull
#define NOP 0xf800094000000000ull
#define NOP_END 0xf804014000000000ull
#define PHAS_END 0xfa4c070000000000ull
#define END_VERTEX 0xfb275000a0200000ull
#define EMITMTE 0xfb2f4000a0200000ull
#define EMITPIX_EOT 0xfb24000001200082ull
#define EMITPIX_EOR 0xfb26000081200000ull
#define SKIPINV (0x00800000ull << 32)
#define LIMM_O (1ull << 32)   // LIMM destination bank o
#define CF_EOT_OFFSET 0x100u  // the end-of-tile program in CF

static const uint64_t brn[4] = {
    0xf800094000000000ull, 0x48830280a00c0000ull, 0xe9833084a0000000ull, 0xf920000000000000ull,
};

// limm rDEST, value.
static uint64_t usse_limm(unsigned dest, uint32_t value) {
    const uint32_t hi = 0xfc200000u | ((value >> 21) & 0x1fu) << 4 | ((value >> 26) & 0x3fu) << 12;
    const uint32_t lo = (dest & 0x7fu) << 21 | (value & 0x1fffffu);
    return (uint64_t)hi << 32 | lo;
}

typedef struct Code {
    uint32_t offset;   // from the window
    unsigned count;
    uint64_t words[16];
} Code;

static void code_add(Code *c, uint64_t word) {
    c->words[c->count++] = word;
}

// The state copy: o[d..] = pa[d..], 16 to an instruction, then EMITMTE.
static void copyprog(Code *c, uint32_t offset, unsigned n) {
    c->offset = offset;
    c->count = 0;
    code_add(c, PHAS_LAST);
    for (unsigned i = 0; i < 4; i++) {
        code_add(c, brn[i]);
    }
    for (unsigned d = 0; d < n; d += 16) {
        const unsigned r = n - d < 16 ? n - d : 16;
        code_add(c, (uint64_t)(0x50a10009u | (r - 1) << 12) << 32 | (0xa0000000u | d << 21 | d << 7));
    }
    code_add(c, EMITMTE);
}

static void colour_fs(Code *c, uint32_t offset, uint32_t colour) {
    c->offset = offset;
    c->count = 0;
    code_add(c, PHAS_LAST);
    code_add(c, usse_limm(0, colour) | LIMM_O | SKIPINV);
    code_add(c, NOP_END);
}

enum { CV_DUMMY_VS, CV_COPY_STATE, CV_COPY_TERM, CV_VS, CV_COUNT };
enum { CF_DUMMY_FS, CF_BG_FS, CF_EOR, CF_PTOFF, CF_EOT, CF_TRI_FS, CF_COUNT };

// Step E2's vertex program over an ImDrawVert in pa0..pa4 and the scale and offset in sa0..sa3:
// o0, o1 = position * scale + offset, o2 = 0.5, o3 = 1.0, o4..o7 = the colour's four bytes / 255
// (the base colour): vnmad, vunpck_u8 and limm with skipinv (a mask-3
// VPCK U8-to-F32 writes two registers, docs/sgxd/usse.md), here into o. Temporaries
// r0, r1.
#define IMGUI_VS_TEMPS 2u
static const uint64_t imgui_vs[] = {
    0x08a44184b0040000ull,   // mul r0.xy, pa0.xy, sa0.xy
    0x08a4418530041001ull,   // add o0.xy, r0.xy, sa2.xy
    0x4080018da09d8200ull,   // unpck.u8.scl o4.xy, pa4 bytes 0 1
    0x4080018da0df8202ull,   // unpck.u8.scl o6.xy, pa4 bytes 2 3
    0xfc20f18100400000ull,   // limm o2, 0.5
    0xfc20f1c100600000ull,   // limm o3, 1.0
};
// SGXD_IMGUI_COLOUR_UV (sgxd_test 07) also copies the UV to o8, o9, the recipe's own copy form
// (copyprog's OR with a repeat), as TC0.
#define IMGUI_VS_UV 0x50a11009a1000100ull   // or o8..o9, pa2..pa3 (repeat 2)
// Its fragment program there: r and g of the iterated colour (pa0, pa1) and the iterated u and v
// (pa4, pa5) as four unorm bytes into o0, one scaled VPCK F32 to U8 (its rounding and saturation
// in docs/sgxd/usse.md), then the end.
#define IMGUI_FS_UV 0x40800c3da01d8004ull   // pack.u8.f32.scl o0, pa0.xy pa4.xy
// SGXD_IMGUI_UV_RAW + n: or o0, pa n, #0, the copy form with one word. SGXD_IMGUI_TEXEL copies
// the texel from pa4 so.
#define IMGUI_FS_RAW(n) (0x50a10009a0000000ull | (uint64_t)(n) << 7)
// SGXD_IMGUI_TEXEL's texture issue: USEISSUE NONE, TEXISSUE TC0, TEXPROJ RHW, TEXLASTISSUE
// (issues fill the PA words in PDS order, docs/sgxd/pds.md).
#define IMGUI_DOUTI_TEX 0x0000f900u

static int imgui_uv(const SgxdRecipe *r) {
    return (r->imgui_fs >= SGXD_IMGUI_COLOUR_UV && r->imgui_fs <= SGXD_IMGUI_TEXEL) ||
           r->imgui_fs == SGXD_IMGUI_MODULATE;
}
// The modes that sample texture 0 into pa4.
static int imgui_texel(const SgxdRecipe *r) {
    return r->imgui_fs == SGXD_IMGUI_TEXEL || r->imgui_fs == SGXD_IMGUI_MODULATE;
}
// A PDS part's room in the layout: 0x40 bytes, TERM and PIXEV 0x80, and FPRIM 0x80 in the
// texture modes, whose FPRIM carries the texture's four words (17 words with them; in 0x40 its
// last word would land on BGSEC's DOUTD address when sgxd_recipe_set_texture rewrites it).
static uint32_t pds_slot(const SgxdRecipe *r, unsigned part) {
    return part == SGXD_R_TERM || part == SGXD_R_PIXEV ||
           (part == SGXD_R_FPRIM && imgui_texel(r)) ? 0x80u : 0x40u;
}

// Its fragment program, the pixel's iterated base colour (four F32 lanes in pa0..pa3) to o0 as
// four unorm bytes: a pack of F32 to F16 into pa0 and one of F16 to U8 scaled into o0, with END.
static const uint64_t imgui_fs[] = {
    0x40800d7ea0198002ull,   // pack.f16.f32 pa0, pa0..pa3
    0x40a40a3da01d8000ull,   // pack.u8.f16.scl o0, pa0 (end)
};
// SGXD_IMGUI_BLEND's (sgxd_test 09): the same packs into pa0, then the blend over the output
// buffer's o0 (the pixel's current value), one SOP2: colour = pa0 * pa0.a
// + o0 * (1 - pa0.a), alpha = pa0.a + o0.a * (1 - pa0.a), ends the program.
static const uint64_t imgui_fs_blend[] = {
    0x40800d7ea0198002ull,   // pack.f16.f32 pa0, pa0..pa3
    0x40a00a3ea01d8000ull,   // pack.u8.f16.scl pa0, pa0
    0x80848add90000000ull,   // sop2 o0, pa0, o0: SRCALPHA, ONE_MINUS_SRCALPHA (end)
};
// SGXD_IMGUI_MODULATE's (sgxd_test 12): SGXD_IMGUI_BLEND's packs, then texel * colour as one SOP2
// (CSEL1 SRC2, CSEL2 ZERO, ASEL1 SRC2ALPHA, ASEL2 ZERO, ADD;
// SKIPINV; source 1 pa0, source 2 pa4, destination pa0), then E5's blend.
static const uint64_t imgui_fs_modulate[] = {
    0x40800d7ea0198002ull,   // pack.f16.f32 pa0, pa0..pa3
    0x40a00a3ea01d8000ull,   // pack.u8.f16.scl pa0, pa0
    0x80a00082a0000004ull,   // sop2 pa0, pa0, pa4: SRC2, ZERO (pa0 * pa4 per byte)
    0x80848add90000000ull,   // sop2 o0, pa0, o0: SRCALPHA, ONE_MINUS_SRCALPHA (end)
};

static void make_cv(const SgxdRecipe *r, Code cv[CV_COUNT]) {
    memset(cv, 0, sizeof(Code) * CV_COUNT);
    cv[CV_DUMMY_VS].offset = 0x00;
    code_add(&cv[CV_DUMMY_VS], PHAS_END);
    copyprog(&cv[CV_COPY_STATE], 0x40, 24);
    copyprog(&cv[CV_COPY_TERM], 0x80, 2);
    cv[CV_VS].offset = 0xc0;
    if (r->imgui) {
        code_add(&cv[CV_VS], PHAS_LAST);
        code_add(&cv[CV_VS], NOP);
        for (unsigned i = 0; i < sizeof(imgui_vs) / sizeof(imgui_vs[0]); i++) {
            code_add(&cv[CV_VS], imgui_vs[i]);
        }
        if (imgui_uv(r)) {
            code_add(&cv[CV_VS], IMGUI_VS_UV);
        }
        code_add(&cv[CV_VS], END_VERTEX);
        return;
    }
    if (r->copy_vs) {
        // A caller's scene: o[n] = pa[n] for every vertex word, 16 to an instruction.
        code_add(&cv[CV_VS], PHAS_LAST);
        code_add(&cv[CV_VS], NOP);
        for (unsigned d = 0; d < NV; d += 16) {
            const unsigned n = NV - d < 16 ? NV - d : 16;
            code_add(&cv[CV_VS],
                     (uint64_t)(0x50a10009u | (n - 1) << 12) << 32 | (0xa0000000u | d << 21 | d << 7));
        }
        code_add(&cv[CV_VS], END_VERTEX);
        return;
    }
    // o0..o2 = pa0..pa2, o3 = c2 = 1.0, vertex end (a pass-through).
    static const uint64_t vs[] = {
        PHAS_LAST, NOP, 0x0881008190000002ull, 0x0885010190248002ull, 0x0881008190400042ull,
        0x0883010150400082ull, END_VERTEX,
    };
    for (unsigned i = 0; i < sizeof(vs) / sizeof(vs[0]); i++) {
        code_add(&cv[CV_VS], vs[i]);
    }
}

// Draw n's state copy: COPY for the first, COPY1.. for the others.
static unsigned copy_part(unsigned n) { return n ? SGXD_R_COPY1 + n - 1 : SGXD_R_COPY; }
// Its offset: COPY1.. are consecutive, and the draws past COPY7 follow it.
static uint32_t copy_offset(const SgxdRecipe *r, unsigned n) {
    return n ? r->offset[SGXD_R_COPY1] + (n - 1) * 0x40 : r->offset[SGXD_R_COPY];
}


static void make_cf(const SgxdRecipe *r, Code cf[CF_COUNT]) {
    memset(cf, 0, sizeof(Code) * CF_COUNT);
    cf[CF_DUMMY_FS].offset = 0x00;
    code_add(&cf[CF_DUMMY_FS], PHAS_LAST);
    for (unsigned i = 0; i < 4; i++) {
        code_add(&cf[CF_DUMMY_FS], brn[i]);
    }
    code_add(&cf[CF_DUMMY_FS], NOP_END);
    // Each colour is its own shader: a pixel task's PAs are per instance, and without SKIPINV a
    // quad's uncovered instances would write o0 too.
    colour_fs(&cf[CF_BG_FS], 0x40, SGXD_RECIPE_BG);
    cf[CF_EOR].offset = 0x80;
    code_add(&cf[CF_EOR], PHAS_LAST);
    code_add(&cf[CF_EOR], 0xfca0000000000000ull);
    code_add(&cf[CF_EOR], 0xfca0000000200000ull);
    code_add(&cf[CF_EOR], EMITPIX_EOR);
    cf[CF_PTOFF].offset = 0xc0;
    code_add(&cf[CF_PTOFF], PHAS_END);
    // End of tile: r0..r5 = the PBE words, EMITPIX.
    cf[CF_EOT].offset = CF_EOT_OFFSET;
    code_add(&cf[CF_EOT], PHAS_LAST);
    for (unsigned i = 0; i < 6; i++) {
        code_add(&cf[CF_EOT], usse_limm(i, r->pbe[i]) | SKIPINV);
    }
    code_add(&cf[CF_EOT], EMITPIX_EOT);
    if (r->imgui) {
        cf[CF_TRI_FS].offset = 0x140;
        code_add(&cf[CF_TRI_FS], PHAS_LAST);
        if (r->imgui_fs == SGXD_IMGUI_MODULATE) {
            for (unsigned i = 0; i < sizeof(imgui_fs_modulate) / sizeof(imgui_fs_modulate[0]);
                 i++) {
                code_add(&cf[CF_TRI_FS], imgui_fs_modulate[i]);
            }
            return;
        }
        if (imgui_uv(r)) {
            code_add(&cf[CF_TRI_FS], (r->imgui_fs == SGXD_IMGUI_TEXEL ? IMGUI_FS_RAW(4) :
                                      r->imgui_fs >= SGXD_IMGUI_UV_RAW ?
                                      IMGUI_FS_RAW(r->imgui_fs - SGXD_IMGUI_UV_RAW) :
                                      IMGUI_FS_UV) | SKIPINV);
            code_add(&cf[CF_TRI_FS], NOP_END);
            return;
        }
        if (r->imgui_fs == SGXD_IMGUI_BLEND) {
            for (unsigned i = 0; i < sizeof(imgui_fs_blend) / sizeof(imgui_fs_blend[0]); i++) {
                code_add(&cf[CF_TRI_FS], imgui_fs_blend[i]);
            }
            return;
        }
        for (unsigned i = 0; i < sizeof(imgui_fs) / sizeof(imgui_fs[0]); i++) {
            code_add(&cf[CF_TRI_FS], imgui_fs[i]);
        }
        return;
    }
    colour_fs(&cf[CF_TRI_FS], 0x140, SGXD_RECIPE_TRI);
}

#define VI(cv, n) ((cv)[n].offset >> 3)
#define FI(cf, n) ((cf)[n].offset >> 3)

// ---------------------------------------------------------------------------- PDS

static void secondary(Pds *p, uint32_t dw0) {
    memset(p, 0, sizeof(*p));
    const unsigned k = pds_pair(p, 0, dw0, 0);
    pds_word(p, 1, 0);
    pds_emit(p, doutu(k, 0));
    pds_emit(p, HALT);
}

// DOUTU only, issued first. The unused first pair keeps the data
// at two units, the size the TA state points at.
static void colour_prim(Pds *p, unsigned idx) {
    memset(p, 0, sizeof(*p));
    pds_pair(p, 0, 0, 0);
    const unsigned i = pds_pair(p, 0, doutu0(1, idx, 0), 2);
    pds_word(p, 1, 0);
    pds_emit(p, doutu(i, 0));
    pds_emit(p, HALT);
}

// ImGui mode's FPRIM: DOUTU, then the base colour's iterator into pa0..pa3: DOUTI 0x0ed0a00f,
// (V0, USECOLFLOAT, four F32 lanes, Gouraud, linear), ITRSIZE 4. With the UV, TC0's iterator
// follows into pa4, pa5 (two F32 lanes, ITRSIZE 2) and takes USELASTISSUE (bit 25) from V0's. In
// SGXD_IMGUI_TEXEL mode the texture issue follows V0's instead, then the DOUTT of tex, the texel
// unconverted into pa4 (TAGSIZE 1): DOUTI then DOUTT.
static void imgui_prim(const SgxdRecipe *r, const Code *cf, Pds *p, const uint32_t tex[4]) {
    memset(p, 0, sizeof(*p));
    const int texel = imgui_texel(r), uv = imgui_uv(r) && !texel;
    // DOUTU1: SDSOFT, ITERATORSDEPENDENCY and, with a texture issue, TEXTUREDEPENDENCY (bit 3).
    // Without it the task starts before the sample lands: pa4 then holds stale words.
    const unsigned k = pds_pair(p, 0, doutu0(1, FI(cf, CF_TRI_FS), 0),
                                1u << 1 | 1u << 2 | (texel ? 1u << 3 : 0));
    pds_word(p, 1, 0);
    const unsigned w = pds_word(p, 1, uv ? 0x0cd0a00fu : 0x0ed0a00fu);
    const unsigned t = uv ? pds_word(p, 1, 0x0e40000fu) : texel ? pds_word(p, 1, IMGUI_DOUTI_TEX) : 0;
    const unsigned q = texel ? pds_quad(p, tex) : 0;
    pds_emit(p, doutu(k, 0));
    pds_emit(p, douti(w, 4));
    if (uv || texel) {
        pds_emit(p, douti(t, uv ? 2 : 1));
    }
    if (texel) {
        pds_emit(p, doutt(q, 0));
    }
    pds_emit(p, HALT);
}

// State words 10 and 11, the region clip: MODE OUTSIDE (2), left
// and right tiles in the first, top and bottom in the second, inclusive; clip 0 is the target.
static uint32_t clip_x(const SgxdRecipe *r, uint32_t clip) {
    return 2u << 30 | (clip ? (clip >> 24 & 0x3f) << 16 | (clip >> 16 & 0x3f) : r->tx - 1);
}
static uint32_t clip_y(const SgxdRecipe *r, uint32_t clip) {
    return clip ? (clip >> 8 & 0x3f) << 16 | (clip & 0x3f) : r->ty - 1;
}

// Draw n's state copy: DOUTD of its 24 state words, then the copy program.
static void copy_pds(const SgxdRecipe *r, const Code *cv, unsigned n, Pds *p) {
    memset(p, 0, sizeof(*p));
    const unsigned k = pds_pair(p, 0, sgxd_recipe_va(r, SGXD_R_STATE) + n * 0x60, doutd1(0, 24));
    const unsigned i = pds_pair(p, 0, doutu0(0, VI(cv, CV_COPY_STATE), 0), 0);
    pds_word(p, 1, 0);
    pds_emit(p, doutd(k));
    pds_emit(p, doutu(i, 0));
    pds_emit(p, HALT);
}

// The PDS programs COPY .. BGPRIM, by layout part.
static void make_pds(const SgxdRecipe *r, const Code *cv, const Code *cf, Pds *pds) {
    Pds *p;
    // The state copies of the first eight draws (the rest are written with the scene).
    for (unsigned n = 0; n < r->ndraws && n < 8; n++) {
        copy_pds(r, cv, n, &pds[copy_part(n)]);
    }
    // The vertex fetch: ds1[50] = IR0L * stride, then a MOVSA DOUTD of the vertex and DOUTU (IF0).
    p = &pds[SGXD_R_VSPDS];
    memset(p, 0, sizeof(*p));
    {
        const unsigned words = r->vertex_words;
        pds_word(p, 1, words * 4);
        pds_word(p, 1, 0);
        const unsigned k = pds_pair(p, 0, sgxd_recipe_va(r, SGXD_R_VERTS), doutd1(0, words));
        const unsigned kick = pds_pair(p, 0, doutu0(0, VI(cv, CV_VS), r->imgui ? 1 : 0), 0);
        pds_emit(p, mul(1, 0, 0, 1, 50));
        pds_emit(p, movsa(movs(3, 0, k, 0, 25, S1L, S1H, S2L, S2L)));
        pds_emit(p, cc(doutu(kick, 1), 3));
        pds_emit(p, HALT);
    }
    if (r->imgui) {
        // The vertex program's scale and offset: four words into sa0..sa3, then the dummy
        // program.
        p = &pds[SGXD_R_VSEC];
        memset(p, 0, sizeof(*p));
        const unsigned k = pds_pair(p, 0, r->base + r->vertex_sa, doutd1(0, 4));
        const unsigned i = pds_pair(p, 0, doutu0(0, VI(cv, CV_DUMMY_VS), 0), 0);
        pds_word(p, 1, 0);
        pds_emit(p, doutd(k));
        pds_emit(p, doutu(i, 0));
        pds_emit(p, HALT);
    } else {
        secondary(&pds[SGXD_R_VSEC], doutu0(0, VI(cv, CV_DUMMY_VS), 0));
    }
    // The terminate state: PA0 = 0x2000 (TERMINATE present), PA1..3 = TARNDCLIP.
    p = &pds[SGXD_R_TERM];
    memset(p, 0, sizeof(*p));
    {
        const uint32_t region = (r->tx - 1) << 16 | (r->ty - 1);
        const unsigned k = pds_pair(p, 0, 0x2000, douta1(0));
        const unsigned n = pds_pair(p, 0, region, douta1(1));
        const unsigned i = pds_pair(p, 0, doutu0(0, VI(cv, CV_COPY_TERM), 0), 0);
        pds_word(p, 1, 0);
        pds_emit(p, douta(k));
        pds_emit(p, douta(n));
        pds_emit(p, douta(n));
        pds_emit(p, douta(n));
        pds_emit(p, doutu(i, 0));
        pds_emit(p, HALT);
    }
    secondary(&pds[SGXD_R_FSEC], doutu0(1, FI(cf, CF_DUMMY_FS), 0));
    if (r->imgui) {
        static const uint32_t no_texture[4] = {0};
        imgui_prim(r, cf, &pds[SGXD_R_FPRIM], no_texture);
    } else {
        colour_prim(&pds[SGXD_R_FPRIM], FI(cf, CF_TRI_FS));
    }
    secondary(&pds[SGXD_R_BGSEC], doutu0(1, FI(cf, CF_DUMMY_FS), 0));
    colour_prim(&pds[SGXD_R_BGPRIM], FI(cf, CF_BG_FS));
}

// The pixel event (end of tile, end of render, PT off), and the idle core-0 event program.
static unsigned make_pixev(const Code *cf, uint32_t *out) {
    const uint32_t words[] = {
        doutu0(1, FI(cf, CF_EOT), 2), 0, doutu0(1, FI(cf, CF_EOR), 1), 0,
        doutu0(1, FI(cf, CF_PTOFF), 0), 0, 0, 0xfffeffffu, 0x100, 0x30000, 0x20000, 0,
        0x93800003u, 0x07020185u, 0xaf000000u, 0x9480000bu, 0xcf088230u,
        0x87600000u, 0xcf800630u, 0xc7600830u, 0x10000a60u, 0x07600b05u,
        0xaf000000u, 0x07041185u, 0xaf000000u,
    };
    memcpy(out, words, sizeof(words));
    return sizeof(words) / 4;
}

static unsigned make_idle(const Code *cv, uint32_t *out) {
    const uint32_t words[] = {doutu0(0, VI(cv, CV_DUMMY_VS), 0), 1, 0, 0, 0x07000185u, 0xaf000000u};
    memcpy(out, words, sizeof(words));
    return sizeof(words) / 4;
}

// ---------------------------------------------------------------------------- layout

// A draw init and sgxd_recipe_set_draws accept: first a multiple of 8, its indices inside nindices.
// A draw with programs and its own index list is checked against nothing but its count.
static int draw_valid(const SgxdRecipeDraw *d, unsigned nindices) {
    const int own_indices = d->programs && d->programs->indices;
    return (own_indices || (!(d->first & 7) && d->first <= nindices &&
                            d->count <= nindices - d->first)) &&
           d->count && (d->programs || d->fprim == SGXD_R_FPRIM || d->fprim == SGXD_R_BGPRIM) &&
           d->cull <= 2 && !(d->clip && (d->clip & 0x40404040u));
}

static int init(SgxdRecipe *r, uint32_t width, uint32_t height, uint32_t base, uint32_t pages,
                const float *verts, const uint32_t *imgui_verts, unsigned nverts,
                const uint16_t *indices, unsigned nindices, const SgxdRecipeDraw *draws,
                unsigned ndraws, int copy_vs, uint32_t vertex_sa, unsigned imgui_fs, int programs);

// A draw's VDM stream words: the state copy and the index list, and in ImGui and program mode
// the vertex secondary between them, which loads the program's SA.
static unsigned draw_words(const SgxdRecipe *r) { return r->imgui || r->programs ? 10 : 8; }

unsigned sgxd_recipe_grid;
uint32_t sgxd_recipe_pixelbe = 0x80;

int sgxd_recipe_init(SgxdRecipe *r, uint32_t width, uint32_t height, uint32_t base,
                     uint32_t pages) {
    return init(r, width, height, base, pages, triangle_verts, NULL, 3, triangle_indices, 3,
                &triangle_draw, 1, 0, 0, 0, 0);
}

int sgxd_recipe_init_scene(SgxdRecipe *r, uint32_t width, uint32_t height, uint32_t base,
                           uint32_t pages, const float *verts, unsigned nverts,
                           const uint16_t *indices, unsigned nindices,
                           const SgxdRecipeDraw *draws, unsigned ndraws) {
    return init(r, width, height, base, pages, verts, NULL, nverts, indices, nindices, draws,
                ndraws, 1, 0, 0, 0);
}

int sgxd_recipe_init_programs(SgxdRecipe *r, uint32_t width, uint32_t height, uint32_t base,
                              uint32_t pages, const uint16_t *indices, unsigned nindices,
                              const SgxdRecipeDraw *draws, unsigned ndraws) {
    // VERTS holds the recipe's triangle for a draw without programs; nothing else reads it.
    return init(r, width, height, base, pages, triangle_verts, NULL, 3, indices, nindices, draws,
                ndraws, 1, 0, 0, 1);
}

int sgxd_recipe_init_imgui(SgxdRecipe *r, uint32_t width, uint32_t height, uint32_t base,
                           uint32_t pages, const uint32_t *verts, unsigned nverts,
                           const uint16_t *indices, unsigned nindices,
                           const SgxdRecipeDraw *draws, unsigned ndraws, uint32_t sa_offset,
                           unsigned fs) {
    if ((sa_offset & 15) || fs > SGXD_IMGUI_MODULATE) {
        return -1;
    }
    return init(r, width, height, base, pages, NULL, verts, nverts, indices, nindices, draws,
                ndraws, 0, sa_offset, fs, 0);
}

static int init(SgxdRecipe *r, uint32_t width, uint32_t height, uint32_t base, uint32_t pages,
                const float *verts, const uint32_t *imgui_verts, unsigned nverts,
                const uint16_t *indices, unsigned nindices, const SgxdRecipeDraw *draws,
                unsigned ndraws, int copy_vs, uint32_t vertex_sa, unsigned imgui_fs, int programs) {
    if (width < 32 || height < 32 || width > 2048 || height > 2048 || (base & 0xfffffu) ||
        pages < 64 || pages > 0x8000 || !nverts || nverts > 0xffff || !nindices ||
        nindices > SGXD_RECIPE_MAX_INDICES || !ndraws || ndraws > SGXD_RECIPE_MAX_DRAWS) {
        return -1;
    }
    for (unsigned n = 0; n < ndraws; n++) {
        if (!draw_valid(&draws[n], nindices)) {
            return -1;
        }
    }
    memset(r, 0, sizeof(*r));
    r->programs = programs;
    r->draws = draws;
    r->ndraws = ndraws;
    r->copy_vs = copy_vs;
    r->width = width;
    r->height = height;
    r->base = base;
    r->pages = pages;
    r->verts = verts;
    r->imgui_verts = imgui_verts;
    r->imgui = imgui_verts != NULL;
    r->vertex_words = r->imgui ? SGXD_RECIPE_IMGUI_WORDS : NV;
    r->vertex_sa = vertex_sa;
    r->imgui_fs = imgui_fs;
    r->nverts = nverts;
    r->indices = indices;
    r->nindices = nindices;
    const uint32_t W = width, H = height;

    r->tx = (W + 31) / 32;
    r->ty = (H + 31) / 32;
    r->count = sgxd_recipe_grid ? sgxd_recipe_grid : W * H < 0x4b000 ? 2 : 4;
    if (r->count == 2) {
        r->mx = ((r->tx + 1) / 2 + 3) & ~3u;
        r->my = ((r->ty + 1) / 2 + 3) & ~3u;
        r->mtile1 = r->mx << 22 | r->mx << 12 | r->mx;
        r->mtile2 = r->my << 22 | r->my << 12 | r->my;
    } else {
        // The 4x4 grid (960x544, Sony's display form).
        r->mx = ((r->tx + 3) / 4 + 3) & ~3u;
        r->my = ((r->ty + 3) / 4 + 3) & ~3u;
        r->mtile1 = 1u << 31 | r->mx << 22 | 2 * r->mx << 12 | 3 * r->mx;
        r->mtile2 = r->my << 22 | 2 * r->my << 12 | 3 * r->my;
    }
    r->stride = r->mx * r->my;
    r->te_screen = (r->ty - 1) << 12 | (r->tx - 1);
    r->mte_screen = (H - 1) << 12 | (W - 1);
    r->rgn_size = (r->count * r->count * r->mx * r->my * 12 + 63) & ~63u;
    uint32_t pp = 1;
    const uint32_t cm = r->count * (r->mx > r->my ? r->mx : r->my);
    while (pp < cm) {
        pp *= 2;
    }
    r->tpc_size = (pp * pp * 8 + 63) & ~63u;

    uint32_t *L = r->offset;
    uint32_t o;
    // The shared buffer's recipes have no page table or pages of their own.
    const int own_pb = sgxd_recipe_pb.last == 0;
    L[SGXD_R_PT] = 0;
    L[SGXD_R_PAGES] = own_pb ? 0x40000 : 0;
    o = own_pb ? 0x40000 + pages * 0x1000 : 0;
    for (unsigned c = 0; c < 4; c++) {
        L[SGXD_R_RGN0 + c] = o;
        o += r->rgn_size;
    }
    L[SGXD_R_EXTZ] = o;
    o += r->rgn_size;
    for (unsigned c = 0; c < 4; c++) {
        L[SGXD_R_TPC0 + c] = o;
        o += r->tpc_size;
    }
    L[SGXD_R_BGREC] = o;
    o += 128;
    // DPM_STATE_TABLE_BASE: 16-byte aligned, the slot's 34 words.
    L[SGXD_R_DPMSLOT] = o;
    o += 256;
    o = (o + 0xfff) & ~0xfffu;
    L[SGXD_R_FB] = o;
    o += sgxd_recipe_external_fb ? 0 : W * H * 4;
    L[SGXD_R_DEPTH] = o;
    o += sgxd_recipe_external_fb ? 0 : W * H * 4;
    L[SGXD_R_STREAM] = o;
    {
        const uint32_t s = ((draw_words(r) * ndraws + 5) * 4 + 0xff) & ~0xffu;
        o += s > 0x200 ? s : 0x200;
    }
    L[SGXD_R_STATE] = o;
    {
        const uint32_t s = (ndraws * 0x60 + 0xff) & ~0xffu;
        o += s > 0x100 ? s : 0x100;
    }
    uint32_t index_offset = (nverts * r->vertex_words * 4 + 0x3f) & ~0x3fu;
    index_offset = index_offset > 0x40 ? index_offset : 0x40;
    L[SGXD_R_VERTS] = o;
    L[SGXD_R_INDICES] = o + index_offset;
    {
        const uint32_t s = (index_offset + (nindices + 1) / 2 * 4 + 0xff) & ~0xffu;
        o += s > 0x100 ? s : 0x100;
    }
    L[SGXD_R_PDS] = o;
    for (unsigned s = SGXD_R_COPY; s <= SGXD_R_IDLE; s++) {
        L[s] = o;
        o += pds_slot(r, s);
    }
    // The other draws' state copies, consecutive; the unused parts of COPY1..COPY7 take no room.
    for (unsigned n = 1; n < 8; n++) {
        L[copy_part(n)] = o + (n < ndraws ? n - 1 : ndraws - 1) * 0x40;
    }
    o += (ndraws - 1) * 0x40;
    o = (o + 0xfff) & ~0xfffu;
    L[SGXD_R_CV] = o;
    o += 0x400;
    L[SGXD_R_CF] = o;
    o += 0x400;
    L[SGXD_R_END] = o;

    r->shared_pb = !own_pb;
    r->fb_external = sgxd_recipe_external_fb;
    if (r->shared_pb) {
        r->req_base = sgxd_recipe_pb.req_base;
        r->pt = sgxd_recipe_pb.pt;
        r->first = sgxd_recipe_pb.first;
        r->last = sgxd_recipe_pb.last;
    } else {
        r->req_base = base;
        r->pt = sgxd_recipe_va(r, SGXD_R_PT);
        r->first = (sgxd_recipe_va(r, SGXD_R_PAGES) - base) / 0x1000;
        r->last = r->first + pages - 1;
    }
    // The BGREC tag is 24 bits of 16 bytes from the 3D's requestor base.
    if (sgxd_recipe_va(r, SGXD_R_END) - r->req_base > 0x10000000u) {
        return -1;
    }
    const uint32_t pb_pages = r->last - r->first + 1;
    r->ta_free_list = r->last << 16 | r->first;
    r->ta_alloc = (r->last - 1) << 16;
    r->ta_threshold = pb_pages + 16;
    const uint32_t gbl = pb_pages - 34;
    const uint32_t mn = r->ta_threshold < gbl ? r->ta_threshold : gbl;
    r->pds_threshold = mn > 256 ? mn - 256 : 0;
    r->global_list = 0x10000 | gbl;
    r->bgobjtag = (((sgxd_recipe_va(r, SGXD_R_BGREC) - r->req_base) >> 4) & 0xffffffu) | 2u << 24;
    r->bgobjdepth = fbits(1.0f);
    r->bgobj = 0x300;

    r->pbe[0] = SGXD_RECIPE_PACK_U8888;
    r->pbe[1] = sgxd_recipe_va(r, SGXD_R_FB);
    r->pbe[2] = (W - 1) >> 1;
    r->pbe[3] = 0;
    r->pbe[4] = floor_log2((W + 15) / 16) << 24 | floor_log2((H + 15) / 16) << 28;
    r->pbe[5] = (W - 1) | (H - 1) << 12;

    Code cv[CV_COUNT], cf[CF_COUNT];
    Pds pds[SGXD_R_COUNT];
    make_cv(r, cv);
    make_cf(r, cf);
    make_pds(r, cv, cf, pds);
    for (unsigned s = SGXD_R_COPY; s <= SGXD_R_BGPRIM; s++) {
        r->units[s] = pds_units(&pds[s]);
    }
    for (unsigned n = 1; n < ndraws && n < 8; n++) {
        r->units[copy_part(n)] = pds_units(&pds[copy_part(n)]);
    }
    if (r->imgui) {
        vertex_dms(SGXD_RECIPE_IMGUI_WORDS, IMGUI_VS_TEMPS, 4,
                   imgui_uv(r) ? 10 : 8, &r->vdm_w2, &r->vdm_w4,
                   &r->vdm_w5);
    } else {
        vertex_dms(NV, 0, 0, NV, &r->vdm_w2, &r->vdm_w4, &r->vdm_w5);
    }
    return 0;
}

// ---------------------------------------------------------------------------- memory

// A PDS pointer word: units << 27 | va >> 4.
static uint32_t ptr(const SgxdRecipe *r, unsigned part) {
    return r->units[part] << 27 | ((sgxd_recipe_va(r, part) >> 4) & 0x07ffffffu);
}
static uint32_t msbs(const SgxdRecipe *r, unsigned sec, unsigned pri) {
    return (sgxd_recipe_va(r, sec) & 0x80000000u ? 1u << 14 : 0) |
           (sgxd_recipe_va(r, pri) & 0x80000000u ? 1u << 13 : 0);
}

static void put(uint8_t *memory, uint32_t offset, const uint32_t *words, unsigned count) {
    memcpy(memory + offset, words, count * 4);
}

static void fill(uint8_t *memory, uint32_t offset, uint32_t value, uint32_t bytes) {
    uint32_t *w = (uint32_t *)(memory + offset);
    for (uint32_t n = 0; n < bytes / 4; n++) {
        w[n] = value;
    }
}

// The DPM page table's free list: page p links p - 1 and p + 1, 0xffff at both ends.
static void page_list(uint32_t *pt, uint32_t first, uint32_t last) {
    for (uint32_t p = first; p <= last; p++) {
        const uint32_t prev = p != first ? p - 1 : 0xffff;
        const uint32_t next = p != last ? p + 1 : 0xffff;
        pt[p] = prev << 16 | next;
    }
}

// A recipe's own page list, rewritten for every pass: without the shared buffer each TA loads
// the whole list again. The shared buffer's is the DPM's once written.
static void put_page_list(const SgxdRecipe *r, uint8_t *memory) {
    if (!r->shared_pb) {
        page_list((uint32_t *)(memory + r->offset[SGXD_R_PT]), r->first, r->last);
    }
}

SgxdRecipePb sgxd_recipe_pb;
int sgxd_recipe_external_fb;
// The shared buffer's free lists are loaded in the DPM: the TA's, the 3D's (build order).
static int pb_ta_loaded, pb_3d_loaded;

void sgxd_recipe_pb_write(void *pt_memory) {
    page_list(pt_memory, sgxd_recipe_pb.first, sgxd_recipe_pb.last);
    sgxd_recipe_pb_reset();
}

void *sgxd_recipe_pb_memory;

void sgxd_recipe_pb_rewrite(void) {
    if (sgxd_recipe_pb_memory && sgxd_recipe_pb.last != 0) {
        sgxd_recipe_pb_write(sgxd_recipe_pb_memory);
    }
}

void sgxd_recipe_pb_reset(void) {
    pb_ta_loaded = 0;
    pb_3d_loaded = 0;
}

void sgxd_recipe_restage(const SgxdRecipe *r, void *memory_) {
    uint8_t *memory = memory_;
    const uint32_t *L = r->offset;
    put_page_list(r, memory);
    // Nothing for the region arrays, EXTZ or the tail pointer caches: TE_RGNHDR_INIT writes
    // every region header at each TA's start (T3 waits for it in every core), and the tail
    // pointer caches and EXTZ, zeroed when the recipe is written, are reused (zeroed once, at
    // creation).
    if (!r->fb_external) {
        fill(memory, L[SGXD_R_FB], SGXD_RECIPE_SENTINEL, r->width * r->height * 4);
        fill(memory, L[SGXD_R_DEPTH], SGXD_RECIPE_SENTINEL, r->width * r->height * 4);
    }
}

void sgxd_recipe_write_scene(const SgxdRecipe *r, void *memory_) {
    uint8_t *memory = memory_;
    uint32_t *verts = (uint32_t *)(memory + r->offset[SGXD_R_VERTS]);
    for (unsigned n = 0; n < r->nverts * r->vertex_words; n++) {
        verts[n] = r->imgui ? r->imgui_verts[n] : fbits(r->verts[n]);
    }
    uint32_t *words = (uint32_t *)(memory + r->offset[SGXD_R_INDICES]);
    for (unsigned n = 0; n < r->nindices; n += 2) {
        words[n / 2] = r->indices[n] | (n + 1 < r->nindices ? (uint32_t)r->indices[n + 1] << 16 : 0);
    }
}

void sgxd_recipe_set_target(SgxdRecipe *r, void *memory_, uint32_t fb_va, uint32_t stride_px) {
    uint8_t *memory = memory_;
    r->fb_external = 1;
    r->pbe[1] = fb_va;
    r->pbe[2] = (stride_px - 1) >> 1;
    // The end-of-tile program's LIMMs of words 1 and 2, after its PHAS.
    uint64_t *eot = (uint64_t *)(memory + r->offset[SGXD_R_CF] + CF_EOT_OFFSET);
    for (unsigned i = 1; i <= 2; i++) {
        eot[1 + i] = usse_limm(i, r->pbe[i]) | SKIPINV;
    }
}

void sgxd_recipe_set_target_pack(SgxdRecipe *r, void *memory_, uint32_t pbe0) {
    uint8_t *memory = memory_;
    r->pbe[0] = pbe0;
    uint64_t *eot = (uint64_t *)(memory + r->offset[SGXD_R_CF] + CF_EOT_OFFSET);
    eot[1] = usse_limm(0, r->pbe[0]) | SKIPINV;
}

void sgxd_recipe_set_background_depth(SgxdRecipe *r, void *memory, float z) {
    r->bgobjdepth = fbits(z);
    uint32_t *bg = (uint32_t *)((uint8_t *)memory + r->offset[SGXD_R_BGREC]);
    bg[12] = bg[14] = bg[16] = fbits(z);
}

void sgxd_recipe_set_background_stencil(SgxdRecipe *r, uint8_t stencil) {
    r->bgobj = (r->bgobj & ~0xffu) | stencil;
}

void sgxd_recipe_set_zls(SgxdRecipe *r, uint32_t depth, uint32_t stencil, int load, int store) {
    memset(r->zls, 0, sizeof(r->zls));
    if (depth == 0) {
        return;
    }
    // ZLSCTL: F32Z (format 0) or I24ZI8S (1, load format bits 23..21) loads, F32Z stores,
    // linear, ZLSEXTENT + 1 tiles a row, EXTERNALZBUFFER (bit 20); ZLOADEN (14) and FORCEZLOAD
    // (1), ZSTOREEN (18) and FORCEZSTORE (2), and for the stencil SLOADEN (13) and SSTOREEN
    // (17). The bases are offsets from BIF_ZLS_REQ_BASE, which the kick sets to req_base; the
    // stencil's carry ENABLE (bit 0). I24ZI8S loads its stencil from the word, so
    // STENCIL_LOAD_BASE stays off.
    const uint32_t extent = (r->width + 31) / 32 - 1;
    uint32_t ctl = 1u << 20 | extent << 3;
    if (load) {
        ctl |= 1u << 14 | 1u << 1 | (stencil ? 1u << 13 : 0) | (load == 2 ? 1u << 21 : 0);
    }
    if (store) {
        ctl |= 1u << 18 | 1u << 2 | (stencil ? 1u << 17 : 0);
    }
    r->zls[0] = ctl;
    r->zls[1] = r->zls[2] = depth - r->req_base;
    if (stencil) {
        r->zls[3] = load == 2 ? 0 : (stencil - r->req_base) | 1u;
        r->zls[4] = (stencil - r->req_base) | 1u;
    }
}

void sgxd_recipe_write_vertex_sa(const SgxdRecipe *r, void *memory_, const float sa[4]) {
    uint32_t *words = (uint32_t *)((uint8_t *)memory_ + r->vertex_sa);
    for (unsigned n = 0; n < 4; n++) {
        words[n] = fbits(sa[n]);
    }
}

// State word 8 of a draw: the colour+UV program's six PAs need two size units.
static uint32_t pixel_size_word(const SgxdRecipe *r, unsigned fprim) {
    if (fprim == SGXD_R_FPRIM && imgui_uv(r)) {
        return pixel_dms(6, 0, 0);
    }
    return 0x08018000u;
}

// State word 19: VTXSIZE 4, the position; ImGui mode adds the base colour (bit 11) and with the
// UV TC0's two words (TEXSIZE 1 in word 22).
static uint32_t outselects(const SgxdRecipe *r) {
    if (!r->imgui) {
        return OUTSELECTS;
    }
    return imgui_uv(r) ? 10u << 24 | 0x1000u | 0x800u : IMGUI_OUTSELECTS;
}

// The vertex secondary's VDM word: its units, all pipes, SECONDARY, and USEATTRIBUTESIZE in
// 4-word units: the ImGui program's four SA words.
static uint32_t vsec_word(const SgxdRecipe *r) {
    return r->units[SGXD_R_VSEC] << 27 | 7u << 13 | 1u << 8 | (r->imgui ? 1u : 0u);
}

// Draw n's MTE state packet, and in the VDM control stream its state copy and index list. A draw
// with programs takes its PDS pointers, sizes, viewport, output selects and index list from them.
static void write_draw(const SgxdRecipe *r, uint8_t *memory, unsigned n, const SgxdRecipeDraw *d) {
    const uint32_t *L = r->offset;
    const uint32_t W = r->width, H = r->height;
    const SgxdRecipeDrawPrograms *g = d->programs;
    uint32_t *stream = (uint32_t *)(memory + L[SGXD_R_STREAM]);
    // Every copy program has COPY's shape and units.
    const uint32_t copy_va = r->base + copy_offset(r, n);
    const uint32_t clip = d->clip;
    const unsigned fprim = g ? SGXD_R_FPRIM : d->fprim;
    uint32_t state[24] = {
        0xdfff, d->isp, 0, d->ispc ? d->ispc : SGXD_RECIPE_ISP_C, d->isp, 0,
        d->ispc ? d->ispc : SGXD_RECIPE_ISP_C, ptr(r, SGXD_R_FSEC),
        pixel_size_word(r, fprim) | msbs(r, SGXD_R_FSEC, fprim), ptr(r, fprim),
        clip_x(r, clip), clip_y(r, clip),
        fbits(W / 2.0f), fbits(W / 2.0f), fbits(H / 2.0f), fbits(H / 2.0f), fbits(0.5f),
        fbits(0.5f), 0, outselects(r), 0x3727c5ac, 0x8000 | d->cull,
        imgui_uv(r) ? 1u : 0u, 0,
    };
    if (g) {
        state[7] = g->fsec_units << 27 | ((g->fsec >> 4) & 0x07ffffffu);
        state[8] = g->pixel_size | (g->fsec & 0x80000000u ? 1u << 14 : 0) |
                   (g->fprim & 0x80000000u ? 1u << 13 : 0);
        state[9] = g->fprim_units << 27 | ((g->fprim >> 4) & 0x07ffffffu);
        for (unsigned i = 0; i < 6; i++) {
            state[12 + i] = fbits(g->viewport[i]);
        }
        state[19] = g->outselects;
        state[22] = g->texsize;
    }
    put(memory, L[SGXD_R_STATE] + n * 0x60, state, 24);
    uint32_t *w = stream + draw_words(r) * n;
    *w++ = 1u << 30 | copy_va >> 4;
    *w++ = r->units[SGXD_R_COPY] << 27 | 1u << 25 | 1u << 17 | (1u + n % 4) << 13 | 1u << 9 | 6;
    if (g) {
        // USEATTRIBUTESIZE in four-word units.
        *w++ = 1u << 30 | g->vsec >> 4;
        *w++ = g->vsec_units << 27 | 7u << 13 | 1u << 8 | (g->vsec_sa + 3) / 4;
    } else if (r->imgui || r->programs) {
        *w++ = 1u << 30 | sgxd_recipe_va(r, SGXD_R_VSEC) >> 4;
        *w++ = vsec_word(r);
    }
    *w++ = 2u << 30 | (g ? g->primitive : 0u) << 26 | 7u << 22 | d->count;
    *w++ = g && g->indices ? g->indices : sgxd_recipe_va(r, SGXD_R_INDICES) + 2 * d->first;
    *w++ = g ? g->vdm_w2 : r->vdm_w2;
    *w++ = 0;
    if (g) {
        *w++ = g->vdm_w4 | ((g->vspds >> 4) & 0x0fffffffu);
        *w++ = g->vdm_w5 | 0x100 | g->vspds_units;
    } else {
        *w++ = r->vdm_w4 | ((sgxd_recipe_va(r, SGXD_R_VSPDS) >> 4) & 0x0fffffffu);
        *w++ = r->vdm_w5 | 0x100 | r->units[SGXD_R_VSPDS];
    }
}

// The stream's end after count draws: the secondary and the terminate.
static void write_stream_end(const SgxdRecipe *r, uint8_t *memory, unsigned count) {
    uint32_t *stream = (uint32_t *)(memory + r->offset[SGXD_R_STREAM]);
    const uint32_t end[5] = {
        1u << 30 | sgxd_recipe_va(r, SGXD_R_VSEC) >> 4, vsec_word(r),
        1u << 30 | 1u << 29 | sgxd_recipe_va(r, SGXD_R_TERM) >> 4,
        r->units[SGXD_R_TERM] << 27 | 1u << 25 | 1u << 17 | 1u << 13 | 1u << 9 | 1,
        0xc0000000u,
    };
    memcpy(stream + draw_words(r) * count, end, sizeof(end));
}

int sgxd_recipe_set_draws(const SgxdRecipe *r, void *memory, const SgxdRecipeDraw *draws,
                          unsigned count) {
    if (!count || count > r->ndraws) {
        return -1;
    }
    for (unsigned n = 0; n < count; n++) {
        if (!draw_valid(&draws[n], r->nindices)) {
            return -1;
        }
    }
    for (unsigned n = 0; n < count; n++) {
        write_draw(r, memory, n, &draws[n]);
    }
    write_stream_end(r, memory, count);
    return 0;
}

void sgxd_recipe_write(const SgxdRecipe *r, void *memory_) {
    uint8_t *memory = memory_;
    const uint32_t *L = r->offset;
    const uint32_t W = r->width, H = r->height;
    memset(memory, 0, L[SGXD_R_END]);
    if (!r->shared_pb) {
        fill(memory, L[SGXD_R_PAGES], SGXD_RECIPE_SENTINEL, r->pages * 0x1000);
    }
    // The region arrays start with the sentinel, which shows a header the TE's init did not write.
    fill(memory, L[SGXD_R_RGN0], SGXD_RECIPE_SENTINEL, L[SGXD_R_EXTZ] - L[SGXD_R_RGN0]);
    sgxd_recipe_restage(r, memory);

    Code cv[CV_COUNT], cf[CF_COUNT];
    Pds pds[SGXD_R_COUNT];
    make_cv(r, cv);
    make_cf(r, cf);
    make_pds(r, cv, cf, pds);

    for (unsigned n = 0; n < r->ndraws; n++) {
        write_draw(r, memory, n, &r->draws[n]);
    }
    write_stream_end(r, memory, r->ndraws);

    sgxd_recipe_write_scene(r, memory);

    // The background object: its PDS pointers, then (0, 0), (W, 0), (0, H) as (x + 1024) * 16.
    const uint32_t bg[17] = {
        ptr(r, SGXD_R_BGSEC), 1u << 27 | 3u << 15 | msbs(r, SGXD_R_BGSEC, SGXD_R_BGPRIM),
        ptr(r, SGXD_R_BGPRIM), 0, 0, fbits(1.0f), 0, 0, fbits(1.0f), 2, 0,
        0x4000u << 16 | 0x4000, fbits(1.0f), ((W + 0x400) * 0x10) << 16 | 0x4000, fbits(1.0f),
        0x4000u << 16 | (H + 0x400) * 0x10, fbits(1.0f),
    };
    put(memory, L[SGXD_R_BGREC], bg, 17);

    uint32_t words[64];
    for (unsigned s = SGXD_R_COPY; s <= SGXD_R_BGPRIM; s++) {
        put(memory, L[s], words, pds_words(&pds[s], words));
    }
    for (unsigned n = 1; n < r->ndraws; n++) {
        Pds copy;
        copy_pds(r, cv, n, &copy);
        put(memory, copy_offset(r, n), words, pds_words(&copy, words));
    }
    put(memory, L[SGXD_R_PIXEV], words, make_pixev(cf, words));
    put(memory, L[SGXD_R_IDLE], words, make_idle(cv, words));
    for (unsigned n = 0; n < CV_COUNT; n++) {
        memcpy(memory + L[SGXD_R_CV] + cv[n].offset, cv[n].words, cv[n].count * 8);
    }
    for (unsigned n = 0; n < CF_COUNT; n++) {
        memcpy(memory + L[SGXD_R_CF] + cf[n].offset, cf[n].words, cf[n].count * 8);
    }
}

#define SA_BG_FS 0x180u   // CF offset of o0 = sa0

// or o0, sa0, #0 (SKIPINV): the recipe's o0 = pa0 with source bank 3, SA.
static void put_sa_shader(const SgxdRecipe *r, uint8_t *memory) {
    const uint64_t shader[3] = {PHAS_LAST, 0x50a10009e0000000ull | SKIPINV, NOP_END};
    memcpy(memory + r->offset[SGXD_R_CF] + SA_BG_FS, shader, sizeof(shader));
}

// xor o0, sa0, #imm: the or above as opcode 11 (OP2 bit 35 clear), the
// immediate in bits 6..0, 20..14 and 37..36.
void sgxd_recipe_sa_xor(const SgxdRecipe *r, void *memory, uint32_t imm) {
    const uint64_t xor = 0x58a10001e0000000ull | SKIPINV | (imm & 0x7fu) |
                         (uint64_t)(imm >> 7 & 0x7fu) << 14 | (uint64_t)(imm >> 14 & 3u) << 36;
    const uint64_t shader[3] = {PHAS_LAST, xor, NOP_END};
    memcpy((uint8_t *)memory + r->offset[SGXD_R_CF] + SA_BG_FS, shader, sizeof(shader));
}

// A secondary that DMAs the word at base + colour_offset into sa0: DS0 pair 0 = DOUTD of one word
// to AO 0, DS1 pair 0 = DOUTU2 0, DS0 pair 1 = the dummy DOUTU. Two data units.
static void put_sa_secondary(const SgxdRecipe *r, uint8_t *memory, uint32_t at,
                             uint32_t colour_offset) {
    const uint32_t secondary[11] = {
        r->base + colour_offset, doutd1(0, 1), 0, 0, doutu0(1, 0, 0), 0, 0, 0,
        doutd(0), doutu(1, 0), HALT,
    };
    put(memory, at, secondary, 11);
}

void sgxd_recipe_sa_background(const SgxdRecipe *r, void *memory_, uint32_t colour_offset,
                               uint32_t colour) {
    uint8_t *memory = memory_;
    const uint32_t *L = r->offset;
    put_sa_shader(r, memory);
    *(uint32_t *)(memory + colour_offset) = colour;
    put_sa_secondary(r, memory, L[SGXD_R_BGSEC], colour_offset);
    uint32_t *bg = (uint32_t *)(memory + L[SGXD_R_BGREC]);
    bg[0] = 2u << 27 | ((sgxd_recipe_va(r, SGXD_R_BGSEC) >> 4) & 0x07ffffffu);
    bg[1] = (bg[1] & ~(0x1fu << 17)) | 1u << 17;   // SECATTRSIZE: one 32-dword chunk
    ((uint32_t *)(memory + L[SGXD_R_BGPRIM]))[4] = doutu0(1, SA_BG_FS >> 3, 0);
}

// ---------------------------------------------------------------------------- registers

static void op(SgxdOpList *l, uint32_t kind, uint32_t reg, uint32_t value, uint32_t mask,
               uint32_t timeout_us) {
    if (l->count >= l->max) {
        l->overflow = 1;
        return;
    }
    l->ops[l->count++] = (SgxdOp){kind, reg, value, mask, timeout_us};
}
static void wr(SgxdOpList *l, uint32_t reg, uint32_t value) { op(l, SGXD_OP_WRITE, reg, value, 0, 0); }
static void poll(SgxdOpList *l, uint32_t reg, uint32_t mask, uint32_t value, uint32_t timeout_us) {
    op(l, SGXD_OP_POLL, reg, value, mask, timeout_us);
}

#define CORE(c, reg) (0x8000u + (c) * 0x4000u + (reg))
#define SETUP_POLL_US 100000u   // the probe's set-up waits gave 2 s; each completes in µs

void sgxd_recipe_slc(SgxdOpList *l, int inval) {
    wr(l, 0x4d2c, 0x10);
    poll(l, 0x4d10, 2, 2, 1000);
    wr(l, 0x4d14, 2);
    if (inval) {
        wr(l, 0x4d28, 1);
        poll(l, 0x4d10, 1, 1, 1000);
        wr(l, 0x4d14, 1);
    }
}

// Core 0's event program: IDLE, one unit (T0.4; the master's events run it, recipe R1).
static void idle_event(const SgxdRecipe *r, SgxdOpList *l) {
    wr(l, CORE(0, 0x0a68), sgxd_recipe_va(r, SGXD_R_IDLE));
    wr(l, CORE(0, 0x0a6c), 1);
}

// The TA kick and TA done write only what the TA side uses: the next pass's TA runs while the
// last pass's 3D does, and Sony's microkernel writes none of the 3D side's registers there
// (they all come between one 3D's end and the next 3D's kick).
void sgxd_recipe_ta_kick(const SgxdRecipe *r, SgxdOpList *l) {
    // T0.4-T0.8: the vertex code base, TA_CLK_GATE, the caches, the clocks and DMS_CTRL.
    wr(l, 0x0a0c, sgxd_recipe_va(r, SGXD_R_CV) >> 6);
    for (unsigned c = 0; c < 4; c++) {
        wr(l, CORE(c, 0x0310), 1);
    }
    idle_event(r, l);
    // The broadcast invalidates only, as Sony's: a core's own (0x8810, 0x8808, 0x8a08 and core
    // 1's) with a 3D in flight stops that 3D.
    wr(l, 0x0810, 1);
    wr(l, 0x0808, 1);
    if (!r->keep_code_caches) {
        wr(l, 0x0a08, 1);
    }
    // PDS_INV0 and PDS_INV_CSC, as Sony's TA kick: their events are bits 1 and 0.
    wr(l, 0x0ad0, 1);
    wr(l, 0x0ae0, 1);
    poll(l, 0x0138, 3, 3, SETUP_POLL_US);
    wr(l, 0x0140, 3);
    wr(l, 0x0004, 0x0a8a4a6a);
    wr(l, 0x0a74, 0x0e208200);

    // The DPM's two state slots (DPM_STATE_CONTEXT_ID: LS bit 0, DALLOC bit 1, ALLOC bit 2,
    // NCOP bit 3): slot 0 for both, or with the shared buffer Sony's (5, LS and ALLOC slot 1,
    // DALLOC slot 0, then 0xd). Sony's microkernel writes only 0xa and 0xd (and 2 and 5 before
    // them) with a 3D in flight: its TA never clears the 3D's slot.
    const uint32_t dpm = r->shared_pb ? 5 : 0;
    wr(l, 0x0630, dpm | 8);
    // T1: the requestor bases and the DPM's TA free list.
    // With the shared buffer, the free list once: Sony's TA writes none of it again.
    wr(l, 0x0c90, r->req_base);
    const uint32_t t1[][2] = {
        {0x0618, r->pt}, {0x061c, r->ta_free_list}, {0x0648, r->ta_alloc}, {0x0638, 0},
        {0x0620, r->ta_threshold}, {0x0614, r->pds_threshold}, {0x0624, r->pds_threshold},
        {0x0628, r->global_list}, {0x480c, 0xffffffffu}, {0x0664, 3},
    };
    if (!r->shared_pb || !pb_ta_loaded) {
        for (unsigned n = 0; n < sizeof(t1) / sizeof(t1[0]); n++) {
            wr(l, t1[n][0], t1[n][1]);
        }
        wr(l, 0x0114, 1);
        wr(l, 0x0684, 1);
        poll(l, 0x0118, 1, 1, SETUP_POLL_US);
        wr(l, 0x0114, 1);
        pb_ta_loaded = r->shared_pb;
    }

    // T2: TE and MTE, then each core's region array, tail pointer cache and TE_STATE.
    const uint32_t t2[][2] = {
        {0x0204, 0}, {0x0208, r->mtile1}, {0x020c, r->mtile2}, {0x0210, r->te_screen},
        {0x0214, r->stride}, {0x0218, 0x00400000}, {0x023c, 0xa2}, {0x0240, 0x358637bd},
        {0x0244, 0x358637bd}, {0x0248, r->mte_screen}, {0x0388, 0}, {0x0250, 0x88},
        {0x0238, sgxd_recipe_va(r, SGXD_R_STREAM)},
    };
    for (unsigned n = 0; n < sizeof(t2) / sizeof(t2[0]); n++) {
        wr(l, t2[n][0], t2[n][1]);
    }
    for (unsigned c = 0; c < 4; c++) {
        wr(l, CORE(c, 0x021c), sgxd_recipe_va(r, SGXD_R_RGN0 + c));
        wr(l, CORE(c, 0x0220), sgxd_recipe_va(r, SGXD_R_TPC0 + c));
        wr(l, CORE(c, 0x0274), 0);
    }
    idle_event(r, l);

    // T3: the scene reset.
    // Cores 1-3's init events from the last scene, so the wait below sees this one's.
    for (unsigned c = 1; c < 4; c++) {
        wr(l, CORE(c, 0x0114), 0x20);
    }
    const uint32_t t3[][2] = {
        {0x0630, dpm}, {0x435c, 1}, {0x4368, 1}, {0x0380, 1}, {0x0630, dpm | 8}, {0x0694, 2},
        {0x0698, 2}, {0x024c, 1},
    };
    for (unsigned n = 0; n < sizeof(t3) / sizeof(t3[0]); n++) {
        wr(l, t3[n][0], t3[n][1]);
    }
    poll(l, 0x012c, 0x00100240, 0x00100240, SETUP_POLL_US);
    wr(l, 0x0134, 0x00100240);
    // Each core's TE initialises its own region array, and latches its own event: waiting on
    // core 0's alone lets the kick start before cores 1-3 finish, and a header left with the
    // sentinel sends the next 3D to page 0xeeee.
    for (unsigned c = 0; c < 4; c++) {
        poll(l, CORE(c, 0x0118), 0x20, 0x20, SETUP_POLL_US);
        wr(l, CORE(c, 0x0114), 0x20);
    }
    wr(l, 0x0114, 0x20);
    poll(l, 0x4360, 1, 1, SETUP_POLL_US);
    poll(l, 0x436c, 1, 1, SETUP_POLL_US);
    wr(l, 0x4370, 1);

    // T4: start.
    wr(l, 0x0750, 0);
    wr(l, 0x0218, 0x01400000);
    wr(l, 0x0134, 0x2000);
    wr(l, 0x4134, 0x2000);
    for (unsigned c = 0; c < 4; c++) {
        wr(l, CORE(c, 0x0134), 0x2000);
    }
    idle_event(r, l);
    wr(l, 0x0200, 1);
}

void sgxd_recipe_ta_wait(SgxdOpList *l, uint32_t timeout_us) {
    poll(l, CORE(0, 0x012c), 0x2000, 0x2000, timeout_us);
}

void sgxd_recipe_ta_done(const SgxdRecipe *r, SgxdOpList *l) {
    (void)r;
    for (unsigned c = 0; c < 4; c++) {
        poll(l, CORE(c, 0x0008), 0x00100048, 0, SETUP_POLL_US);
    }
    const uint32_t t5[][2] = {
        {0x4320, 1}, {0x4320, 0}, {0x0320, 1}, {0x0320, 0}, {0x0134, 0x2000}, {0x4134, 0x2000},
        {0x4800, 0x1fff}, {0x0224, 0x40000000},
    };
    for (unsigned n = 0; n < sizeof(t5) / sizeof(t5[0]); n++) {
        wr(l, t5[n][0], t5[n][1]);
    }
    poll(l, 0x012c, 0x400, 0x400, SETUP_POLL_US);
    wr(l, 0x0134, 0x400);
    sgxd_recipe_slc(l, 1);
}

void sgxd_recipe_3d_kick(const SgxdRecipe *r, SgxdOpList *l) {
    // D0: the 3D side's bases and SOFT_RESET bit 6's pulse, where Sony's microkernel has them.
    wr(l, 0x0080, 0x40);
    wr(l, 0x0080, 0);
    wr(l, 0x0a10, sgxd_recipe_va(r, SGXD_R_CF) >> 6 | 1u << 26);
    wr(l, 0x0cac, r->req_base);
    wr(l, 0x0cb0, r->req_base);
    // D1: the DPM. With the shared buffer, Sony's: the TA's slot 1 into the scene's DPMSLOT table and the table
    // into the 3D's slot 0 (DPM_STATE_STORE 0x10 and LOAD 0x20 in EVENT_STATUS), the 3D free
    // list once, and a deallocating render (DPM_3D_DEALLOCATE ENABLE and GLOBAL).
    if (r->shared_pb) {
        const uint32_t ta = 5, d3 = 4;
        const uint32_t table = sgxd_recipe_va(r, SGXD_R_DPMSLOT);
        const uint32_t task[2][3] = {{ta, 1, 0x10}, {d3, 4, 0x20}};
        for (unsigned n = 0; n < 2; n++) {
            wr(l, 0x0630, task[n][0]);
            wr(l, 0x062c, table);
            wr(l, 0x0134, task[n][2]);
            wr(l, 0x0694, task[n][1]);
            poll(l, 0x012c, task[n][2], task[n][2], SETUP_POLL_US);
            wr(l, 0x0134, task[n][2]);
        }
    } else {
        wr(l, 0x0630, 8);
    }
    if (!r->shared_pb || !pb_3d_loaded) {
        wr(l, 0x0600, r->pt);
        wr(l, 0x0604, r->ta_free_list);
        wr(l, 0x064c, r->ta_alloc);
        wr(l, 0x0660, 0);
        wr(l, 0x0664, 5);
        wr(l, 0x0114, 2);
        wr(l, 0x0680, 1);
        poll(l, 0x0118, 2, 2, SETUP_POLL_US);
        wr(l, 0x0114, 2);
        pb_3d_loaded = r->shared_pb;
    }
    wr(l, 0x063c, r->shared_pb ? 3 : 0);
    wr(l, 0x0658, 0);
    wr(l, 0x0004, 0x0a8a4a6a);

    // Sony's pre-kick invalidate, bank 0: it reaches all four cores' code caches.
    wr(l, 0x0114, 0x80);
    wr(l, 0x0134, 0x04000000);
    wr(l, 0x0810, 1);
    wr(l, 0x0808, 1);
    if (!r->keep_code_caches) {
        wr(l, 0x0a08, 1);
    }
    poll(l, 0x0118, 0x80, 0x80, SETUP_POLL_US);
    poll(l, 0x012c, 0x04000000, 0x04000000, SETUP_POLL_US);
    wr(l, 0x0114, 0x80);
    wr(l, 0x0134, 0x04000000);

    // D2: ISP, PBE and the pixel event.
    const uint32_t d2[][2] = {
        {0x04c0, 0}, {0x040c, 0}, {0x0410, 0}, {0x0400, 0x80}, {0x0404, 0},
        {0x0408, sgxd_recipe_va(r, SGXD_R_RGN0)}, {0x4510, sgxd_recipe_va(r, SGXD_R_RGN1)},
        {0x4514, sgxd_recipe_va(r, SGXD_R_RGN2)}, {0x4518, sgxd_recipe_va(r, SGXD_R_RGN3)},
        {0x451c, r->rgn_size}, {0x0414, 0}, {0x0418, 1}, {0x041c, 0x1e3ce508},
        {0x0420, 0x1e3ce508}, {0x050c, sgxd_recipe_va(r, SGXD_R_EXTZ)}, {0x0424, 0xe9},
        {0x042c, 0}, {0x044c, 0}, {0x0480, r->zls[0]}, {0x0484, r->zls[1]},
        {0x0488, r->zls[2]}, {0x048c, r->zls[3]}, {0x0490, r->zls[4]},
        {0x04b8, r->bgobjdepth}, {0x04bc, r->bgobj}, {0x04c4, r->bgobjtag},
        {0x04c8, 0x88}, {0x04cc, 0x188}, {0x04dc, 8}, {0x0520, r->mtile1},
        {0x0524, r->mtile2}, {0x0528, r->stride}, {0x0148, 0},
        {0x0a5c, sgxd_recipe_va(r, SGXD_R_PIXEV)}, {0x0a60, 3}, {0x0a64, 0x4000}, {0x0ae8, 0},
    };
    for (unsigned n = 0; n < sizeof(d2) / sizeof(d2[0]); n++) {
        wr(l, d2[n][0], d2[n][1]);
    }
    wr(l, 0x0140, 5);
    wr(l, 0x043c, 1);
    wr(l, 0x0ad4, 1);
    wr(l, 0x0ae0, 1);
    poll(l, 0x0138, 5, 5, SETUP_POLL_US);
    wr(l, 0x0140, 5);

    wr(l, 0x0134, 0x00040001);
    wr(l, 0x4134, 0x00040001);
    for (unsigned c = 0; c < 4; c++) {
        wr(l, CORE(c, 0x0134), 0x00040001);
    }
    idle_event(r, l);
    wr(l, 0x0428, 1);
}

void sgxd_recipe_3d_wait(SgxdOpList *l, uint32_t timeout_us) {
    poll(l, CORE(0, 0x012c), 0x40000, 0x40000, timeout_us);
}

void sgxd_recipe_3d_done(const SgxdRecipe *r, SgxdOpList *l) {
    for (unsigned c = 0; c < 4; c++) {
        poll(l, CORE(c, 0x0008), 0x186, 0, SETUP_POLL_US);
    }
    // A deallocating render's pages are back in the list at DPM_3D_MEM_FREE, after
    // PIXELBE_END_RENDER. Sony's 3D done acknowledges both; the next 3D kick's store and load
    // must not meet the DPM still deallocating (the master then reads page 0xeeee).
    if (r->shared_pb) {
        poll(l, 0x012c, 1, 1, SETUP_POLL_US);
    }
    wr(l, 0x0134, 0x00040001);
    wr(l, 0x4134, 0x00040001);
    for (unsigned c = 0; c < 4; c++) {
        wr(l, CORE(c, 0x0134), 0x00040001);
    }
    wr(l, 0x0004, 0x0a8a8aaa);
    sgxd_recipe_slc(l, 1);
}

void sgxd_recipe_set_clip(const SgxdRecipe *r, void *memory, unsigned draw, uint32_t clip) {
    uint32_t *state = (uint32_t *)((uint8_t *)memory + r->offset[SGXD_R_STATE] + draw * 0x60);
    state[10] = clip_x(r, clip);
    state[11] = clip_y(r, clip);
}

void sgxd_recipe_texture_words(uint32_t va, uint32_t width, uint32_t height, int bilinear,
                               uint32_t words[4]) {
    sgxd_recipe_texture_words_format(va, width, height, bilinear, 0x0c, 0, words);
}

void sgxd_recipe_texture_words_format(uint32_t va, uint32_t width, uint32_t height, int bilinear,
                                      unsigned code, unsigned swizzle, uint32_t words[4]) {
    words[0] = 0x03fe0090u | (bilinear ? 1u << 12 | 1u << 10 : 0) | (code >> 7 & 1u) << 31;
    words[1] = 0x60000000u | (code & 0x1fu) << 24 | (width - 1) << 12 | (height - 1);
    words[2] = va;
    words[3] = 0x80000000u | (swizzle & 7u) << 28;
}

void sgxd_recipe_set_texture(const SgxdRecipe *r, void *memory, const uint32_t words[4]) {
    Code cf[CF_COUNT];
    Pds p;
    uint32_t program[SGXD_RECIPE_TEX_PDS / 4];
    make_cf(r, cf);
    imgui_prim(r, cf, &p, words);
    const unsigned n = pds_words(&p, program);
    // The layout gives FPRIM the room of the texture program in these modes (pds_slot); a
    // longer program would run into BGSEC.
    if (imgui_texel(r) && n * 4 <= pds_slot(r, SGXD_R_FPRIM)) {
        put(memory, r->offset[SGXD_R_FPRIM], program, n);
    }
}

void sgxd_recipe_draw_texture(const SgxdRecipe *r, void *memory, uint32_t offset, unsigned draw,
                              const uint32_t words[4]) {
    Code cf[CF_COUNT];
    Pds p;
    uint32_t program[SGXD_RECIPE_TEX_PDS / 4];
    make_cf(r, cf);
    imgui_prim(r, cf, &p, words);
    const unsigned n = pds_words(&p, program);
    memcpy((uint8_t *)memory + offset + draw * SGXD_RECIPE_TEX_PDS, program, n * 4);
}

void sgxd_recipe_draw_textures(const SgxdRecipe *r, void *memory, uint32_t offset,
                               const uint32_t (*words)[4]) {
    for (unsigned n = 0; n < r->ndraws; n++) {
        if (r->draws[n].fprim != SGXD_R_FPRIM) {
            continue;
        }
        sgxd_recipe_draw_texture(r, memory, offset, n, words[n]);
        // State words 8 and 9: the primary's top address bit, and its pointer (FPRIM's units).
        uint32_t *state = (uint32_t *)((uint8_t *)memory + r->offset[SGXD_R_STATE] + n * 0x60);
        const uint32_t va = r->base + offset + n * SGXD_RECIPE_TEX_PDS;
        state[8] = (state[8] & ~(1u << 13)) | (va & 0x80000000u ? 1u << 13 : 0);
        state[9] = r->units[SGXD_R_FPRIM] << 27 | ((va >> 4) & 0x07ffffffu);
    }
}

void sgxd_recipe_sa_draw_colours(const SgxdRecipe *r, void *memory_, uint32_t offset,
                                 const uint32_t *colours) {
    uint8_t *memory = memory_;
    const uint32_t *L = r->offset;
    put_sa_shader(r, memory);
    for (unsigned n = 0; n < r->ndraws; n++) {
        const uint32_t at = offset + 0x40 * (n + 1);
        *(uint32_t *)(memory + sgxd_recipe_sa_colour_offset(offset, n)) = colours[n];
        put_sa_secondary(r, memory, at, sgxd_recipe_sa_colour_offset(offset, n));
        // State words 7 and 8: the secondary's pointer, and DMS info with SECATTRSIZE 1 and
        // the pointers' top bits.
        uint32_t *state = (uint32_t *)(memory + L[SGXD_R_STATE] + n * 0x60);
        const uint32_t va = r->base + at;
        state[7] = 2u << 27 | ((va >> 4) & 0x07ffffffu);
        state[8] = (state[8] & ~(0x1fu << 17 | 1u << 14)) | 1u << 17 |
                   (va & 0x80000000u ? 1u << 14 : 0);
    }
    ((uint32_t *)(memory + L[SGXD_R_FPRIM]))[4] = doutu0(1, SA_BG_FS >> 3, 0);
}
