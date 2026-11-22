// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// azahar-native — windows.h: lists of mappings of the app's pages, for native.c's guest windows
// and sgxd.c's GPU ranges. Plain C with no kernel calls, so a host test can take it
// (windows_test.c).

#ifndef AZAHAR_WINDOWS_H
#define AZAHAR_WINDOWS_H

#include <stdint.h>
#include "azahar_native.h"

// A mapping of the app's pages: [va, va + size) shows the app's [uva, uva + size). Kept as lists
// so a page move (reback.c) finds the descriptors naming an app page without a table sweep. A
// list is sorted by va, its windows do not overlap, and windows that adjoin in both va and uva
// are one.
typedef struct AzaharWindow {
    uint32_t va, uva, size;
} AzaharWindow;

typedef struct AzaharWindows {
    AzaharWindow *w;
    unsigned count, cap;
} AzaharWindows;

static void windows_open_gap(AzaharWindows *l, unsigned at) {
    for (unsigned i = l->count; i > at; i--) {
        l->w[i] = l->w[i - 1];
    }
    l->count++;
}

static void windows_close_gap(AzaharWindows *l, unsigned at) {
    for (unsigned i = at; i + 1 < l->count; i++) {
        l->w[i] = l->w[i + 1];
    }
    l->count--;
}

// [va, va + size) no longer shows anything. AZAHAR_ERR_ALLOC, and nothing changed, when it would
// cut a window in two and the list is full.
static int windows_remove(AzaharWindows *l, uint32_t va, uint32_t size) {
    const uint64_t a = va, b = (uint64_t)va + size;
    for (unsigned i = 0; i < l->count; i++) {
        const uint64_t s = l->w[i].va, e = s + l->w[i].size;
        if (s < a && e > b) {
            // The windows do not overlap: this is the only one [a, b) touches.
            if (l->count == l->cap) {
                return AZAHAR_ERR_ALLOC;
            }
            windows_open_gap(l, i + 1);
            l->w[i + 1].va = (uint32_t)b;
            l->w[i + 1].uva = l->w[i].uva + (uint32_t)(b - s);
            l->w[i + 1].size = (uint32_t)(e - b);
            l->w[i].size = (uint32_t)(a - s);
            return 0;
        }
    }
    unsigned out = 0;
    for (unsigned i = 0; i < l->count; i++) {
        AzaharWindow w = l->w[i];
        const uint64_t s = w.va, e = s + w.size;
        if (e > a && s < b) {
            if (s < a) {
                w.size = (uint32_t)(a - s);         // its head stays
            } else if (e > b) {
                w.uva += (uint32_t)(b - s);         // its tail stays
                w.va = (uint32_t)b;
                w.size = (uint32_t)(e - b);
            } else {
                continue;                           // covered whole
            }
        }
        l->w[out++] = w;
    }
    l->count = out;
    return 0;
}

// [va, va + size) shows the app's [uva, uva + size), replacing what it showed.
// AZAHAR_ERR_ALLOC when the list is full.
static int windows_add(AzaharWindows *l, uint32_t va, uint32_t uva, uint32_t size) {
    const int r = windows_remove(l, va, size);
    if (r) {
        return r;
    }
    unsigned i = 0;
    while (i < l->count && l->w[i].va < va) {
        i++;
    }
    const int join_prev = i > 0 && (uint64_t)l->w[i - 1].va + l->w[i - 1].size == va &&
                          (uint64_t)l->w[i - 1].uva + l->w[i - 1].size == uva;
    const int join_next = i < l->count && (uint64_t)va + size == l->w[i].va &&
                          (uint64_t)uva + size == l->w[i].uva;
    if (join_prev) {
        l->w[i - 1].size += size;
        if (join_next) {
            l->w[i - 1].size += l->w[i].size;
            windows_close_gap(l, i);
        }
        return 0;
    }
    if (join_next) {
        l->w[i].va = va;
        l->w[i].uva = uva;
        l->w[i].size += size;
        return 0;
    }
    if (l->count == l->cap) {
        return AZAHAR_ERR_ALLOC;
    }
    windows_open_gap(l, i);
    l->w[i].va = va;
    l->w[i].uva = uva;
    l->w[i].size = size;
    return 0;
}

#endif
