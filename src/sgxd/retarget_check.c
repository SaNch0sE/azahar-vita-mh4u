// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// retarget_check — host test of sgxd_recipe_set_size: a recipe made at 1024x1024 and set to a
// smaller size against a recipe made at that size. Their tiling, PBE words 4 and 5, TA and 3D
// kick lists (register by register; a value may differ only where both are addresses in their
// own recipe's block), terminate program, end of tile and background record must agree. Build
// and run:
//
//     cc -std=c99 -Wall -Wextra -Werror -Wno-missing-field-initializers -I .
//         -I ../../vita_native/src sgxd_recipe.c retarget_check.c -o retarget_check
//     ./retarget_check

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sgxd_recipe.h"

enum { INDICES = 96, DRAWS = 8, LAYOUT = 1024 };

static uint16_t indices[INDICES];
static SgxdRecipeDraw draws[DRAWS];
static int failures;

static void fail(const char *what, uint32_t w, uint32_t h) {
    printf("%ux%u: %s differs\n", w, h, what);
    failures++;
}

static int made(SgxdRecipe *r, uint8_t **memory, uint32_t w, uint32_t h, uint32_t base) {
    if (sgxd_recipe_init_programs(r, w, h, base, 418, indices, INDICES, draws, DRAWS) < 0) {
        return 0;
    }
    *memory = calloc(1, r->offset[SGXD_R_END]);
    sgxd_recipe_write(r, *memory);
    return 1;
}

static int in_block(const SgxdRecipe *r, uint32_t value) {
    return value >= r->base && value < r->base + r->offset[SGXD_R_END];
}

static void compare_ops(const char *what, void (*build)(const SgxdRecipe *, SgxdOpList *),
                        const SgxdRecipe *a, const SgxdRecipe *b, uint32_t w, uint32_t h) {
    static SgxdOp ops_a[512], ops_b[512];
    SgxdOpList la = {ops_a, 0, 512, 0}, lb = {ops_b, 0, 512, 0};
    build(a, &la);
    build(b, &lb);
    if (la.overflow || lb.overflow || la.count != lb.count) {
        fail(what, w, h);
        return;
    }
    for (unsigned n = 0; n < la.count; n++) {
        const SgxdOp *x = &ops_a[n], *y = &ops_b[n];
        if (x->op != y->op || x->reg != y->reg || x->mask != y->mask) {
            fail(what, w, h);
            return;
        }
        // The USE code bases (va >> 6) and the background object's tag (BGREC from the
        // requestor base, >> 4) are addresses in each recipe's own layout too.
        const int layout_address = x->reg == 0x0a0c || x->reg == 0x0a10 || x->reg == 0x04c4;
        if (x->value != y->value && !layout_address &&
            !(in_block(a, x->value) && in_block(b, y->value))) {
            printf("%ux%u: %s op %u reg %05x: %08x set, %08x made\n", w, h, what, n, x->reg,
                   x->value, y->value);
            failures++;
        }
    }
}

static void check(const SgxdRecipe *layout, const uint8_t *layout_memory, uint32_t w, uint32_t h) {
    SgxdRecipe a = *layout;
    uint8_t *memory_a = malloc(layout->offset[SGXD_R_END]);
    memcpy(memory_a, layout_memory, layout->offset[SGXD_R_END]);
    if (sgxd_recipe_set_size(&a, memory_a, w, h) < 0) {
        fail("set_size", w, h);
        free(memory_a);
        return;
    }
    SgxdRecipe b;
    uint8_t *memory_b;
    if (!made(&b, &memory_b, w, h, 0x02000000)) {
        fail("init", w, h);
        free(memory_a);
        return;
    }
    const uint32_t fields_a[] = {a.tx, a.ty, a.mx, a.my, a.count, a.mtile1, a.mtile2, a.stride,
                                 a.te_screen, a.mte_screen, a.rgn_size, a.tpc_size, a.pbe[4],
                                 a.pbe[5], a.width, a.height};
    const uint32_t fields_b[] = {b.tx, b.ty, b.mx, b.my, b.count, b.mtile1, b.mtile2, b.stride,
                                 b.te_screen, b.mte_screen, b.rgn_size, b.tpc_size, b.pbe[4],
                                 b.pbe[5], b.width, b.height};
    if (memcmp(fields_a, fields_b, sizeof(fields_a)) != 0) {
        fail("tiling or PBE words", w, h);
    }
    compare_ops("TA kick", sgxd_recipe_ta_kick, &a, &b, w, h);
    compare_ops("3D kick", sgxd_recipe_3d_kick, &a, &b, w, h);

    // The terminate program: the same for both, and the one b's layout holds.
    uint32_t term_a[SGXD_RECIPE_TERM_WORDS], term_b[SGXD_RECIPE_TERM_WORDS], units_a, units_b;
    const unsigned na = sgxd_recipe_term_pds(&a, term_a, &units_a);
    const unsigned nb = sgxd_recipe_term_pds(&b, term_b, &units_b);
    if (!na || na != nb || units_a != units_b || memcmp(term_a, term_b, na * 4) != 0 ||
        units_b != b.units[SGXD_R_TERM] ||
        memcmp(term_b, memory_b + b.offset[SGXD_R_TERM], nb * 4) != 0) {
        fail("terminate program", w, h);
    }
    // The end of tile's PBE words (LIMMs 4 and 5) and the background record's corners.
    const uint64_t *eot_a = (const uint64_t *)(memory_a + a.offset[SGXD_R_CF] + 0x100);
    const uint64_t *eot_b = (const uint64_t *)(memory_b + b.offset[SGXD_R_CF] + 0x100);
    if (eot_a[5] != eot_b[5] || eot_a[6] != eot_b[6]) {
        fail("end of tile", w, h);
    }
    const uint32_t *bg_a = (const uint32_t *)(memory_a + a.offset[SGXD_R_BGREC]);
    const uint32_t *bg_b = (const uint32_t *)(memory_b + b.offset[SGXD_R_BGREC]);
    if (memcmp(bg_a + 3, bg_b + 3, 14 * 4) != 0) {
        fail("background record", w, h);
    }
    // Below the layout's size set_draws refuses until a terminate program is named.
    const int resized = w != layout->layout_width || h != layout->layout_height;
    if (sgxd_recipe_set_draws(&a, memory_a, draws, 1) != (resized ? -2 : 0)) {
        fail("set_draws before set_term", w, h);
    }
    sgxd_recipe_set_term(&a, 0x01200000, units_a);
    if (sgxd_recipe_set_draws(&a, memory_a, draws, 1) != 0) {
        fail("set_draws after set_term", w, h);
    }
    printf("%ux%u: tiling %ux%u tiles, %u x %u macrotile tiles, region %u bytes, TPC %u bytes\n",
           w, h, a.tx, a.ty, a.mx, a.my, a.rgn_size, a.tpc_size);
    free(memory_a);
    free(memory_b);
}

int main(void) {
    for (unsigned n = 0; n < INDICES; n++) {
        indices[n] = (uint16_t)n;
    }
    for (unsigned n = 0; n < DRAWS; n++) {
        draws[n] = (SgxdRecipeDraw){0, 3, SGXD_RECIPE_ISP_NEVER, SGXD_R_FPRIM, 0, 0, NULL, 0};
    }
    sgxd_recipe_grid = 4;
    sgxd_recipe_external_fb = 1;
    SgxdRecipe layout;
    uint8_t *memory;
    if (!made(&layout, &memory, LAYOUT, LAYOUT, 0x01000000)) {
        printf("the %ux%u recipe was not made\n", LAYOUT, LAYOUT);
        return 1;
    }
    static const uint32_t sizes[][2] = {
        {32, 32}, {64, 64}, {64, 128}, {128, 32}, {512, 32}, {256, 256}, {1024, 64}, {32, 1024},
        {384, 416}, {1024, 1024},
    };
    for (unsigned n = 0; n < sizeof(sizes) / sizeof(sizes[0]); n++) {
        check(&layout, memory, sizes[n][0], sizes[n][1]);
    }
    // Below the made sizes (32) set_size still takes the target; too big is refused.
    SgxdRecipe small = layout;
    if (sgxd_recipe_set_size(&small, memory, 4, 8) != 0 || small.tx != 1 || small.ty != 1 ||
        small.mte_screen != (7u << 12 | 3u)) {
        fail("set_size below 32", 4, 8);
    }
    SgxdRecipe big = layout;
    if (sgxd_recipe_set_size(&big, memory, 2048, 32) != -1) {
        fail("set_size past the layout", 2048, 32);
    }
    free(memory);
    printf("retarget_check: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
