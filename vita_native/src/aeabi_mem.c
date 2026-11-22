// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// The EABI's memory helpers, which clang calls for the copies and fills it does not inline (gcc
// called memcpy and memset themselves). Sony's kernel library exports memcpy, memmove and memset,
// not these. Built with -fno-builtin (CMakeLists.txt): otherwise each call below would be turned
// back into a call to the helper it is in.

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);

void __aeabi_memcpy(void *dst, const void *src, size_t n) { memcpy(dst, src, n); }
void __aeabi_memcpy4(void *dst, const void *src, size_t n) { memcpy(dst, src, n); }
void __aeabi_memcpy8(void *dst, const void *src, size_t n) { memcpy(dst, src, n); }
void __aeabi_memmove(void *dst, const void *src, size_t n) { memmove(dst, src, n); }
void __aeabi_memmove4(void *dst, const void *src, size_t n) { memmove(dst, src, n); }
void __aeabi_memmove8(void *dst, const void *src, size_t n) { memmove(dst, src, n); }
// The EABI's memset takes the count before the value.
void __aeabi_memset(void *dst, size_t n, int c) { memset(dst, c, n); }
void __aeabi_memset4(void *dst, size_t n, int c) { memset(dst, c, n); }
void __aeabi_memset8(void *dst, size_t n, int c) { memset(dst, c, n); }
void __aeabi_memclr(void *dst, size_t n) { memset(dst, 0, n); }
void __aeabi_memclr4(void *dst, size_t n) { memset(dst, 0, n); }
void __aeabi_memclr8(void *dst, size_t n) { memset(dst, 0, n); }
