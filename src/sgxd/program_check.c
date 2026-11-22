// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// program_check — host test of sgxd_program against the recipe: the programs the recipe runs
// (VSPDS, the ImGui VSEC, FSEC) rebuilt by sgxd_program's builders must match word for
// word. Build and run:
//
//     cc -std=c99 -Wall -Wextra -Werror -I . -I ../../vita_native/src sgxd_recipe.c
//         sgxd_program.c program_check.c -o program_check && ./program_check

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sgxd_program.h"
#include "sgxd_recipe.h"

static int failures;

static void compare(const char *what, const uint8_t *memory, const SgxdRecipe *r, unsigned part,
                    const SgxdPdsProgram *p) {
    const uint32_t *words = (const uint32_t *)(memory + r->offset[part]);
    int same = p->units == r->units[part] || part > SGXD_R_BGPRIM;
    for (unsigned n = 0; n < p->count; n++) {
        if (words[n] != p->words[n]) {
            printf("%s word %u: recipe %08x, sgxd_program %08x\n", what, n, words[n], p->words[n]);
            same = 0;
        }
    }
    printf("%s: %u words, %u units, %s\n", what, p->count, p->units, same ? "PASS" : "FAIL");
    failures += !same;
}

// The DOUTU word 0 the recipe put in a program's data: the first pair holding a DOUTU of cb.
static uint32_t kick_word(const uint8_t *memory, const SgxdRecipe *r, unsigned part, unsigned at) {
    return ((const uint32_t *)(memory + r->offset[part]))[at];
}

int main(void) {
    static uint16_t indices[64];
    static SgxdRecipeDraw draws[1] = {{0, 3, SGXD_RECIPE_ISP_A, SGXD_R_FPRIM, 0, 0, NULL}};
    static const float verts[12] = {0};
    for (unsigned n = 0; n < 64; n++) {
        indices[n] = (uint16_t)n;
    }
    SgxdRecipe r;
    if (sgxd_recipe_init_scene(&r, 64, 64, 0x01000000, 418, verts, 3, indices, 3, draws, 1) < 0) {
        return 1;
    }
    uint8_t *memory = calloc(1, r.offset[SGXD_R_END]);
    sgxd_recipe_write(&r, memory);

    SgxdPdsProgram p;
    // VSPDS: one fetch of the vertex's four words at stride 16. Its DOUTU word 0 is the second
    // DS0 pair's low word: data word 4.
    const SgxdVertexFetch fetch = {sgxd_recipe_va(&r, SGXD_R_VERTS), 16, 4, 0};
    sgxd_program_vertex_pds(&fetch, 1, kick_word(memory, &r, SGXD_R_VSPDS, 4), 0, &p);
    compare("VSPDS", memory, &r, SGXD_R_VSPDS, &p);
    // FSEC and BGSEC: no SA, the dummy program.
    sgxd_program_secondary_pds(0, 0, kick_word(memory, &r, SGXD_R_FSEC, 0), &p);
    compare("FSEC", memory, &r, SGXD_R_FSEC, &p);
    sgxd_program_secondary_pds(0, 0, kick_word(memory, &r, SGXD_R_BGSEC, 0), &p);
    compare("BGSEC", memory, &r, SGXD_R_BGSEC, &p);
    free(memory);

    // ImGui mode's VSEC: four SA words, then the dummy vertex program.
    static uint32_t imgui_verts[5 * 3];
    if (sgxd_recipe_init_imgui(&r, 64, 64, 0x01000000, 418, imgui_verts, 3, indices, 3, draws, 1,
                               0x7000000, SGXD_IMGUI_COLOUR) < 0) {
        return 1;
    }
    memory = calloc(1, r.offset[SGXD_R_END]);
    sgxd_recipe_write(&r, memory);
    sgxd_program_secondary_pds(r.base + r.vertex_sa, 4, kick_word(memory, &r, SGXD_R_VSEC, 2), &p);
    compare("ImGui VSEC", memory, &r, SGXD_R_VSEC, &p);
    free(memory);

    // The display transfer's blit against sgxd_test 08's TEXEL mode: its FPRIM (DOUTU word 0 in
    // data word 0) with a texture's words, and its fragment program at CF + 0x140.
    if (sgxd_recipe_init_imgui(&r, 64, 64, 0x01000000, 418, imgui_verts, 3, indices, 3, draws, 1,
                               0x7000000, SGXD_IMGUI_TEXEL) < 0) {
        return 1;
    }
    memory = calloc(1, r.offset[SGXD_R_END]);
    sgxd_recipe_write(&r, memory);
    uint32_t tex[4];
    sgxd_recipe_texture_words(0x48000000, 240, 400, 0, tex);
    sgxd_recipe_set_texture(&r, memory, tex);
    sgxd_program_blit_pixel_pds(kick_word(memory, &r, SGXD_R_FPRIM, 0), tex, &p);
    compare("blit FPRIM", memory, &r, SGXD_R_FPRIM, &p);
    uint64_t code[4];
    const unsigned n = sgxd_program_blit_fragment(code);
    const int same = memcmp(memory + r.offset[SGXD_R_CF] + 0x140, code, n * 8) == 0;
    printf("blit fragment program: %u words, %s\n", n, same ? "PASS" : "FAIL");
    failures += !same;

    // A guest screen's pixel primary is the blit's with the texel converted: exactly the texture
    // DOUTI's TAGSIZE (bit 6, two words) and the DOUTT's FCONV (F16, 2 << 4) differ. Its texture
    // is U8U8U8 (0x98: TEXFEXT and base format 0x18) under swizzle 1.
    SgxdPdsProgram blit, screen;
    sgxd_recipe_texture_words_format(0x2008ca00, 240, 400, 0, 0x98, 1, tex);
    const int u888 = tex[0] == (0x03fe0090u | 1u << 31) && tex[1] == 0x780ef18fu &&
                     tex[2] == 0x2008ca00u && tex[3] == 0x90000000u;
    printf("U8U8U8 texture words %08x %08x %08x %08x: %s\n", tex[0], tex[1], tex[2], tex[3],
           u888 ? "PASS" : "FAIL");
    failures += !u888;
    sgxd_program_blit_pixel_pds(0x1234, tex, &blit);
    sgxd_program_screen_pixel_pds(0x1234, tex, &screen);
    unsigned diffs = 0, expected = 0;
    for (unsigned i = 0; i < blit.count && blit.count == screen.count; i++) {
        const uint32_t x = blit.words[i] ^ screen.words[i];
        diffs += x != 0;
        expected += x == 1u << 6 || x == 2u << 4;
    }
    const int screen_ok = blit.count == screen.count && blit.units == screen.units && diffs == 2 &&
                          expected == 2;
    printf("screen FPRIM: %u words, %u differ from the blit's: %s\n", screen.count, diffs,
           screen_ok ? "PASS" : "FAIL");
    failures += !screen_ok;
    free(memory);

    printf("program_check: %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
