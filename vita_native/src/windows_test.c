// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// windows_test: host test of windows.h against a page-by-page model. Build and run:
//
//     cc -std=c99 -Wall -Wextra -Werror windows_test.c -o windows_test && ./windows_test

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "windows.h"

enum { PAGES = 512, NONE = 0xffffffffu, MAX_CAP = 64, STEPS = 200000 };

static uint32_t model[PAGES];       // the app page each page shows, or NONE
static AzaharWindow storage[MAX_CAP];
static AzaharWindows list = {storage, 0, MAX_CAP};
static unsigned long rng = 12345;

static uint32_t next(uint32_t n) {
    rng = rng * 6364136223846793005ul + 1442695040888963407ul;
    return (uint32_t)(rng >> 33) % n;
}

static void fail(const char *what, unsigned step) {
    printf("step %u: %s\n", step, what);
    for (unsigned i = 0; i < list.count; i++) {
        printf("  window %u: va %#x uva %#x size %#x\n", i, list.w[i].va, list.w[i].uva,
               list.w[i].size);
    }
    exit(1);
}

// What the list shows, page by page.
static void shown(uint32_t *out) {
    for (unsigned p = 0; p < PAGES; p++) {
        out[p] = NONE;
    }
    for (unsigned i = 0; i < list.count; i++) {
        for (uint32_t off = 0; off < list.w[i].size; off += 0x1000) {
            out[(list.w[i].va + off) / 0x1000] = list.w[i].uva + off;
        }
    }
}

static void check(unsigned step) {
    for (unsigned i = 0; i < list.count; i++) {
        const AzaharWindow *w = &list.w[i];
        if (!w->size || (w->va | w->uva | w->size) & 0xfffu ||
            (uint64_t)w->va + w->size > (uint64_t)PAGES * 0x1000) {
            fail("a window is empty, unaligned or out of range", step);
        }
        if (i > 0) {
            const AzaharWindow *v = &list.w[i - 1];
            if ((uint64_t)v->va + v->size > w->va) {
                fail("windows overlap or are out of order", step);
            }
            if (v->va + v->size == w->va && v->uva + v->size == w->uva) {
                fail("adjoining windows were not joined", step);
            }
        }
    }
    uint32_t got[PAGES];
    shown(got);
    if (memcmp(got, model, sizeof(got)) != 0) {
        fail("the list does not show what the model does", step);
    }
}

static void model_remove(uint32_t *m, uint32_t va, uint32_t size) {
    for (uint32_t off = 0; off < size; off += 0x1000) {
        m[(va + off) / 0x1000] = NONE;
    }
}

// STEPS random edits with a list of `cap` windows.
static void run(unsigned cap) {
    for (unsigned p = 0; p < PAGES; p++) {
        model[p] = NONE;
    }
    list.count = 0;
    list.cap = cap;
    unsigned adds = 0, removes = 0, full = 0, joins = 0;
    for (unsigned step = 0; step < STEPS; step++) {
        // Mostly short ranges, so windows join and cut each other often.
        const uint32_t first = next(PAGES);
        const uint32_t room = PAGES - first;
        const uint32_t pages = 1 + next(next(4) == 0 ? room : (room < 8 ? room : 8));
        const uint32_t va = first * 0x1000, size = pages * 0x1000;
        uint32_t before[PAGES];
        memcpy(before, model, sizeof(model));
        if (next(3) != 0) {
            // App addresses from a few bases, so neighbours often continue each other.
            const uint32_t uva = 0x10000000u * (1 + next(3)) + va + 0x1000 * next(2);
            const unsigned count = list.count;
            const int r = windows_add(&list, va, uva, size);
            if (r == 0) {
                for (uint32_t off = 0; off < size; off += 0x1000) {
                    model[(va + off) / 0x1000] = uva + off;
                }
                adds++;
                joins += list.count <= count;
            } else if (r == AZAHAR_ERR_ALLOC) {
                // Full: either nothing changed (no room to cut a window), or the range was
                // removed and only the new window found no room.
                uint32_t got[PAGES];
                shown(got);
                model_remove(model, va, size);
                if (memcmp(got, before, sizeof(got)) == 0) {
                    memcpy(model, before, sizeof(model));
                } else if (list.count != list.cap) {
                    fail("an add failed with room left", step);
                }
                full++;
            } else {
                fail("an add returned an unknown error", step);
            }
        } else {
            const int r = windows_remove(&list, va, size);
            if (r == 0) {
                model_remove(model, va, size);
                removes++;
            } else if (r == AZAHAR_ERR_ALLOC) {
                if (list.count != list.cap) {
                    fail("a remove failed with room left", step);
                }
                full++;
            } else {
                fail("a remove returned an unknown error", step);
            }
        }
        check(step);
    }
    printf("windows_test: %u windows, %u steps: %u adds (%u joined or replaced), %u removes, "
           "%u refused for room; every step matched the model\n",
           cap, STEPS, adds, joins, removes, full);
}

int main(void) {
    run(4);
    run(12);
    run(MAX_CAP);
    return 0;
}
