// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// The little of libc zstd needs in wasm: the mem* functions and an allocator.
//
// The allocator keeps one free list sorted by address and merges neighbours on free. zstd
// allocates its context once and reuses it frame after frame, and the page allocates its
// buffers once per job size, so a first fit is enough.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void* memcpy(void* dst, const void* src, size_t n) {
    __builtin_memcpy(dst, src, n); // memory.copy with bulk memory
    return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
    __builtin_memmove(dst, src, n);
    return dst;
}

void* memset(void* dst, int c, size_t n) {
    __builtin_memset(dst, c, n);
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* x = a;
    const unsigned char* y = b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return x[i] < y[i] ? -1 : 1;
        }
    }
    return 0;
}

#define ALIGN 16
#define PAGE 65536

typedef struct Block {
    size_t size; // whole block, header included
    struct Block* next;
} Block;

#define HEADER ((sizeof(Block) + ALIGN - 1) & ~(size_t)(ALIGN - 1))

extern unsigned char __heap_base;

static uintptr_t heap_top;
static Block* free_list;

static Block* grow(size_t size) {
    if (heap_top == 0) {
        heap_top = ((uintptr_t)&__heap_base + ALIGN - 1) & ~(uintptr_t)(ALIGN - 1);
    }
    const uintptr_t end = heap_top + size;
    const uintptr_t have = __builtin_wasm_memory_size(0) * (uintptr_t)PAGE;
    if (end > have) {
        const uintptr_t pages = (end - have + PAGE - 1) / PAGE;
        if (__builtin_wasm_memory_grow(0, pages) == (size_t)-1) {
            return NULL;
        }
    }
    Block* b = (Block*)heap_top;
    b->size = size;
    heap_top = end;
    return b;
}

void* malloc(size_t size) {
    if (size == 0) {
        size = 1;
    }
    if (size > SIZE_MAX / 2) {
        return NULL;
    }
    const size_t need = HEADER + ((size + ALIGN - 1) & ~(size_t)(ALIGN - 1));
    Block** link = &free_list;
    for (Block* b = free_list; b; link = &b->next, b = b->next) {
        if (b->size < need) {
            continue;
        }
        if (b->size - need >= HEADER + ALIGN) {
            Block* rest = (Block*)((unsigned char*)b + need);
            rest->size = b->size - need;
            rest->next = b->next;
            *link = rest;
            b->size = need;
        } else {
            *link = b->next;
        }
        return (unsigned char*)b + HEADER;
    }
    Block* b = grow(need);
    return b ? (unsigned char*)b + HEADER : NULL;
}

void free(void* ptr) {
    if (!ptr) {
        return;
    }
    Block* b = (Block*)((unsigned char*)ptr - HEADER);
    Block* prev = NULL;
    Block* next = free_list;
    while (next && next < b) {
        prev = next;
        next = next->next;
    }
    b->next = next;
    if (next && (unsigned char*)b + b->size == (unsigned char*)next) {
        b->size += next->size;
        b->next = next->next;
    }
    if (prev && (unsigned char*)prev + prev->size == (unsigned char*)b) {
        prev->size += b->size;
        prev->next = b->next;
    } else if (prev) {
        prev->next = b;
    } else {
        free_list = b;
    }
}

void* calloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) {
        return NULL;
    }
    void* p = malloc(count * size);
    if (p) {
        memset(p, 0, count * size);
    }
    return p;
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) {
        return malloc(size);
    }
    Block* b = (Block*)((unsigned char*)ptr - HEADER);
    const size_t have = b->size - HEADER;
    if (size <= have) {
        return ptr;
    }
    void* p = malloc(size);
    if (p) {
        memcpy(p, ptr, have);
        free(ptr);
    }
    return p;
}
