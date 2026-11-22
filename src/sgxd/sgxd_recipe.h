// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// sgxd_recipe — the TA+3D recipe: a pass's memory and sgxdSubmit op lists, at any GPU address
// and size, drawing its triangle or a caller's scene of several draws (docs/sgxd/README.md, "One
// render").
//
// The register sequences are the ones probed on the hardware (the TA's preparation, kick and
// finish; the 3D's DPM setup, cache invalidates, ISP setup and render start), with cores 2 and 3
// written directly: sgxdSubmit reaches every core bank.
//
// Plain C99 and no Vita headers.

#ifndef SGXD_RECIPE_H
#define SGXD_RECIPE_H

#include <stdint.h>

#include "sgxd.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SGXD_R_PT, SGXD_R_PAGES, SGXD_R_RGN0, SGXD_R_RGN1, SGXD_R_RGN2, SGXD_R_RGN3, SGXD_R_EXTZ,
    SGXD_R_TPC0, SGXD_R_TPC1, SGXD_R_TPC2, SGXD_R_TPC3, SGXD_R_BGREC, SGXD_R_FB, SGXD_R_DEPTH,
    SGXD_R_STREAM, SGXD_R_STATE, SGXD_R_VERTS, SGXD_R_INDICES, SGXD_R_PDS, SGXD_R_COPY,
    SGXD_R_VSPDS, SGXD_R_VSEC, SGXD_R_TERM, SGXD_R_FSEC, SGXD_R_FPRIM, SGXD_R_BGSEC,
    SGXD_R_BGPRIM, SGXD_R_PIXEV, SGXD_R_IDLE, SGXD_R_COPY1, SGXD_R_COPY2, SGXD_R_COPY3,
    SGXD_R_COPY4, SGXD_R_COPY5, SGXD_R_COPY6, SGXD_R_COPY7, SGXD_R_CV, SGXD_R_CF, SGXD_R_END,
    SGXD_R_COUNT
};

extern const char *const sgxd_recipe_names[SGXD_R_COUNT];

#define SGXD_RECIPE_SENTINEL 0xeeeeeeeeu
#define SGXD_RECIPE_BG 0xff202020u
#define SGXD_RECIPE_TRI 0xff0000ffu
#define SGXD_RECIPE_MAX_INDICES 0x10000u
// Draws of a scene. Draw n's state copy is COPY for the first and COPY1 + (n - 1) * 0x40 for the
// others, so COPY1..COPY7 are the second to eighth draws' and the rest follow COPY7's.
#define SGXD_RECIPE_MAX_DRAWS 4096u
// ISP A: opaque, DCMP always, DWRITEDIS, BPRES. With DCMP never (bits 22-24
// clear) a draw covers no pixel but still puts its tiles in the scene, so their background is
// drawn: a tile no primitive touches is left as it was.
#define SGXD_RECIPE_ISP_A (0u << 25 | 7u << 22 | 1u << 20 | 0u << 15 | 1u << 9)
#define SGXD_RECIPE_ISP_NEVER (SGXD_RECIPE_ISP_A & ~(7u << 22))
// ISP A with PASSTYPE translucent (bits 27..25 = 1): what lies under the draw is shaded first,
// so a program that reads the output buffer sees it.
#define SGXD_RECIPE_ISP_TRANS (SGXD_RECIPE_ISP_A | 1u << 25)
// Words of an ImDrawVert: x and y in pixels, u and v (f32), colour A8B8G8R8 unorm.
#define SGXD_RECIPE_IMGUI_WORDS 5u
// ImGui mode's pixel output (sgxd_recipe_init_imgui): the iterated colour (sgxd_test 06), or r
// and g of the colour with the iterated u and v as blue and alpha (sgxd_test 07, TC0 through
// DOUTI).
// SGXD_IMGUI_UV_RAW + n iterates as SGXD_IMGUI_COLOUR_UV does and writes pa n's F32 bits as the
// pixel word (n 0..5: r, g, b, a, u, v), so the frame holds the iterators' values.
// SGXD_IMGUI_TEXEL (sgxd_test 08) iterates the colour into pa0..pa3 and samples texture 0 at TC0
// into pa4, a texture issue and a DOUTT with the texel unconverted; the pixel word is the
// texel's bits. Each draw's texture comes from sgxd_recipe_draw_textures.
// SGXD_IMGUI_BLEND (sgxd_test 09) iterates the colour as SGXD_IMGUI_COLOUR does and blends it over
// the pixel's current value in the output buffer: colour = src * a + dst * (1 - a), alpha = a +
// dst.a * (1 - a), ImGui's blend. Overlapping draws need a translucent ISP pass type.
// SGXD_IMGUI_MODULATE (sgxd_test 12, ImGui's own program) samples as SGXD_IMGUI_TEXEL does,
// multiplies the texel by the colour bytewise (texel * colour / 255 on each of the four bytes) and
// blends the product over the output buffer as SGXD_IMGUI_BLEND does.
enum { SGXD_IMGUI_COLOUR, SGXD_IMGUI_COLOUR_UV, SGXD_IMGUI_UV_RAW,
       SGXD_IMGUI_TEXEL = SGXD_IMGUI_UV_RAW + 6, SGXD_IMGUI_BLEND, SGXD_IMGUI_MODULATE };

// One draw of a scene: a triangle list of indices[first .. first + count),
// first a multiple of 8, in the ISP A word's state, shaded by FPRIM (the triangle's colour) or
// BGPRIM (the background's), with MTE CULLMODE cull (0 none, 1 clockwise, 2 counterclockwise).
// clip 0 keeps the whole target; SGXD_RECIPE_CLIP(x0, y0, x1, y1) keeps the 32-pixel tiles x0..x1,
// y0..y1 (inclusive), the TA's region clip (MODE OUTSIDE).
typedef struct SgxdRecipeDraw {
    unsigned first, count;
    uint32_t isp;
    unsigned fprim;   // SGXD_R_FPRIM or SGXD_R_BGPRIM
    unsigned cull;
    uint32_t clip;
} SgxdRecipeDraw;
#define SGXD_RECIPE_CLIP(x0, y0, x1, y1)                                                          \
    (1u << 31 | (uint32_t)(x0) << 24 | (uint32_t)(x1) << 16 | (uint32_t)(y0) << 8 | (uint32_t)(y1))

typedef struct SgxdRecipe {
    uint32_t width, height, base, pages;
    uint32_t offset[SGXD_R_COUNT];   // from base; offset[SGXD_R_END] is the footprint
    uint32_t first, last;            // the parameter pages' numbers from base & ~0xfffff
    uint32_t tx, ty, mx, my, count, mtile1, mtile2, stride, te_screen, mte_screen;
    uint32_t rgn_size, tpc_size;
    uint32_t ta_free_list, ta_alloc, ta_threshold, pds_threshold, global_list;
    uint32_t bgobjtag;
    uint32_t pbe[6];
    uint32_t vdm_w2, vdm_w4, vdm_w5;  // the VDM index list's words 2, 4 and 5
    uint32_t units[SGXD_R_COUNT];    // PDS programs' data units (COPY .. BGPRIM)
    const float *verts;              // the scene: clip-space x, y, z, w a vertex (w taken as 1)
    const uint16_t *indices;         // the draws' triangle lists
    unsigned nverts, nindices;
    const SgxdRecipeDraw *draws;     // the caller's
    unsigned ndraws;
    int copy_vs;                     // the vertex program copies w too (a caller's scene)
    const uint32_t *imgui_verts;     // ImGui mode (sgxd_recipe_init_imgui): its vertices
    int imgui;
    unsigned vertex_words;           // a vertex's words in VERTS: 4, or 5 in ImGui mode
    uint32_t vertex_sa;              // ImGui mode: the vertex program's SA block, from base
    unsigned imgui_fs;               // ImGui mode: SGXD_IMGUI_*
    int fb_external;                 // the target is not the block's FB (sgxd_recipe_set_target)
    int keep_code_caches;            // sgxd_test 11: the kicks skip the USE_CACHE invalidates
} SgxdRecipe;

// A width x height (32 to 2048) target at GPU address base (1 MiB aligned), with pages parameter
// pages, drawing the recipe's triangle. Returns 0, or -1 for sizes the layout does not cover.
// From 0x4b000 pixels (960x544) the macrotiles are a 4x4 grid, below it 2x2.
int sgxd_recipe_init(SgxdRecipe *r, uint32_t width, uint32_t height, uint32_t base,
                     uint32_t pages);
// The same with a scene of the caller's: nverts vertices of four
// floats, which the vertex program copies, nindices 16-bit indices and 1 to
// SGXD_RECIPE_MAX_DRAWS draws of them. The arrays are the caller's and must outlive r; the layout
// sizes VERTS and INDICES by the counts, so a later sgxd_recipe_write_scene may change values,
// not counts or draws.
int sgxd_recipe_init_scene(SgxdRecipe *r, uint32_t width, uint32_t height, uint32_t base,
                           uint32_t pages, const float *verts, unsigned nverts,
                           const uint16_t *indices, unsigned nindices,
                           const SgxdRecipeDraw *draws, unsigned ndraws);

// ImGui's vertex path. nverts ImDrawVert records (SGXD_RECIPE_IMGUI_WORDS words each)
// fetched at stride 20 into pa0..pa4; the vertex program maps x, y through the scale and offset in
// its SA (clip = pixel * scale + offset; sa0, sa1 the scale, sa2, sa3 the offset), which each
// draw's vertex secondary DMAs from base + sa_offset (16-byte aligned, outside the layout: write
// it with sgxd_recipe_write_vertex_sa), and unpacks the colour into the base colour. Draws
// shaded by FPRIM iterate it (DOUTI V0, four F32 lanes) and pack it into the pixel; with fs
// SGXD_IMGUI_COLOUR_UV the UV goes out as TC0 too and the pixel is (r, g, u, v). BGPRIM draws
// and the background are unchanged; sgxd_recipe_sa_draw_colours does not apply.
int sgxd_recipe_init_imgui(SgxdRecipe *r, uint32_t width, uint32_t height, uint32_t base,
                           uint32_t pages, const uint32_t *verts, unsigned nverts,
                           const uint16_t *indices, unsigned nindices,
                           const SgxdRecipeDraw *draws, unsigned ndraws, uint32_t sa_offset,
                           unsigned fs);
void sgxd_recipe_write_vertex_sa(const SgxdRecipe *r, void *memory, const float sa[4]);

static inline uint32_t sgxd_recipe_va(const SgxdRecipe *r, unsigned part) {
    return r->base + r->offset[part];
}

// Every byte from base to base + offset[SGXD_R_END], through the CPU's view of it: the DPM page
// table and its free list, the sentinels, the zeroed caches and every program and structure.
void sgxd_recipe_write(const SgxdRecipe *r, void *memory);

// The background's colour through SA, loaded by the secondary program. BGSEC DMAs the word at base
// + colour_offset (written here) into sa0 and needs two data units and one SECATTRSIZE chunk;
// BGPRIM runs o0 = sa0 from CF + 0x180.
void sgxd_recipe_sa_background(const SgxdRecipe *r, void *memory, uint32_t colour_offset,
                               uint32_t colour);

// Rewrites CF + 0x180, the program sgxd_recipe_sa_background and
// sgxd_recipe_sa_draw_colours run, as xor o0, sa0, #imm (16 bits, into the low bytes). The
// program keeps its address, so the kicks' USE_CACHE invalidates must reach every core's code
// cache before it runs; with r->keep_code_caches set they are skipped, to show the rule.
void sgxd_recipe_sa_xor(const SgxdRecipe *r, void *memory, uint32_t imm);

// Each draw's colour through SA, as the background's above. At base + offset (64-byte
// aligned, 0x40 * (ndraws + 1) bytes, outside the layout) go each draw's own SA block, 0x40
// bytes from offset + 0x40 * (draw + 1): its secondary, which DMAs the block's colour word
// (at + 0x30) into sa0, and the word; the draw's state packet points at the secondary
// with one SECATTRSIZE chunk, and FPRIM runs o0 = sa0. Every draw shaded by FPRIM takes its
// colour so; a new colour needs only its word rewritten (sgxd_recipe_sa_colour_offset).
void sgxd_recipe_sa_draw_colours(const SgxdRecipe *r, void *memory, uint32_t offset,
                                 const uint32_t *colours);

// SGXD_IMGUI_TEXEL and SGXD_IMGUI_MODULATE modes: each draw's own texture. At base +
// offset (16-byte aligned, SGXD_RECIPE_TEX_PDS bytes a draw, outside the layout) go the draws'
// copies of FPRIM, each sending its draw's four texture control words (SceGxmTexture's,
// DOUTT0..3); every draw's state packet then points at its copy. sgxd_recipe_draw_texture
// rewrites one draw's copy, for a new texture. Call both after sgxd_recipe_write, which points
// the draws back at FPRIM.
#define SGXD_RECIPE_TEX_PDS 0x80u
void sgxd_recipe_draw_textures(const SgxdRecipe *r, void *memory, uint32_t offset,
                               const uint32_t (*words)[4]);
void sgxd_recipe_draw_texture(const SgxdRecipe *r, void *memory, uint32_t offset, unsigned draw,
                              const uint32_t words[4]);
// A linear U8U8U8U8 texture's four control words (SceGxmTexture, libgxm's for one level), as
// test 08 ran them: DADJUST 31, MIPMAPCLAMP 15, U and V CLAMP; TEXTYPE LINEAR, format 0x0c,
// width and height less one (1 to 4096); the address va; FCNORM, swizzle ABGR (R the lowest
// byte). Bilinear sets MAGFILTER and MINFILTER (bits 12 and 10). Rows are width texels apart;
// libgxm aligns a linear texture's rows to 8 texels, so keep width a multiple of 8.
void sgxd_recipe_texture_words(uint32_t va, uint32_t width, uint32_t height, int bilinear,
                               uint32_t words[4]);
static inline uint32_t sgxd_recipe_sa_colour_offset(uint32_t offset, unsigned draw) {
    return offset + 0x40 * (draw + 1) + 0x30;
}

// What a render changes, set back: the page table's free list, the region headers, EXTZ, the
// tail pointer caches and, unless the target is external, FB and DEPTH (to the sentinel).
void sgxd_recipe_restage(const SgxdRecipe *r, void *memory);

void sgxd_recipe_write_scene(const SgxdRecipe *r, void *memory);
// This frame's draws, count of them (1 to r->ndraws, the number init sized the layout
// for), each checked as init checks r->draws against r->nindices. Rewrites their state packets
// and the VDM stream, which then ends after them. The vertices and indices they use are the
// caller's to write in place: vertex n at offset[SGXD_R_VERTS] + n * vertex_words * 4, the
// indices as 16-bit values from offset[SGXD_R_INDICES]. Returns 0, or -1 with nothing written.
int sgxd_recipe_set_draws(const SgxdRecipe *r, void *memory, const SgxdRecipeDraw *draws,
                          unsigned count);
// SGXD_IMGUI_TEXEL and SGXD_IMGUI_MODULATE: FPRIM with the texture words of texture 0, for
// every draw shaded by FPRIM. FPRIM is a PDS program: rewrite it only before its first kick, or
// after the PDS cache rule's invalidate (docs/sgxd/pds.md).
void sgxd_recipe_set_texture(const SgxdRecipe *r, void *memory, const uint32_t words[4]);
// Draw n's region clip (SgxdRecipeDraw's clip: 0 or SGXD_RECIPE_CLIP), its state words 10 and 11.
void sgxd_recipe_set_clip(const SgxdRecipe *r, void *memory, unsigned draw, uint32_t clip);

// Render to fb_va instead of the block's FB: a linear A8B8G8R8 target of r->width x r->height
// with stride_px pixels a row. Rewrites the end-of-tile program's PBE words in memory, so the
// 3D kick's USE code-cache invalidate must follow (sgxd_recipe_3d_kick has it).
void sgxd_recipe_set_target(SgxdRecipe *r, void *memory, uint32_t fb_va, uint32_t stride_px);

typedef struct SgxdOpList {
    SgxdOp *ops;
    unsigned count, max;
    int overflow;
} SgxdOpList;

// The TA: the code bases, the cache invalidates, the DPM free-list load, TE and MTE, the scene
// reset, then VDM_START last (T0.4-T4).
void sgxd_recipe_ta_kick(const SgxdRecipe *r, SgxdOpList *list);
// TA_FINISHED in core 0's EVENT_STATUS (T5.1).
void sgxd_recipe_ta_wait(SgxdOpList *list, uint32_t timeout_us);
// The TA's end: the cores idle, the TPC flush, the SLC flush and invalidate (T5.2-T5.8).
void sgxd_recipe_ta_done(const SgxdRecipe *r, SgxdOpList *list);
// The 3D: the DPM 3D free-list load, the code-cache invalidate, ISP, PBE and the pixel event,
// then ISP_START_RENDER last (D1, D2).
void sgxd_recipe_3d_kick(const SgxdRecipe *r, SgxdOpList *list);
// PIXELBE_END_RENDER in core 0's EVENT_STATUS (D3.1).
void sgxd_recipe_3d_wait(SgxdOpList *list, uint32_t timeout_us);
// The 3D's end: the cores idle, the event clears, the SLC flush and invalidate (D3.2-D3.5).
void sgxd_recipe_3d_done(const SgxdRecipe *r, SgxdOpList *list);

// The SLC flush, and with inval the invalidate (T0.1, T0.2). An invalidate only while idle.
void sgxd_recipe_slc(SgxdOpList *list, int inval);

#ifdef __cplusplus
}
#endif

#endif
