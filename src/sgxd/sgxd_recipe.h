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
    SGXD_R_TPC0, SGXD_R_TPC1, SGXD_R_TPC2, SGXD_R_TPC3, SGXD_R_BGREC, SGXD_R_DPMSLOT, SGXD_R_FB,
    SGXD_R_DEPTH,
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
// A draw with its own programs (sgxd_program.h builds them) instead of the recipe's
// vertex fetch, vertex secondary and pixel programs. Every address is a GPU VA, every PDS program
// 16-byte aligned, and each *_units is its PDS data size (pds_units).
typedef struct SgxdRecipeDrawPrograms {
    uint32_t vspds, vspds_units;      // the vertex fetch and the vertex program's DOUTU
    uint32_t vdm_w2, vdm_w4, vdm_w5;  // sgxd_program_vertex_dms: the index list's words 2, 4, 5
    uint32_t vsec, vsec_units;        // the vertex secondary: its SA and secondary program
    uint32_t vsec_sa;                 // the SA words it loads (USEATTRIBUTESIZE)
    uint32_t fsec, fsec_units;        // the pixel secondary
    uint32_t fprim, fprim_units;      // the pixel primary: iterators, textures, DOUTU
    uint32_t pixel_size;              // state word 8 less the address MSBs (sgxd_program_pixel_dms)
    uint32_t outselects, texsize;     // state words 19 and 22 (the GXP's vertex_outputs1 and 2)
    float viewport[6];                // state words 12-17: tx, sx, ty, sy, tz, sz
    uint32_t indices;                 // the 16-bit index list, 0 for the layout's INDICES
    uint32_t primitive;               // SGXD_PROGRAM_TRIANGLES, _STRIP or _FAN (sgxd_program.h)
} SgxdRecipeDrawPrograms;

typedef struct SgxdRecipeDraw {
    unsigned first, count;
    uint32_t isp;
    unsigned fprim;   // SGXD_R_FPRIM or SGXD_R_BGPRIM; unused with programs
    unsigned cull;
    uint32_t clip;
    const SgxdRecipeDrawPrograms *programs;   // NULL: the recipe's programs
    // ISP C, the stencil (SCMP 27..25, SOP1 24..22 stencil fail,
    // SOP2 21..19 depth fail, SOP3 18..16 pass, SCMPMASK 15..8, SWMASK 7..0), for both faces;
    // the reference is ISP A's SREF (7..0). 0 means SGXD_RECIPE_ISP_C.
    uint32_t ispc;
} SgxdRecipeDraw;
// ISP C with no stencil: SCMP ALWAYS, every op KEEP, both masks 0.
#define SGXD_RECIPE_ISP_C 0x0e000000u
// ISP A's CPRES: the object carries its ISP C word.
#define SGXD_RECIPE_ISP_CPRES (1u << 8)
#define SGXD_RECIPE_CLIP(x0, y0, x1, y1)                                                          \
    (1u << 31 | (uint32_t)(x0) << 24 | (uint32_t)(x1) << 16 | (uint32_t)(y0) << 8 | (uint32_t)(y1))

typedef struct SgxdRecipe {
    uint32_t width, height, base, pages;
    uint32_t offset[SGXD_R_COUNT];   // from base; offset[SGXD_R_END] is the footprint
    uint32_t first, last;            // the parameter pages' numbers from req_base
    uint32_t req_base;               // the DPM's requestor base (TA, 3D and ZLS)
    uint32_t pt;                     // the DPM page table's GPU address
    int shared_pb;                   // the pages are sgxd_recipe_pb's, not the layout's PAGES
    uint32_t tx, ty, mx, my, count, mtile1, mtile2, stride, te_screen, mte_screen;
    uint32_t rgn_size, tpc_size;
    uint32_t ta_free_list, ta_alloc, ta_threshold, pds_threshold, global_list;
    uint32_t bgobjtag;
    uint32_t bgobjdepth;             // ISP_BGOBJDEPTH, the 3D kick's: 1.0 until a depth clear sets it
    uint32_t bgobj;                  // ISP_BGOBJ, the 3D kick's: 0x300, stencil (7..0) 0 until set
    uint32_t zls[5];                 // ISP_ZLSCTL and the four ZLS bases, the 3D kick's: 0 until set
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
    // Core 0's event program is someone else's (sgxd's microkernel; sgxd_test 03 mkevents=1):
    // the kicks leave 0x8a68/0x8a6c alone instead of pointing them at the recipe's IDLE.
    int events_owned;
    int programs;                    // sgxd_recipe_init_programs: draws bring programs
    uint32_t pixev_va, pixev_units, pixev_attribute_size;  // sgxd_recipe_set_pixel_event
    uint32_t layout_width, layout_height, layout_rgn_size, layout_tpc_size;
    uint32_t term_va, term_units;          // sgxd_recipe_set_term: the stream's terminate program
    uint32_t double_pixels;                // DOUBLE_PIXEL_PARTITIONS (0x0148) at the 3D kick
} SgxdRecipe;

// The macrotile grid of later inits. 0 (the default) picks by size as Sony's kernel does: 4x4
// from 0x4b000 pixels (960x544), 2x2 below. 4 is 4x4 at every size: below 960x544 the 2x2 grid
// leaves cores idle over much of the 3D time.
extern unsigned sgxd_recipe_grid;
// EUR_CR_PIXELBE (0x0400) at the 3D kicks built after it is set: 0x80 (the alpha threshold) by
// default; bit 10 is AA_EDGEOPT_OFF (the SCALE_AA probes, sgxd_recipe_pica_eot_mix).
extern uint32_t sgxd_recipe_pixelbe;
// One parameter buffer for every recipe inited after it is set, as Sony's microkernel runs: the
// page table at pt, the pages first..last as page numbers from req_base, which every such recipe's
// requestor bases take. Its TA and 3D free lists are loaded once, at the first TA and the first 3D
// built after sgxd_recipe_pb_reset, and every 3D deallocates its scene's pages back into them. The
// TA's DPM state slot (1) goes to the recipe's DPMSLOT table after its TA and into the 3D's slot
// (0) before its render. last 0 (the default): each recipe loads its own layout's PT and PAGES at
// every kick and never deallocates.
// The parameter pages a TA started beside binned scenes is sure of (sgxd_session.c's page budget):
// far past any pass seen (up to 734), with azahar's 7104-page buffer.
#define SGXD_RECIPE_PASS_PAGES 3000u

typedef struct SgxdRecipePb {
    uint32_t req_base, pt, first, last;
} SgxdRecipePb;
extern SgxdRecipePb sgxd_recipe_pb;
// Set before init: every recipe renders to an external target (sgxd_recipe_set_target), so
// the layout gives the block's own FB and DEPTH no room. With sgxd_recipe_pb set, the page
// table and pages take none either.
extern int sgxd_recipe_external_fb;
// A code heap every recipe shares (0: none): every kick sets USE code bases 2 and 3 to it,
// the vertex and the pixel data master, beside bases 0 and 1 (the recipe's CV and CF). Programs
// a DOUTU reaches through bases 2 and 3 are the same under every recipe, so none carries a copy.
extern uint32_t sgxd_recipe_code_heap;
// Every recipe's kicks leave core 0's event program alone, as events_owned does, and their SLC
// requests take Sony's form: one value to MASTER_SLC_CTRL_FLUSH and _INVAL together, both events
// awaited at once (sgxd_recipe_slc). sgxd_ptla_events sets it, the events page's release clears it.
extern int sgxd_recipe_events_owned;
// sgxd_recipe_pb's page list into the CPU's view of its page table: page p links p - 1 and
// p + 1, 0xffff at both ends. Before its first TA only; the DPM owns it afterwards.
void sgxd_recipe_pb_write(void *pt_memory);
// The shared buffer's page table in memory, set where it is written first: sgxd_recipe_pb_rewrite
// writes its list afresh (every page free) for a DPM reset with the GPU idle.
extern void *sgxd_recipe_pb_memory;
void sgxd_recipe_pb_rewrite(void);
// The next TA and 3D built load the shared buffer's free lists again (at sgxd_recipe_pb_write).
void sgxd_recipe_pb_reset(void);
// Whether the TA's and the 3D's free lists are loaded: a kick built now loads them if not.
void sgxd_recipe_pb_loaded(int *ta, int *d3);
// The loads a TA and 3D kick of r leave, for a kept list sent instead of a built one.
void sgxd_recipe_pb_kicked(const SgxdRecipe *r);

// A width x height (32 to 2048) target at GPU address base (1 MiB aligned), with pages parameter
// pages, drawing the recipe's triangle. Returns 0, or -1 for sizes the layout does not cover.
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

// Draws that bring their own programs (SgxdRecipeDraw.programs), each draw's VDM words
// with a vertex secondary as in ImGui mode. The layout's VERTS stays unused; its INDICES holds
// the caller's nindices indices (an identity list for non-indexed draws). draws sizes the layout
// as sgxd_recipe_init_scene's, and each frame sets its own with sgxd_recipe_set_draws.
int sgxd_recipe_init_programs(SgxdRecipe *r, uint32_t width, uint32_t height, uint32_t base,
                              uint32_t pages, const uint16_t *indices, unsigned nindices,
                              const SgxdRecipeDraw *draws, unsigned ndraws);
// The background object's depth, a pass's depth clear: ISP_BGOBJDEPTH, which the next 3D kick
// writes and every pixel's depth starts from, and its three vertices' z. 1.0 as written.
void sgxd_recipe_set_background_depth(SgxdRecipe *r, void *memory, float z);
// The background object's stencil, a pass's stencil clear: ISP_BGOBJ's STENCIL (7..0), which the
// next 3D kick writes. 0 as written.
void sgxd_recipe_set_background_stencil(SgxdRecipe *r, uint8_t stencil);
// The next 3D kick's depth load and store: F32 depth at the GPU address depth, rows of whole 32x32
// tiles (AlignUp(width, 32) samples a row), and with stencil (not 0) one byte a sample at stencil
// in the same layout. load (1: F32 and the stencil bytes, 2: I24ZI8S words at depth, a D24S8
// buffer's, the stencil their top byte) and store force the ZLS for every tile a primitive touches;
// the store is always F32. depth 0 turns the ZLS off, as written.
void sgxd_recipe_set_zls(SgxdRecipe *r, uint32_t depth, uint32_t stencil, int load, int store);

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
// The same linear texture in another format: code TEXFEXT (word 0 bit 31) above the base format
// (word 1 28..24), so 0x0c is U8U8U8U8 and 0x98 U8U8U8, and swizzle (word 3 30..28). Rows are width
// texels of the format apart.
void sgxd_recipe_texture_words_format(uint32_t va, uint32_t width, uint32_t height, int bilinear,
                                      unsigned code, unsigned swizzle, uint32_t words[4]);
static inline uint32_t sgxd_recipe_sa_colour_offset(uint32_t offset, unsigned draw) {
    return offset + 0x40 * (draw + 1) + 0x30;
}

// What a render changes, set back: the page table's free list and, unless the target is
// external, FB and DEPTH (to the sentinel). The region headers are the TE's to write at each TA,
// and the tail pointer caches and EXTZ stay as sgxd_recipe_write zeroed them.
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
// The target's pack mode, PBE word 0 (the end-of-tile program's first LIMM), as
// sgxd_recipe_set_target rewrites the others: A8B8G8R8 as written, or three bytes a pixel. gxm's
// colour formats are these words: U888 is U8U8U8_BGR, R first (the hardware's store), and U888_RGB,
// SWIZ 1 (bit 20), is U8U8U8_RGB, B first. The stride stays in pixels.
#define SGXD_RECIPE_PACK_U8888 0x00010000u
#define SGXD_RECIPE_PACK_U888 0x10010000u
#define SGXD_RECIPE_PACK_U888_RGB 0x10110000u
// U8888 SWIZ 2, stored 0xRRGGBBAA: the PICA's RGBA8 (bytes A, B, G, R) from an A8B8G8R8 source.
#define SGXD_RECIPE_PACK_U8888_RGBA 0x00210000u
// R5G6B5 SWIZ 1, conventional (red 0xf800): the PICA's RGB565.
#define SGXD_RECIPE_PACK_R5G6B5 0x30110000u
// A4R4G4B4: A in bits 15:12, R in 3:0 (red 0xf00f). A source read with its lanes mirrored
// (A, B, G, R) gives the PICA's RGBA4, R in 15:12.
#define SGXD_RECIPE_PACK_A4R4G4B4 0x50010000u
// A1R5G5B5 SWIZ 2: R in bits 15:11, G 10:6, B 5:1, A 0, the PICA's RGB5A1 (SWIZ 0 stores A B G R
// from bit 15, 1 A R G B, 3 B G R A; channels truncated, alpha its top bit).
#define SGXD_RECIPE_PACK_A1R5G5B5_RGBA 0x40210000u
// A4R4G4B4 SWIZ 2: R in bits 15:12, A in 3:0, the PICA's RGBA4 from an A8B8G8R8 source.
#define SGXD_RECIPE_PACK_A4R4G4B4_RGBA 0x50210000u
// F32F32 SWIZ 1: a two-word pixel (DOUBLE_PIXEL_PARTITIONS 1, sgxd_recipe_set_double_pixels)
// stored word 1 first.
#define SGXD_RECIPE_PACK_F32F32_SWAPPED 0x11110000u
// EXTMODE1 U8 SWIZ 1: one byte a pixel, the colour's A.
#define SGXD_RECIPE_PACK_U8_A 0x80910000u
// The output word's low 16 bits stored raw, two bytes a pixel (the F16 pack's code).
#define SGXD_RECIPE_PACK_U16_RAW 0xf0010000u
void sgxd_recipe_set_target_pack(SgxdRecipe *r, void *memory, uint32_t pbe0);

// Route 1: a pass renders the guest's picture
// transposed, the recipe's x a guest row and its y a guest column, and its end of tile stores
// each tile into the guest's colour buffer as the PICA lays it out: 8x8 tiles row after row,
// Morton order inside, x first. The tile's eight 8x16 strips are TWIDDLED emits 16 rows high,
// each two PICA tiles, their words computed from IR0 (the tile's origin in 16 pixels, X in bits
// 0-7 and Y in 8-15), which the pixel event program DOUTAs into the task's pa0. The PBE takes
// a tile's emit words once for its four tasks, so every task emits all eight strips and the
// clips keep each one's own. Strips past the target (its width a multiple of 8, its height of
// 16, neither of 32) get an empty clip.
typedef struct SgxdPicaStore {
    uint32_t buffer;   // the guest colour buffer's GPU address
    uint32_t pack;     // its format: PBE word 0's pack and swizzle (SGXD_RECIPE_PACK_*)
    uint32_t bpp;      // its bytes a pixel: 2, 3 or 4
} SgxdPicaStore;
#define SGXD_RECIPE_PICA_EOT_MAX 128u
// The end-of-tile program storing r's target into s's buffer: up to SGXD_RECIPE_PICA_EOT_MAX
// instructions into out, temporaries r0..r15. Returns the count, or 0 for a target route 1 does
// not take (width a multiple of 8 up to 1024, height of 16 from 32 to 1024).
unsigned sgxd_recipe_pica_eot(const SgxdRecipe *r, const SgxdPicaStore *s, uint64_t *out);
// The pixel event program for it: the recipe's, with IR0 DOUTAed into the end-of-tile task's pa0
// first (three times: a DOUTA is repeated), and the end of tile at eot_index (DOUTU's
// instruction index from code base 1) with 16 temporaries. Four data units; the 3D kick takes it
// through sgxd_recipe_set_pixel_event(r, va, 4, 4).
#define SGXD_RECIPE_PICA_PIXEV_WORDS 32u
void sgxd_recipe_pica_pixev(const SgxdRecipe *r, uint32_t eot_index,
                            uint32_t out[SGXD_RECIPE_PICA_PIXEV_WORDS]);
// A target of width x height (1 to the layout's size) drawn with the recipe's layout: the tiling
// and everything that follows from it, the end of tile's PBE words 4 and 5 and the background
// record; EXTZ and the tail pointer caches are left as they are. The terminate state
// carries the tiling too, and the PDS keeps a program it has run by its address, so the caller
// places sgxd_recipe_term_pds's program elsewhere and names it with sgxd_recipe_set_term before
// the draws; set_draws refuses (-2) until then when the size is not the layout's. -1: too big.
int sgxd_recipe_set_size(SgxdRecipe *r, void *memory, uint32_t width, uint32_t height);
// The terminate state's PDS program for the recipe's current size: its words (at most
// SGXD_RECIPE_TERM_WORDS) into out, its data units into *units; returns the words.
#define SGXD_RECIPE_TERM_WORDS 32u
unsigned sgxd_recipe_term_pds(const SgxdRecipe *r, uint32_t *out, uint32_t *units);
// The VDM stream's terminate state from the PDS program at va with units (0: the layout's own),
// for the next sgxd_recipe_set_draws.
void sgxd_recipe_set_term(SgxdRecipe *r, uint32_t va, uint32_t units);
// The 3D kick's DOUBLE_PIXEL_PARTITIONS (0x0148): 1 gives each pixel two output words, one 64-bit
// o register; 0 at init.
void sgxd_recipe_set_double_pixels(SgxdRecipe *r, int on);

// The pixel event program the 3D kick names (EVENT_PIXEL_PDS_EXEC, _DATA's units and _INFO's
// ATTRIBUTE_SIZE): as init sets it, the layout's PIXEV, 3 units and 2.
void sgxd_recipe_set_pixel_event(SgxdRecipe *r, uint32_t va, uint32_t units,
                                 uint32_t attribute_size);

// The fields a kick's list carries that change from one render on a recipe to the next: a side
// render's size (sgxd_recipe_set_size), the double pixels, a pass's background and depth store.
// A list built with `kept` set records where each lands, so a list kept for the recipe takes
// another render's values in place (sgxd_session's kept lists).
enum {
    SGXD_KEPT_MTILE1,
    SGXD_KEPT_MTILE2,
    SGXD_KEPT_TE_SCREEN,
    SGXD_KEPT_STRIDE,
    SGXD_KEPT_MTE_SCREEN,
    SGXD_KEPT_RGN_SIZE,
    SGXD_KEPT_BGOBJDEPTH,
    SGXD_KEPT_BGOBJ,
    SGXD_KEPT_BGOBJTAG,
    SGXD_KEPT_DOUBLE_PIXELS,
    SGXD_KEPT_ZLS0, // the five ZLS words
    SGXD_KEPT_COUNT = SGXD_KEPT_ZLS0 + 5,
};
typedef struct SgxdKept {
    uint16_t op, field;
} SgxdKept;
uint32_t sgxd_recipe_kept_value(const SgxdRecipe *r, unsigned field);

typedef struct SgxdOpList {
    SgxdOp *ops;
    unsigned count, max;
    int overflow;
    SgxdKept *kept;
    unsigned nkept, max_kept;
} SgxdOpList;

// The TA: the code bases, the cache invalidates, the DPM free-list load, TE and MTE, the scene
// reset, then VDM_START last (T0.4-T4).
void sgxd_recipe_ta_kick(const SgxdRecipe *r, SgxdOpList *list);
// TA_FINISHED in core 0's EVENT_STATUS (T5.1).
void sgxd_recipe_ta_wait(SgxdOpList *list, uint32_t timeout_us);
// The TA's end: the cores idle, the TPC flush, the SLC flush and invalidate (T5.2-T5.8; with
// sgxd_recipe_events_owned the vertex data master's flush alone).
void sgxd_recipe_ta_done(const SgxdRecipe *r, SgxdOpList *list);
// The 3D: the DPM 3D free-list load, the code-cache invalidate, ISP, PBE and the pixel event,
// then ISP_START_RENDER last (D1, D2).
void sgxd_recipe_3d_kick(const SgxdRecipe *r, SgxdOpList *list);
// PIXELBE_END_RENDER in core 0's EVENT_STATUS (D3.1).
void sgxd_recipe_3d_wait(SgxdOpList *list, uint32_t timeout_us);
// The 3D's end: the cores idle, the event clears, the SLC flush and invalidate (D3.2-D3.5; with
// sgxd_recipe_events_owned the pixel data master's flush alone).
void sgxd_recipe_3d_done(const SgxdRecipe *r, SgxdOpList *list);

// The SLC flush of everything, and with inval the invalidate of everything (T0.1, T0.2).
void sgxd_recipe_slc(SgxdOpList *list, int inval);

#ifdef __cplusplus
}
#endif

#endif
