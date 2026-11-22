// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// sgxd_program: the PDS programs and size words around a caller's USSE programs: the work
// libgxm's shader patcher does for a GXM program (docs/sgxd/pds.md). Plain C99 and no Vita
// headers, so the host build and tests take it too.

#ifndef SGXD_PROGRAM_H
#define SGXD_PROGRAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A PDS program's words: data units of four words, then the code. The builders write at most
// this many.
#define SGXD_PROGRAM_MAX_PDS 160u   // 32 data units (64 words a segment) and 32 instructions
// Attributes a vertex fetch takes (the PICA has 12), and distinct strides among them.
#define SGXD_PROGRAM_MAX_FETCH 12u
#define SGXD_PROGRAM_MAX_STRIDES 8u

// The VDM index list's primitive types (bits 26-28).
#define SGXD_PROGRAM_TRIANGLES 0u
#define SGXD_PROGRAM_STRIP 3u
#define SGXD_PROGRAM_FAN 4u

// One attribute's fetch: words from address + index * stride into pa. The vertex PDS multiplies
// the index's low 16 bits by a 16-bit stride.
typedef struct SgxdVertexFetch {
    uint32_t address;
    uint32_t stride;
    uint32_t words;   // 1 to 256
    uint32_t pa;
} SgxdVertexFetch;

typedef struct SgxdPdsProgram {
    uint32_t words[SGXD_PROGRAM_MAX_PDS];
    unsigned count;   // words to write, 16-byte aligned in GPU memory
    unsigned units;   // its data units, for the pointer words
} SgxdPdsProgram;

// DOUTU word 0: the program at code_offset bytes (8-aligned) from code base cb (0 vertex, 1
// pixel), with temps temporaries (the DOUTU counts them in fours).
uint32_t sgxd_program_doutu0(unsigned cb, uint32_t code_offset, unsigned temps);

// The vertex primary: the fetches, then the program on the batch's last vertex (IF0). DOUTU1's
// mode is PERINSTANCE for a program that needs one vertex per instance. Returns 0, or -1 for
// more fetches or strides than it takes, or a stride of 64 KiB or more.
int sgxd_program_vertex_pds(const SgxdVertexFetch *fetch, unsigned count, uint32_t doutu0,
                            int per_instance, SgxdPdsProgram *out);
// The same, with address_at[i] the index in out->words of fetch i's address: the program for
// other addresses is these words with those changed.
int sgxd_program_vertex_pds_at(const SgxdVertexFetch *fetch, unsigned count, uint32_t doutu0,
                               int per_instance, SgxdPdsProgram *out, unsigned *address_at);

// A secondary: sa_words words from sa_address into sa0 (none for 0), then the USSE program of
// doutu0 (a dummy, or the program's secondary). -1 for more than 256 words.
int sgxd_program_secondary_pds(uint32_t sa_address, unsigned sa_words, uint32_t doutu0,
                               SgxdPdsProgram *out);
// The same, with *address_at the index in out->words of sa_address (with sa_words nonzero).
int sgxd_program_secondary_pds_at(uint32_t sa_address, unsigned sa_words, uint32_t doutu0,
                                  SgxdPdsProgram *out, unsigned *address_at);

// The pixel primary: DOUTU with SDSOFT (and ITERATORSDEPENDENCY with iterators), then one DOUTI
// per iterator in order, which fills the PAs from pa0: iterator[i] is the GXP varying
// descriptor's word (DOUTI word 1: its USEISSUE, dimensions, flat, last issue), words[i] the
// registers it writes. -1 for more than 12 iterators.
int sgxd_program_pixel_pds(uint32_t doutu0, const uint32_t *iterators, const unsigned *words,
                           unsigned count, SgxdPdsProgram *out);

// A texture read the pixel primary issues (a non-dependent read): the unit's four state words
// (DOUTT0-3), its coordinate set TCn, and the iterator whose DOUTI it follows.
typedef struct SgxdTextureIssue {
    uint32_t tex[4];
    unsigned coord;
    unsigned after;
    unsigned f16;   // the texel as four F16 (FCONV 2, two words), not four F32 (FCONV 3, four)
    unsigned raw;   // the texel's packed word as stored (FCONV 0, one word), filtered bytewise
    unsigned proj_t;   // TEXPROJ T: the coordinate divided by its set's fourth lane (projected)
} SgxdTextureIssue;

// sgxd_program_pixel_pds with texture issues: after iterator `after`'s DOUTI, each issue's own
// DOUTI (TEXISSUE its TCn, no USE issue, as the merge's TC0 and TC1; TEXLASTISSUE on the last;
// TAGSIZE four words) and DOUTT of its words converted to F32 (FCONV 3), so its texel lands as
// four F32 words at the PA the DOUTIs before it leave (issues fill the PAs in PDS order,
// docs/sgxd/pds.md). DOUTU TEXTUREDEPENDENCY with any issue. -1 for more than 12
// iterators or 7 issues (three units, their split alphas and the fog table).
int sgxd_program_pica_pixel_pds(uint32_t doutu0, const uint32_t *iterators, const unsigned *words,
                                unsigned count, const SgxdTextureIssue *issues,
                                unsigned issue_count, SgxdPdsProgram *out);

// The display transfer's blit (docs/sgxd/ptla.md): a textured quad, exact. A vertex
// is ten words, the clip-space position, a colour and the texture coordinate; the vertex program
// copies them to o0..o9 (OUTSELECTS: the position, the base colour and TC0; TEXSIZE 1), the pixel
// primary issues the colour into pa0..pa3, then the texture at TC0, whose texel lands in pa4, and
// the fragment program writes pa4 to o0. The builders return the words written (at most 4).
#define SGXD_PROGRAM_BLIT_VERTEX_WORDS 10u
#define SGXD_PROGRAM_BLIT_OUTSELECTS (10u << 24 | 0x1000u | 0x800u)
#define SGXD_PROGRAM_BLIT_TEXSIZE 1u
#define SGXD_PROGRAM_BLIT_PA 6u
unsigned sgxd_program_blit_vertex(uint64_t words[4]);
unsigned sgxd_program_blit_fragment(uint64_t words[4]);
// The blit's pixel primary: DOUTU of doutu0 (SDSOFT, ITERATORSDEPENDENCY, TEXTUREDEPENDENCY),
// DOUTI of the colour, DOUTI of the texture issue, DOUTT of tex unconverted.
int sgxd_program_blit_pixel_pds(uint32_t doutu0, const uint32_t tex[4], SgxdPdsProgram *out);

// A guest screen on the display: the blit's vertices and vertex program, but the texel
// converted, so any format the texture unit reads comes back the same way: the half4 query
// (DOUTT FCONV F16, TAGSIZE 2 words) into pa4..pa5, packed to o0 (pack.u8.f16.scl o0, pa4).
// The PA count stays SGXD_PROGRAM_BLIT_PA.
unsigned sgxd_program_screen_fragment(uint64_t words[4]);
// The same blended over the display (a screen overlaid translucent): the colour iterator (white,
// its alpha the screen's opacity) and the texel (its alpha one by the texture's swizzle) as bytes,
// their product, and that over o0 by its alpha (sgxd_recipe.c's SGXD_IMGUI_MODULATE words). The
// draw's ISP pass type must be translucent. Six words.
unsigned sgxd_program_screen_blend_fragment(uint64_t words[6]);
int sgxd_program_screen_pixel_pds(uint32_t doutu0, const uint32_t tex[4], SgxdPdsProgram *out);

// A 64-bit texel copied raw (azahar's ETC1 untile: an 8-byte block a pixel): the texture issued
// with no conversion into pa0 and pa1, then the colour iterator (pa2-pa5), whose USELASTISSUE
// starts the pixel task; sgxd_program_raw64_fragment moves both words to the pixel's output, for
// a render with DOUBLE_PIXEL_PARTITIONS 1 into a 64-bit target.
#define SGXD_PROGRAM_RAW64_PA 2u
int sgxd_program_raw64_pixel_pds(uint32_t doutu0, const uint32_t tex[4], SgxdPdsProgram *out);
// vmov.f32 o0.xy, pa0.xy (END).
unsigned sgxd_program_raw64_fragment(uint64_t words[2]);

// Two textures joined per pixel (azahar's ETC1A4 untile): the blit's vertex with a second
// texture coordinate after the first, twelve words (the position, a colour, TC0, TC1;
// OUTSELECTS VTXSIZE 12, TEXSIZE TC0 and TC1 two-dimensional), and a pixel primary that reads
// both textures unconverted, tex0's texel into pa4 and tex1's into pa5: per texture its DOUTI
// issue (TC0, then TC1 as the last issue) and its DOUTT. The fragment program
// is the caller's. The PA count stays SGXD_PROGRAM_BLIT_PA.
#define SGXD_PROGRAM_MERGE_VERTEX_WORDS 12u
#define SGXD_PROGRAM_MERGE_OUTSELECTS (12u << 24 | 0x1000u | 0x800u)
#define SGXD_PROGRAM_MERGE_TEXSIZE (1u | 1u << 3)
unsigned sgxd_program_merge_vertex(uint64_t words[4]);
int sgxd_program_merge_pixel_pds(uint32_t doutu0, const uint32_t tex0[4], const uint32_t tex1[4],
                                 SgxdPdsProgram *out);

// State word 8 and the VDM index list's words 2, 4 and 5 (libgxm's CalculatePixelDMSInfo and
// vertex DMS).
uint32_t sgxd_program_pixel_dms(unsigned pa, unsigned temps, unsigned sa);
void sgxd_program_vertex_dms(unsigned pa, unsigned temps, unsigned sa, unsigned out,
                             uint32_t *w2, uint32_t *w4, uint32_t *w5);

#ifdef __cplusplus
}
#endif

#endif
